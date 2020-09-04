#include "gpu_backend.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "settings.h"

#include "gpu_hw_opengl.h"
#include "gpu_hw_vulkan.h"
#include "gpu_sw.h"

#ifdef WIN32
#include "gpu_hw_d3d11.h"
#endif

Log_SetChannel(GPUBackend);

std::unique_ptr<GPUBackend> g_gpu_backend;

GPUBackend::GPUBackend() = default;

GPUBackend::~GPUBackend() = default;

static std::unique_ptr<GPUBackend> CreateBackend(GPURenderer backend)
{
  switch (backend)
  {
#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      return std::make_unique<GPU_HW_D3D11>();
#endif

    case GPURenderer::HardwareOpenGL:
      return std::make_unique<GPU_HW_OpenGL>();

    case GPURenderer::HardwareVulkan:
      return std::make_unique<GPU_HW_Vulkan>();

    case GPURenderer::Software:
    default:
      return std::make_unique<GPU_SW>();
  }
}

bool GPUBackend::Create(GPURenderer backend)
{
  g_gpu_backend = CreateBackend(backend);
  if (!g_gpu_backend || !g_gpu_backend->Initialize())
  {
    Log_ErrorPrintf("Failed to initialize GPU backend, falling back to software");
    g_gpu_backend.reset();
    g_gpu_backend = CreateBackend(GPURenderer::Software);
    if (!g_gpu_backend->Initialize())
    {
      g_gpu_backend.reset();
      return false;
    }
  }

  return true;
}

bool GPUBackend::Initialize()
{
  return true;
}

void GPUBackend::Reset()
{
  m_drawing_area = {};
  m_display_aspect_ratio = 1.0f;
  m_display_width = 0;
  m_display_height = 0;
  m_display_origin_left = 0;
  m_display_origin_top = 0;
  m_display_vram_left = 0;
  m_display_vram_top = 0;
  m_display_vram_width = 0;
  m_display_vram_height = 0;
  m_display_vram_start_x = 0;
  m_display_vram_start_y = 0;
  m_display_interlace = GPUInterlacedDisplayMode::None;
  m_display_interlace_field = 0;
  m_display_enabled = false;
  m_display_24bit = false;
}

void GPUBackend::UpdateSettings() {}

void GPUBackend::ResetGraphicsAPIState() {}

void GPUBackend::RestoreGraphicsAPIState() {}

bool GPUBackend::IsHardwareRenderer() const
{
  return false;
}

void GPUBackend::UpdateResolutionScale() {}

std::tuple<u32, u32> GPUBackend::GetEffectiveDisplayResolution()
{
  return std::tie(m_display_vram_width, m_display_vram_height);
}

void GPUBackend::DrawRendererStats(bool is_idle_frame) {}

bool GPUBackend::DoState(StateWrapper& sw)
{
  if (sw.IsReading())
  {
    // Still need a temporary here.
    HeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> temp;
    sw.DoBytes(temp.data(), VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, temp.data(), {});
  }
  else
  {
    FlushRender();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    sw.DoBytes(m_vram_ptr, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
  }

  sw.Do(&m_drawing_area.left);
  sw.Do(&m_drawing_area.top);
  sw.Do(&m_drawing_area.right);
  sw.Do(&m_drawing_area.bottom);

  sw.Do(&m_display_aspect_ratio);
  sw.Do(&m_display_width);
  sw.Do(&m_display_height);
  sw.Do(&m_display_origin_left);
  sw.Do(&m_display_origin_top);
  sw.Do(&m_display_vram_left);
  sw.Do(&m_display_vram_top);
  sw.Do(&m_display_vram_width);
  sw.Do(&m_display_vram_height);
  sw.Do(&m_display_vram_start_x);
  sw.Do(&m_display_vram_start_y);
  sw.Do(&m_display_interlace);
  sw.Do(&m_display_interlace_field);
  sw.Do(&m_display_enabled);
  sw.Do(&m_display_24bit);

  return !sw.HasError();
}

GPUBackendResetCommand* GPUBackend::NewResetCommand()
{
  GPUBackendResetCommand* cmd = static_cast<GPUBackendResetCommand*>(AllocateCommand(sizeof(GPUBackendResetCommand)));
  cmd->type = GPUBackendCommandType::Reset;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendUpdateSettingsCommand* GPUBackend::NewUpdateSettingsCommand()
{
  GPUBackendUpdateSettingsCommand* cmd =
    static_cast<GPUBackendUpdateSettingsCommand*>(AllocateCommand(sizeof(GPUBackendUpdateSettingsCommand)));
  cmd->type = GPUBackendCommandType::UpdateSettings;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendUpdateResolutionScaleCommand* GPUBackend::NewUpdateResolutionScaleCommand()
{
  GPUBackendUpdateResolutionScaleCommand* cmd = static_cast<GPUBackendUpdateResolutionScaleCommand*>(
    AllocateCommand(sizeof(GPUBackendUpdateResolutionScaleCommand)));
  cmd->type = GPUBackendCommandType::UpdateResolutionScale;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendReadVRAMCommand* GPUBackend::NewReadVRAMCommand()
{
  GPUBackendReadVRAMCommand* cmd =
    static_cast<GPUBackendReadVRAMCommand*>(AllocateCommand(sizeof(GPUBackendReadVRAMCommand)));
  cmd->type = GPUBackendCommandType::ReadVRAM;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendFillVRAMCommand* GPUBackend::NewFillVRAMCommand()
{
  GPUBackendFillVRAMCommand* cmd =
    static_cast<GPUBackendFillVRAMCommand*>(AllocateCommand(sizeof(GPUBackendFillVRAMCommand)));
  cmd->type = GPUBackendCommandType::FillVRAM;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendUpdateVRAMCommand* GPUBackend::NewUpdateVRAMCommand(u32 num_words)
{
  const u32 size = sizeof(GPUBackendUpdateVRAMCommand) + (num_words * sizeof(u16));
  GPUBackendUpdateVRAMCommand* cmd = static_cast<GPUBackendUpdateVRAMCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::UpdateVRAM;
  cmd->size = size;
  return cmd;
}

GPUBackendCopyVRAMCommand* GPUBackend::NewCopyVRAMCommand()
{
  GPUBackendCopyVRAMCommand* cmd =
    static_cast<GPUBackendCopyVRAMCommand*>(AllocateCommand(sizeof(GPUBackendCopyVRAMCommand)));
  cmd->type = GPUBackendCommandType::CopyVRAM;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendSetDrawingAreaCommand* GPUBackend::NewSetDrawingAreaCommand()
{
  GPUBackendSetDrawingAreaCommand* cmd =
    static_cast<GPUBackendSetDrawingAreaCommand*>(AllocateCommand(sizeof(GPUBackendSetDrawingAreaCommand)));
  cmd->type = GPUBackendCommandType::SetDrawingArea;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendDrawPolygonCommand* GPUBackend::NewDrawPolygonCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawPolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPolygonCommand::Vertex));
  GPUBackendDrawPolygonCommand* cmd = static_cast<GPUBackendDrawPolygonCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::DrawPolygon;
  cmd->size = size;
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

GPUBackendDrawRectangleCommand* GPUBackend::NewDrawRectangleCommand()
{
  GPUBackendDrawRectangleCommand* cmd =
    static_cast<GPUBackendDrawRectangleCommand*>(AllocateCommand(sizeof(GPUBackendDrawRectangleCommand)));
  cmd->type = GPUBackendCommandType::DrawRectangle;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendDrawLineCommand* GPUBackend::NewDrawLineCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawLineCommand) + (num_vertices * sizeof(GPUBackendDrawLineCommand::Vertex));
  GPUBackendDrawLineCommand* cmd = static_cast<GPUBackendDrawLineCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::DrawLine;
  cmd->size = cmd->Size();
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

GPUBackendClearDisplayCommand* GPUBackend::NewClearDisplayCommand()
{
  GPUBackendClearDisplayCommand* cmd =
    static_cast<GPUBackendClearDisplayCommand*>(AllocateCommand(sizeof(GPUBackendUpdateVRAMCommand)));
  cmd->type = GPUBackendCommandType::ClearDisplay;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendUpdateDisplayCommand* GPUBackend::NewUpdateDisplayCommand()
{
  GPUBackendUpdateDisplayCommand* cmd =
    static_cast<GPUBackendUpdateDisplayCommand*>(AllocateCommand(sizeof(GPUBackendUpdateDisplayCommand)));
  cmd->type = GPUBackendCommandType::UpdateDisplay;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendFlushRenderCommand* GPUBackend::NewFlushRenderCommand()
{
  GPUBackendFlushRenderCommand* cmd =
    static_cast<GPUBackendFlushRenderCommand*>(AllocateCommand(sizeof(GPUBackendFlushRenderCommand)));
  cmd->type = GPUBackendCommandType::FlushRender;
  cmd->size = cmd->Size();
  return cmd;
}

void* GPUBackend::AllocateCommand(u32 size)
{
  for (;;)
  {
    const u32 write_ptr = m_command_fifo_write_ptr.load();
    const u32 available_size = COMMAND_QUEUE_SIZE - write_ptr;
    if ((size + sizeof(GPUBackendSyncCommand)) > available_size)
    {
      Sync();
      continue;
    }

    return &m_command_fifo_data[write_ptr];
  }
}

u32 GPUBackend::GetPendingCommandSize() const
{
  const u32 read_ptr = m_command_fifo_read_ptr.load();
  const u32 write_ptr = m_command_fifo_write_ptr.load();
  return (write_ptr - read_ptr);
}

void GPUBackend::PushCommand(GPUBackendCommand* cmd)
{
  if (!g_settings.cpu_thread)
  {
    // single-thread mode
    if (cmd->type != GPUBackendCommandType::Sync)
      HandleCommand(cmd);
  }
  else
  {
    const u32 new_write_ptr = m_command_fifo_write_ptr.fetch_add(cmd->size) + cmd->size;
    DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
    if (cmd->type == GPUBackendCommandType::Sync || cmd->type == GPUBackendCommandType::FrameDone ||
        (new_write_ptr - m_command_fifo_read_ptr.load()) >= THRESHOLD_TO_WAKE_GPU)
    {
      WakeGPUThread();
    }
  }
}

void GPUBackend::WakeGPUThread()
{
  std::unique_lock<std::mutex> lock(m_sync_mutex);
  if (!m_gpu_thread_sleeping.load())
    return;

  m_wake_gpu_thread_cv.notify_one();
}

void GPUBackend::Sync()
{
  if (!g_settings.cpu_thread)
    return;

  // since we do this on wrap-around, it can't go through the regular path
  const u32 write_ptr = m_command_fifo_write_ptr.load();
  Assert((COMMAND_QUEUE_SIZE - write_ptr) >= sizeof(GPUBackendSyncCommand));
  GPUBackendSyncCommand* cmd = reinterpret_cast<GPUBackendSyncCommand*>(&m_command_fifo_data[write_ptr]);
  cmd->type = GPUBackendCommandType::Sync;
  cmd->size = cmd->Size();
  PushCommand(cmd);

  m_sync_event.Wait();
  m_sync_event.Reset();
}

void GPUBackend::CPUFrameDone()
{
  if (!g_settings.cpu_thread)
    return;

  GPUBackendFrameDoneCommand* cmd =
    reinterpret_cast<GPUBackendFrameDoneCommand*>(AllocateCommand(sizeof(GPUBackendFrameDoneCommand)));
  cmd->type = GPUBackendCommandType::FrameDone;
  cmd->size = cmd->Size();
  PushCommand(cmd);
}

void GPUBackend::ProcessGPUCommands()
{
  for (;;)
  {
    const u32 write_ptr = m_command_fifo_write_ptr.load();
    u32 read_ptr = m_command_fifo_read_ptr.load();
    if (read_ptr == write_ptr)
      return;

    while (read_ptr < write_ptr)
    {
      const GPUBackendCommand* cmd = reinterpret_cast<const GPUBackendCommand*>(&m_command_fifo_data[read_ptr]);
      read_ptr += cmd->size;

      if (cmd->type == GPUBackendCommandType::Sync)
      {
        Assert(read_ptr == m_command_fifo_write_ptr.load());
        m_command_fifo_read_ptr.store(0);
        m_command_fifo_write_ptr.store(0);
        m_sync_event.Signal();
        return;
      }
      else if (cmd->type == GPUBackendCommandType::FrameDone)
      {
        m_frame_done = true;
        m_command_fifo_read_ptr.store(read_ptr);
        return;
      }
      else
      {
        HandleCommand(cmd);
      }
    }

    m_command_fifo_read_ptr.store(read_ptr);
  }
}

void GPUBackend::RunGPUFrame()
{
  m_frame_done = false;

  for (;;)
  {
    g_gpu_backend->ProcessGPUCommands();

    if (m_frame_done)
      break;

    std::unique_lock<std::mutex> lock(m_sync_mutex);
    m_gpu_thread_sleeping.store(true);
    m_wake_gpu_thread_cv.wait(lock);
    m_gpu_thread_sleeping.store(false);
  }
}

void GPUBackend::EndGPUFrame()
{
  g_gpu_backend->ProcessGPUCommands();
  Assert(m_command_fifo_read_ptr.load() == m_command_fifo_write_ptr.load());
  m_command_fifo_read_ptr.store(0);
  m_command_fifo_write_ptr.store(0);
}

void GPUBackend::SetScissorFromDrawingArea() {}

void GPUBackend::HandleCommand(const GPUBackendCommand* cmd)
{
  switch (cmd->type)
  {
    case GPUBackendCommandType::ReadVRAM:
    {
      FlushRender();
      const GPUBackendReadVRAMCommand* ccmd = static_cast<const GPUBackendReadVRAMCommand*>(cmd);
      ReadVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height));
    }
    break;

    case GPUBackendCommandType::FillVRAM:
    {
      FlushRender();
      const GPUBackendFillVRAMCommand* ccmd = static_cast<const GPUBackendFillVRAMCommand*>(cmd);
      FillVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
               ccmd->color, ccmd->params);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      FlushRender();
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      UpdateVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
                 ccmd->data, ccmd->params);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      FlushRender();
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      CopyVRAM(ZeroExtend32(ccmd->src_x), ZeroExtend32(ccmd->src_y), ZeroExtend32(ccmd->dst_x),
               ZeroExtend32(ccmd->dst_y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height), ccmd->params);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      FlushRender();
      m_drawing_area = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd)->new_area;
      SetScissorFromDrawingArea();
    }
    break;

    case GPUBackendCommandType::DrawPolygon:
    {
      DrawPolygon(static_cast<const GPUBackendDrawPolygonCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawRectangle:
    {
      DrawRectangle(static_cast<const GPUBackendDrawRectangleCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawLine:
    {
      DrawLine(static_cast<const GPUBackendDrawLineCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::ClearDisplay:
    {
      ClearDisplay();
    }
    break;

    case GPUBackendCommandType::UpdateDisplay:
    {
      const GPUBackendUpdateDisplayCommand* ccmd = static_cast<const GPUBackendUpdateDisplayCommand*>(cmd);

      m_display_aspect_ratio = ccmd->display_aspect_ratio;
      m_display_width = ccmd->display_width;
      m_display_height = ccmd->display_height;
      m_display_origin_left = ccmd->display_origin_left;
      m_display_origin_top = ccmd->display_origin_top;
      m_display_vram_left = ccmd->display_vram_left;
      m_display_vram_top = ccmd->display_vram_top;
      m_display_vram_width = ccmd->display_vram_width;
      m_display_vram_height = ccmd->display_vram_height;
      m_display_vram_start_x = ccmd->display_vram_start_x;
      m_display_vram_start_y = ccmd->display_vram_start_y;
      m_display_interlace = ccmd->display_interlace;
      m_display_interlace_field = ccmd->display_interlace_field;
      m_display_enabled = ccmd->display_enabled;
      m_display_24bit = ccmd->display_24bit;

      UpdateDisplay();
    }
    break;

    case GPUBackendCommandType::FlushRender:
    {
      FlushRender();
    }
    break;

    default:
      break;
  }
}

void GPUBackend::SoftwareFillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params)
{
  const u16 color16 = RGBA8888ToRGBA5551(color);
  if ((x + width) <= VRAM_WIDTH && !params.interlaced_rendering)
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      std::fill_n(&m_vram_ptr[row * VRAM_WIDTH + x], width, color16);
    }
  }
  else if (params.interlaced_rendering)
  {
    // Hardware tests show that fills seem to break on the first two lines when the offset matches the displayed field.
    const u32 active_field = params.active_line_lsb;
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      if ((row & u32(1)) == active_field)
        continue;

      u16* row_ptr = &m_vram_ptr[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
  else
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &m_vram_ptr[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
}

void GPUBackend::SoftwareUpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                                    GPUBackendCommandParameters params)
{
  // Fast path when the copy is not oversized.
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !params.IsMaskingEnabled())
  {
    const u16* src_ptr = static_cast<const u16*>(data);
    u16* dst_ptr = &m_vram_ptr[y * VRAM_WIDTH + x];
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      std::copy_n(src_ptr, width, dst_ptr);
      src_ptr += width;
      dst_ptr += VRAM_WIDTH;
    }
  }
  else
  {
    // Slow path when we need to handle wrap-around.
    const u16* src_ptr = static_cast<const u16*>(data);
    const u16 mask_and = params.GetMaskAND();
    const u16 mask_or = params.GetMaskOR();

    for (u32 row = 0; row < height;)
    {
      u16* dst_row_ptr = &m_vram_ptr[((y + row++) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (u32 col = 0; col < width;)
      {
        // TODO: Handle unaligned reads...
        u16* pixel_ptr = &dst_row_ptr[(x + col++) % VRAM_WIDTH];
        if (((*pixel_ptr) & mask_and) == 0)
          *pixel_ptr = *(src_ptr++) | mask_or;
      }
    }
  }
}

void GPUBackend::SoftwareCopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                                  GPUBackendCommandParameters params)
{
  // Break up oversized copies. This behavior has not been verified on console.
  if ((src_x + width) > VRAM_WIDTH || (dst_x + width) > VRAM_WIDTH)
  {
    u32 remaining_rows = height;
    u32 current_src_y = src_y;
    u32 current_dst_y = dst_y;
    while (remaining_rows > 0)
    {
      const u32 rows_to_copy =
        std::min<u32>(remaining_rows, std::min<u32>(VRAM_HEIGHT - current_src_y, VRAM_HEIGHT - current_dst_y));

      u32 remaining_columns = width;
      u32 current_src_x = src_x;
      u32 current_dst_x = dst_x;
      while (remaining_columns > 0)
      {
        const u32 columns_to_copy =
          std::min<u32>(remaining_columns, std::min<u32>(VRAM_WIDTH - current_src_x, VRAM_WIDTH - current_dst_x));
        SoftwareCopyVRAM(current_src_x, current_src_y, current_dst_x, current_dst_y, columns_to_copy, rows_to_copy,
                         params);
        current_src_x = (current_src_x + columns_to_copy) % VRAM_WIDTH;
        current_dst_x = (current_dst_x + columns_to_copy) % VRAM_WIDTH;
        remaining_columns -= columns_to_copy;
      }

      current_src_y = (current_src_y + rows_to_copy) % VRAM_HEIGHT;
      current_dst_y = (current_dst_y + rows_to_copy) % VRAM_HEIGHT;
      remaining_rows -= rows_to_copy;
    }

    return;
  }

  // This doesn't have a fast path, but do we really need one? It's not common.
  const u16 mask_and = params.GetMaskAND();
  const u16 mask_or = params.GetMaskOR();

  // Copy in reverse when src_x < dst_x, this is verified on console.
  if (src_x < dst_x || ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH))
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram_ptr[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &m_vram_ptr[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (s32 col = static_cast<s32>(width - 1); col >= 0; col--)
      {
        const u16 src_pixel = src_row_ptr[(src_x + static_cast<u32>(col)) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + static_cast<u32>(col)) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
  else
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram_ptr[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &m_vram_ptr[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (u32 col = 0; col < width; col++)
      {
        const u16 src_pixel = src_row_ptr[(src_x + col) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + col) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
}
