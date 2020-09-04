#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "interrupt_controller.h"
#include "pgxp.h"
#include "system.h"
Log_SetChannel(GPU);

#define CHECK_COMMAND_SIZE(num_words)                                                                                  \
  if (m_fifo.GetSize() < num_words)                                                                                    \
  {                                                                                                                    \
    m_command_total_words = num_words;                                                                                 \
    return false;                                                                                                      \
  }

static u32 s_cpu_to_vram_dump_id = 1;
static u32 s_vram_to_cpu_dump_id = 1;

static constexpr u32 ReplaceZero(u32 value, u32 value_for_zero)
{
  return value == 0 ? value_for_zero : value;
}

template<typename T>
ALWAYS_INLINE static constexpr std::tuple<T, T> MinMax(T v1, T v2)
{
  if (v1 > v2)
    return std::tie(v2, v1);
  else
    return std::tie(v1, v2);
}

void GPU::ExecuteCommands()
{
  m_syncing = true;

  for (;;)
  {
    if (m_pending_command_ticks <= m_max_run_ahead && !m_fifo.IsEmpty())
    {
      switch (m_blitter_state)
      {
        case BlitterState::Idle:
        {
          const u32 command = FifoPeek(0) >> 24;
          if ((this->*s_GP0_command_handler_table[command])())
            continue;
          else
            goto batch_done;
        }

        case BlitterState::WritingVRAM:
        {
          DebugAssert(m_blit_remaining_words > 0);
          const u32 words_to_copy = std::min(m_blit_remaining_words, m_fifo.GetSize());
          m_blit_buffer.reserve(m_blit_buffer.size() + words_to_copy);
          for (u32 i = 0; i < words_to_copy; i++)
            m_blit_buffer.push_back(FifoPop());
          m_blit_remaining_words -= words_to_copy;
          AddCommandTicks(words_to_copy);

          Log_DebugPrintf("VRAM write burst of %u words, %u words remaining", words_to_copy, m_blit_remaining_words);
          if (m_blit_remaining_words == 0)
            FinishVRAMWrite();

          continue;
        }

        case BlitterState::ReadingVRAM:
        {
          goto batch_done;
        }
        break;

        case BlitterState::DrawingPolyLine:
        {
          const u32 words_per_vertex = m_render_command.shading_enable ? 2 : 1;
          u32 terminator_index =
            m_render_command.shading_enable ? ((static_cast<u32>(m_blit_buffer.size()) & 1u) ^ 1u) : 0u;
          for (; terminator_index < m_fifo.GetSize(); terminator_index += words_per_vertex)
          {
            // polyline must have at least two vertices, and the terminator is (word & 0xf000f000) == 0x50005000.
            // terminator is on the first word for the vertex
            if ((FifoPeek(terminator_index) & UINT32_C(0xF000F000)) == UINT32_C(0x50005000))
              break;
          }

          const bool found_terminator = (terminator_index < m_fifo.GetSize());
          const u32 words_to_copy = std::min(terminator_index, m_fifo.GetSize());
          if (words_to_copy > 0)
          {
            m_blit_buffer.reserve(m_blit_buffer.size() + words_to_copy);
            for (u32 i = 0; i < words_to_copy; i++)
              m_blit_buffer.push_back(FifoPop());
          }

          Log_DebugPrintf("Added %u words to polyline", words_to_copy);
          if (found_terminator)
          {
            // drop terminator
            m_fifo.RemoveOne();
            Log_DebugPrintf("Drawing poly-line with %u vertices", GetPolyLineVertexCount());
            FinishPolyLineRenderCommand();
            m_blit_buffer.clear();
            EndCommand();
            continue;
          }
        }
        break;
      }
    }

  batch_done:
    m_fifo_pushed = false;
    UpdateDMARequest();
    if (!m_fifo_pushed)
      break;
  }

  UpdateGPUIdle();
  m_syncing = false;
}

void GPU::EndCommand()
{
  m_blitter_state = BlitterState::Idle;
  m_command_total_words = 0;
}

GPU::GP0CommandHandlerTable GPU::GenerateGP0CommandHandlerTable()
{
  GP0CommandHandlerTable table = {};
  for (u32 i = 0; i < static_cast<u32>(table.size()); i++)
    table[i] = &GPU::HandleUnknownGP0Command;
  table[0x00] = &GPU::HandleNOPCommand;
  table[0x01] = &GPU::HandleClearCacheCommand;
  table[0x02] = &GPU::HandleFillRectangleCommand;
  table[0x03] = &GPU::HandleNOPCommand;
  for (u32 i = 0x04; i <= 0x1E; i++)
    table[i] = &GPU::HandleNOPCommand;
  table[0x1F] = &GPU::HandleInterruptRequestCommand;
  for (u32 i = 0x20; i <= 0x7F; i++)
  {
    const GPURenderCommand rc{i << 24};
    switch (rc.primitive)
    {
      case GPUPrimitive::Polygon:
        table[i] = &GPU::HandleRenderPolygonCommand;
        break;
      case GPUPrimitive::Line:
        table[i] = rc.polyline ? &GPU::HandleRenderPolyLineCommand : &GPU::HandleRenderLineCommand;
        break;
      case GPUPrimitive::Rectangle:
        table[i] = &GPU::HandleRenderRectangleCommand;
        break;
      default:
        table[i] = &GPU::HandleUnknownGP0Command;
        break;
    }
  }
  table[0xE0] = &GPU::HandleNOPCommand;
  table[0xE1] = &GPU::HandleSetDrawModeCommand;
  table[0xE2] = &GPU::HandleSetTextureWindowCommand;
  table[0xE3] = &GPU::HandleSetDrawingAreaTopLeftCommand;
  table[0xE4] = &GPU::HandleSetDrawingAreaBottomRightCommand;
  table[0xE5] = &GPU::HandleSetDrawingOffsetCommand;
  table[0xE6] = &GPU::HandleSetMaskBitCommand;
  for (u32 i = 0xE7; i <= 0xEF; i++)
    table[i] = &GPU::HandleNOPCommand;
  for (u32 i = 0x80; i <= 0x9F; i++)
    table[i] = &GPU::HandleCopyRectangleVRAMToVRAMCommand;
  for (u32 i = 0xA0; i <= 0xBF; i++)
    table[i] = &GPU::HandleCopyRectangleCPUToVRAMCommand;
  for (u32 i = 0xC0; i <= 0xDF; i++)
    table[i] = &GPU::HandleCopyRectangleVRAMToCPUCommand;

  return table;
}

bool GPU::HandleUnknownGP0Command()
{
  const u32 command = FifoPeek() >> 24;
  Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);

  SmallString dump;
  for (u32 i = 0; i < m_fifo.GetSize(); i++)
    dump.AppendFormattedString("%s0x%08X", (i > 0) ? " " : "", FifoPeek(i));
  Log_ErrorPrintf("FIFO: %s", dump.GetCharArray());

  m_fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleNOPCommand()
{
  m_fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleClearCacheCommand()
{
  Log_DebugPrintf("GP0 clear cache");
  m_fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleInterruptRequestCommand()
{
  Log_WarningPrintf("GP0 interrupt request");
  if (!m_GPUSTAT.interrupt_request)
  {
    m_GPUSTAT.interrupt_request = true;
    g_interrupt_controller.InterruptRequest(InterruptController::IRQ::GPU);
  }

  m_fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawModeCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  Log_DebugPrintf("Set draw mode %08X", param);

  GPUDrawModeReg new_mode_reg{static_cast<u16>(param & GPUDrawModeReg::MASK)};
  if (!m_set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  // Bits 0..10 are returned in the GPU status register.
  m_GPUSTAT.bits = (m_GPUSTAT.bits & ~(GPUDrawModeReg::GPUSTAT_MASK)) |
                   (ZeroExtend32(new_mode_reg.bits) & GPUDrawModeReg::GPUSTAT_MASK);
  m_GPUSTAT.texture_disable = new_mode_reg.texture_disable;
  m_draw_mode.bits = new_mode_reg.bits;

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetTextureWindowCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  m_texture_window.bits = param;
  Log_DebugPrintf("Set texture window %02X %02X %02X %02X", m_texture_window.mask_x, m_texture_window.mask_y,
                  m_texture_window.offset_x, m_texture_window.offset_y);

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingAreaTopLeftCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  const u32 left = param & VRAM_WIDTH_MASK;
  const u32 top = (param >> 10) & VRAM_HEIGHT_MASK;
  Log_DebugPrintf("Set drawing area top-left: (%u, %u)", left, top);
  if (m_drawing_area.left != left || m_drawing_area.top != top)
  {
    m_drawing_area.left = left;
    m_drawing_area.top = top;
    UpdateDrawingArea();
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingAreaBottomRightCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  const u32 right = param & VRAM_WIDTH_MASK;
  const u32 bottom = (param >> 10) & VRAM_HEIGHT_MASK;
  Log_DebugPrintf("Set drawing area bottom-right: (%u, %u)", m_drawing_area.right, m_drawing_area.bottom);
  if (m_drawing_area.right != right || m_drawing_area.bottom != bottom)
  {
    m_drawing_area.right = right;
    m_drawing_area.bottom = bottom;
    UpdateDrawingArea();
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingOffsetCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  const s32 x = SignExtendN<11, s32>(param & 0x7FFu);
  const s32 y = SignExtendN<11, s32>((param >> 11) & 0x7FFu);
  Log_DebugPrintf("Set drawing offset (%d, %d)", m_drawing_offset.x, m_drawing_offset.y);
  if (m_drawing_offset.x != x || m_drawing_offset.y != y)
  {
    FlushRender();

    m_drawing_offset.x = x;
    m_drawing_offset.y = y;
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetMaskBitCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  constexpr u32 gpustat_mask = (1 << 11) | (1 << 12);
  const u32 gpustat_bits = (param & 0x03) << 11;
  if ((m_GPUSTAT.bits & gpustat_mask) != gpustat_bits)
    m_GPUSTAT.bits = (m_GPUSTAT.bits & ~gpustat_mask) | gpustat_bits;

  Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.set_mask_while_drawing),
                  BoolToUInt32(m_GPUSTAT.check_mask_before_draw));

  AddCommandTicks(1);
  EndCommand();
  return true;
}

void GPU::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = IsInterlacedRenderingEnabled();
}

void GPU::ClearDisplay()
{
  g_gpu_backend->PushCommand(g_gpu_backend->NewClearDisplayCommand());
}

void GPU::UpdateDisplay()
{
  GPUBackendUpdateDisplayCommand* cmd = g_gpu_backend->NewUpdateDisplayCommand();
  cmd->display_aspect_ratio = m_crtc_state.display_aspect_ratio;
  cmd->display_width = m_crtc_state.display_width;
  cmd->display_height = m_crtc_state.display_height;
  cmd->display_origin_left = m_crtc_state.display_origin_left;
  cmd->display_origin_top = m_crtc_state.display_origin_top;
  cmd->display_vram_left = m_crtc_state.display_vram_left;
  cmd->display_vram_top = m_crtc_state.display_vram_top;
  cmd->display_vram_width = m_crtc_state.display_vram_width;
  cmd->display_vram_height = m_crtc_state.display_vram_height;
  cmd->display_vram_start_x = m_crtc_state.regs.X;
  cmd->display_vram_start_y = m_crtc_state.regs.Y;
  cmd->display_interlace = GetInterlacedDisplayMode();
  cmd->display_interlace_field = m_crtc_state.interlaced_display_field;
  cmd->display_enabled = !m_GPUSTAT.display_disable;
  cmd->display_24bit = m_GPUSTAT.display_area_color_depth_24;
  g_gpu_backend->PushCommand(cmd);
}

void GPU::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.bits;
  cmd->window.bits = m_texture_window.bits;
}

void GPU::UpdateDrawingArea()
{
  GPUBackendSetDrawingAreaCommand* cmd = g_gpu_backend->NewSetDrawingAreaCommand();
  cmd->new_area = m_drawing_area;
  g_gpu_backend->PushCommand(cmd);
}

void GPU::FlushRender()
{
  g_gpu_backend->PushCommand(g_gpu_backend->NewFlushRenderCommand());
}

bool GPU::HandleRenderPolygonCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};

  // shaded vertices use the colour from the first word for the first vertex
  const u32 words_per_vertex = 1 + BoolToUInt32(rc.texture_enable) + BoolToUInt32(rc.shading_enable);
  const u32 num_vertices = rc.quad_polygon ? 4 : 3;
  const u32 total_words = words_per_vertex * num_vertices + BoolToUInt32(!rc.shading_enable);
  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  // setup time
  static constexpr u16 s_setup_time[2][2][2] = {{{46, 226}, {334, 496}}, {{82, 262}, {370, 532}}};
  const TickCount setup_ticks = static_cast<TickCount>(ZeroExtend32(
    s_setup_time[BoolToUInt8(rc.quad_polygon)][BoolToUInt8(rc.shading_enable)][BoolToUInt8(rc.texture_enable)]));
  AddCommandTicks(setup_ticks);

  Log_TracePrintf("Render %s %s %s %s polygon (%u verts, %u words per vert), %d setup ticks",
                  rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  ZeroExtend32(num_vertices), ZeroExtend32(words_per_vertex), setup_ticks);

  GPUBackendDrawPolygonCommand* cmd = g_gpu_backend->NewDrawPolygonCommand(num_vertices);
  FillDrawCommand(cmd, rc);

  // set draw state up
  if (rc.texture_enable)
  {
    const u16 texpage_attribute = Truncate16((rc.shading_enable ? FifoPeek(5) : FifoPeek(4)) >> 16);

    m_GPUSTAT.bits = (m_GPUSTAT.bits & ~(GPUDrawModeReg::GPUSTAT_MASK)) |
                     (ZeroExtend32(texpage_attribute) & GPUDrawModeReg::GPUSTAT_MASK);

    cmd->draw_mode.bits = ((texpage_attribute & GPUDrawModeReg::POLYGON_TEXPAGE_MASK) |
                           (m_draw_mode.bits & ~GPUDrawModeReg::POLYGON_TEXPAGE_MASK));
    cmd->palette.bits = Truncate16(FifoPeek(2) >> 16);
  }
  else
  {
    cmd->palette.bits = 0;
  }

  m_stats.num_vertices += num_vertices;
  m_stats.num_polygons++;

  m_fifo.RemoveOne();

  const u32 first_color = rc.color_for_first_vertex;
  const bool shaded = rc.shading_enable;
  const bool textured = rc.texture_enable;
  const bool pgxp = g_settings.gpu_pgxp_enable;

  bool valid_w = g_settings.gpu_pgxp_texture_correction;
  for (u32 i = 0; i < num_vertices; i++)
  {
    GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
    vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
    const u64 maddr_and_pos = m_fifo.Pop();
    const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
    vert->x = m_drawing_offset.x + vp.x;
    vert->y = m_drawing_offset.y + vp.y;
    vert->precise_x = static_cast<float>(vert->x);
    vert->precise_y = static_cast<float>(vert->y);
    vert->precise_w = 1.0f;
    vert->texcoord = textured ? Truncate16(FifoPop()) : 0;
    const s32 native_x = m_drawing_offset.x + vp.x;

    if (pgxp)
    {
      valid_w &= PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, vert->x, vert->y, m_drawing_offset.x,
                                        m_drawing_offset.y, &vert->precise_x, &vert->precise_y, &vert->precise_w);
    }
  }
  if (pgxp && !valid_w)
  {
    for (u32 i = 0; i < num_vertices; i++)
      cmd->vertices[i].precise_w = 1.0f;
  }

  if (!IsDrawingAreaIsValid())
  {
    EndCommand();
    return true;
  }

  // Cull polygons which are too large.
  const auto [min_x_12, max_x_12] = MinMax(cmd->vertices[1].x, cmd->vertices[2].x);
  const auto [min_y_12, max_y_12] = MinMax(cmd->vertices[1].y, cmd->vertices[2].y);
  const s32 min_x = std::min(min_x_12, cmd->vertices[0].x);
  const s32 max_x = std::max(max_x_12, cmd->vertices[0].x);
  const s32 min_y = std::min(min_y_12, cmd->vertices[0].y);
  const s32 max_y = std::max(max_y_12, cmd->vertices[0].y);

  if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
  {
    Log_DebugPrintf("Culling too-large polygon: %d,%d %d,%d %d,%d", cmd->vertices[0].x, cmd->vertices[0].y,
                    cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[2].x, cmd->vertices[2].y);

    if (!rc.quad_polygon)
    {
      EndCommand();
      return true;
    }

    // turn it into a degenerate triangle
    std::memcpy(&cmd->vertices[0], &cmd->vertices[1], sizeof(GPUBackendDrawPolygonCommand::Vertex));
    cmd->bounds.SetInvalid();
  }
  else
  {
    const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
    const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
    const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
    const u32 clip_bottom = static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

    cmd->bounds.Set(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right), Truncate16(clip_bottom));
    AddDrawTriangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable, rc.texture_enable,
                         rc.transparency_enable);
  }

  // quads
  if (rc.quad_polygon)
  {
    const s32 min_x_123 = std::min(min_x_12, cmd->vertices[3].x);
    const s32 max_x_123 = std::max(max_x_12, cmd->vertices[3].x);
    const s32 min_y_123 = std::min(min_y_12, cmd->vertices[3].y);
    const s32 max_y_123 = std::max(max_y_12, cmd->vertices[3].y);

    // Cull polygons which are too large.
    if ((max_x_123 - min_x_123) >= MAX_PRIMITIVE_WIDTH || (max_y_123 - min_y_123) >= MAX_PRIMITIVE_HEIGHT)
    {
      Log_DebugPrintf("Culling too-large polygon (quad second half): %d,%d %d,%d %d,%d", cmd->vertices[2].x,
                      cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[0].x,
                      cmd->vertices[0].y);

      // turn it into a degenerate triangle
      std::memcpy(&cmd->vertices[3], &cmd->vertices[2], sizeof(GPUBackendDrawPolygonCommand::Vertex));
      cmd->bounds.SetInvalid();
    }
    else
    {
      const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x_123, m_drawing_area.left, m_drawing_area.right));
      const u32 clip_right =
        static_cast<u32>(std::clamp<s32>(max_x_123, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y_123, m_drawing_area.top, m_drawing_area.bottom));
      const u32 clip_bottom =
        static_cast<u32>(std::clamp<s32>(max_y_123, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      cmd->bounds.Include(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right), Truncate16(clip_bottom));
      AddDrawTriangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable, rc.texture_enable,
                           rc.transparency_enable);
    }
  }

  g_gpu_backend->PushCommand(cmd);

  EndCommand();
  return true;
}

bool GPU::HandleRenderRectangleCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 total_words =
    2 + BoolToUInt32(rc.texture_enable) + BoolToUInt32(rc.rectangle_size == GPUDrawRectangleSize::Variable);

  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  Log_TracePrintf("Render %s %s %s rectangle (%u words), %d setup ticks",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  total_words, setup_ticks);

  m_stats.num_vertices++;
  m_stats.num_polygons++;
  m_fifo.RemoveOne();

  GPUBackendDrawRectangleCommand* cmd = g_gpu_backend->NewDrawRectangleCommand();
  FillDrawCommand(cmd, rc);
  cmd->color = rc.color_for_first_vertex;
  cmd->draw_mode.bits = m_draw_mode.bits;
  cmd->window.bits = m_texture_window.bits;

  const GPUVertexPosition vp{FifoPop()};
  cmd->x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
  cmd->y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

  if (rc.texture_enable)
  {
    const u32 texcoord_and_palette = FifoPop();
    cmd->palette.bits = Truncate16(texcoord_and_palette >> 16);
    cmd->texcoord = Truncate16(texcoord_and_palette);
  }
  else
  {
    cmd->palette.bits = 0;
    cmd->texcoord = 0;
  }

  switch (rc.rectangle_size)
  {
    case GPUDrawRectangleSize::R1x1:
      cmd->width = 1;
      cmd->height = 1;
      break;
    case GPUDrawRectangleSize::R8x8:
      cmd->width = 8;
      cmd->height = 8;
      break;
    case GPUDrawRectangleSize::R16x16:
      cmd->width = 16;
      cmd->height = 16;
      break;
    default:
    {
      const u32 width_and_height = FifoPop();
      cmd->width = static_cast<u16>(width_and_height & VRAM_WIDTH_MASK);
      cmd->height = static_cast<u16>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

      if (cmd->width >= MAX_PRIMITIVE_WIDTH || cmd->height >= MAX_PRIMITIVE_HEIGHT)
      {
        Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", cmd->x, cmd->y, cmd->width, cmd->height);
        return true;
      }
    }
    break;
  }

  if (!IsDrawingAreaIsValid())
  {
    EndCommand();
    return true;
  }

  const u32 clip_left = static_cast<u32>(std::clamp<s32>(cmd->x, m_drawing_area.left, m_drawing_area.right));
  const u32 clip_right =
    static_cast<u32>(std::clamp<s32>(cmd->x + cmd->width, m_drawing_area.left, m_drawing_area.right)) + 1u;
  const u32 clip_top = static_cast<u32>(std::clamp<s32>(cmd->y, m_drawing_area.top, m_drawing_area.bottom));
  const u32 clip_bottom =
    static_cast<u32>(std::clamp<s32>(cmd->y + cmd->height, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

  cmd->bounds.Set(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right), Truncate16(clip_bottom));
  AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.texture_enable, rc.transparency_enable);

  g_gpu_backend->PushCommand(cmd);

  EndCommand();
  return true;
}

bool GPU::HandleRenderLineCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 total_words = rc.shading_enable ? 4 : 3;
  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  Log_TracePrintf("Render %s %s line (%u total words)", rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.shading_enable ? "shaded" : "monochrome", total_words);

  m_stats.num_vertices += 2;
  m_stats.num_polygons++;
  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  GPUBackendDrawLineCommand* cmd = g_gpu_backend->NewDrawLineCommand(2);
  FillDrawCommand(cmd, rc);
  cmd->palette.bits = 0;

  if (rc.shading_enable)
  {
    cmd->vertices[0].color = rc.color_for_first_vertex;
    const GPUVertexPosition start_pos{FifoPop()};
    cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
    cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

    cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
    const GPUVertexPosition end_pos{FifoPop()};
    cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
    cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
  }
  else
  {
    cmd->vertices[0].color = rc.color_for_first_vertex;
    cmd->vertices[1].color = rc.color_for_first_vertex;

    const GPUVertexPosition start_pos{FifoPop()};
    cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
    cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

    const GPUVertexPosition end_pos{FifoPop()};
    cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
    cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
  }

  if (!IsDrawingAreaIsValid())
  {
    EndCommand();
    return true;
  }

  const auto [min_x, max_x] = MinMax(cmd->vertices[0].x, cmd->vertices[1].x);
  const auto [min_y, max_y] = MinMax(cmd->vertices[0].y, cmd->vertices[1].y);
  if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
  {
    Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", cmd->vertices[0].y, cmd->vertices[0].y, cmd->vertices[1].x,
                    cmd->vertices[1].y);
    EndCommand();
    return true;
  }

  const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.left));
  const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
  const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
  const u32 clip_bottom = static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;
  cmd->bounds.Set(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right), Truncate16(clip_bottom));
  AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

  EndCommand();
  return true;
}

bool GPU::HandleRenderPolyLineCommand()
{
  // always read the first two vertices, we test for the terminator after that
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 min_words = rc.shading_enable ? 3 : 4;
  CHECK_COMMAND_SIZE(min_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  Log_TracePrintf("Render %s %s poly-line, %d setup ticks", rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.shading_enable ? "shaded" : "monochrome", setup_ticks);

  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  const u32 words_to_pop = min_words - 1;
  // m_blit_buffer.resize(words_to_pop);
  // FifoPopRange(m_blit_buffer.data(), words_to_pop);
  m_blit_buffer.reserve(words_to_pop);
  for (u32 i = 0; i < words_to_pop; i++)
    m_blit_buffer.push_back(Truncate32(FifoPop()));

  // polyline goes via a different path through the blit buffer
  m_blitter_state = BlitterState::DrawingPolyLine;
  m_command_total_words = 0;
  return true;
}

void GPU::FinishPolyLineRenderCommand()
{
  // Multiply by two because we don't use line strips.
  const u32 num_vertices = GetPolyLineVertexCount();
  if (!IsDrawingAreaIsValid())
    return;

  GPUBackendDrawLineCommand* cmd = g_gpu_backend->NewDrawLineCommand(num_vertices);
  FillDrawCommand(cmd, m_render_command);

  u32 buffer_pos = 0;
  const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
  cmd->vertices[0].x = start_vp.x + m_drawing_offset.x;
  cmd->vertices[0].y = start_vp.y + m_drawing_offset.y;
  cmd->vertices[0].color = m_render_command.color_for_first_vertex;
  cmd->bounds.SetInvalid();

  const bool shaded = m_render_command.shading_enable;
  for (u32 i = 1; i < num_vertices; i++)
  {
    cmd->vertices[i].color =
      shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : m_render_command.color_for_first_vertex;
    const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
    cmd->vertices[i].x = m_drawing_offset.x + vp.x;
    cmd->vertices[i].y = m_drawing_offset.y + vp.y;

    const auto [min_x, max_x] = MinMax(cmd->vertices[i - 1].x, cmd->vertices[i].y);
    const auto [min_y, max_y] = MinMax(cmd->vertices[i - 1].x, cmd->vertices[i].y);
    if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
    {
      Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", cmd->vertices[i - 1].x, cmd->vertices[i - 1].y,
                      cmd->vertices[i].x, cmd->vertices[i].y);
    }
    else
    {
      const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.left));
      const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
      const u32 clip_bottom = static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      cmd->bounds.Include(Truncate16(clip_left), Truncate16(clip_right), Truncate16(clip_top), Truncate16(clip_bottom));
      AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, m_render_command.shading_enable);
    }
  }
}

bool GPU::HandleFillRectangleCommand()
{
  CHECK_COMMAND_SIZE(3);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  GPUBackendFillVRAMCommand* cmd = g_gpu_backend->NewFillVRAMCommand();
  FillBackendCommandParameters(cmd);

  cmd->color = FifoPop() & 0x00FFFFFF;
  cmd->x = Truncate16(FifoPeek() & 0x3F0);
  cmd->y = Truncate16((FifoPop() >> 16) & VRAM_COORD_MASK);
  cmd->width = Truncate16(((FifoPeek() & VRAM_WIDTH_MASK) + 0xF) & ~0xF);
  cmd->height = Truncate16((FifoPop() >> 16) & VRAM_HEIGHT_MASK);

  Log_DebugPrintf("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u)", cmd->x, cmd->y, cmd->width, cmd->height);

  AddCommandTicks(46 + ((cmd->width / 8) + 9) * cmd->height);

  g_gpu_backend->PushCommand(cmd);
  m_stats.num_vram_fills++;

  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

  const u32 dst_x = FifoPeek() & VRAM_COORD_MASK;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_COORD_MASK;
  const u32 copy_width = ReplaceZero(FifoPeek() & VRAM_WIDTH_MASK, 0x400);
  const u32 copy_height = ReplaceZero((FifoPop() >> 16) & VRAM_HEIGHT_MASK, 0x200);
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = ((num_pixels + 1) / 2);

  Log_DebugPrintf("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, copy_width,
                  copy_height);

  EndCommand();

  m_blitter_state = BlitterState::WritingVRAM;
  m_blit_buffer.reserve(num_words);
  m_blit_remaining_words = num_words;
  m_vram_transfer.x = Truncate16(dst_x);
  m_vram_transfer.y = Truncate16(dst_y);
  m_vram_transfer.width = Truncate16(copy_width);
  m_vram_transfer.height = Truncate16(copy_height);
  return true;
}

void GPU::FinishVRAMWrite()
{
  if (g_settings.debugging.dump_cpu_to_vram_copies)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("cpu_to_vram_copy_%u.png", s_cpu_to_vram_dump_id++).c_str(),
                   m_vram_transfer.width, m_vram_transfer.height, sizeof(u16) * m_vram_transfer.width,
                   m_blit_buffer.data(), true);
  }

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  // TODO: skip this copy
  const u32 num_words = static_cast<u32>(m_blit_buffer.size()) * 2u;
  GPUBackendUpdateVRAMCommand* cmd = g_gpu_backend->NewUpdateVRAMCommand(num_words);
  FillBackendCommandParameters(cmd);
  cmd->x = m_vram_transfer.x;
  cmd->y = m_vram_transfer.y;
  cmd->width = m_vram_transfer.width;
  cmd->height = m_vram_transfer.height;
  std::memcpy(cmd->data, m_blit_buffer.data(), sizeof(u16) * num_words);
  g_gpu_backend->PushCommand(cmd);

  m_blit_buffer.clear();
  m_vram_transfer = {};
  m_blitter_state = BlitterState::Idle;
  m_stats.num_vram_writes++;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

  m_vram_transfer.x = Truncate16(FifoPeek() & VRAM_COORD_MASK);
  m_vram_transfer.y = Truncate16((FifoPop() >> 16) & VRAM_COORD_MASK);
  m_vram_transfer.width = ((Truncate16(FifoPeek()) - 1) & VRAM_WIDTH_MASK) + 1;
  m_vram_transfer.height = ((Truncate16(FifoPop() >> 16) - 1) & VRAM_HEIGHT_MASK) + 1;

  Log_DebugPrintf("Copy rectangle from VRAM to CPU offset=(%u,%u), size=(%u,%u)", m_vram_transfer.x, m_vram_transfer.y,
                  m_vram_transfer.width, m_vram_transfer.height);
  DebugAssert(m_vram_transfer.col == 0 && m_vram_transfer.row == 0);

  // ensure VRAM shadow is up to date
  GPUBackendReadVRAMCommand* cmd = g_gpu_backend->NewReadVRAMCommand();
  cmd->x = m_vram_transfer.x;
  cmd->y = m_vram_transfer.y;
  cmd->width = m_vram_transfer.width;
  cmd->height = m_vram_transfer.height;
  g_gpu_backend->PushCommand(cmd);
  g_gpu_backend->Sync();

  if (g_settings.debugging.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("vram_to_cpu_copy_%u.png", s_vram_to_cpu_dump_id++).c_str(),
                   m_vram_transfer.width, m_vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   g_gpu_backend->GetVRAM() + (m_vram_transfer.y * VRAM_WIDTH + m_vram_transfer.x), true);
  }

  // switch to pixel-by-pixel read state
  m_stats.num_vram_reads++;
  m_blitter_state = BlitterState::ReadingVRAM;
  m_command_total_words = 0;
  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand()
{
  CHECK_COMMAND_SIZE(4);
  m_fifo.RemoveOne();

  GPUBackendCopyVRAMCommand* cmd = g_gpu_backend->NewCopyVRAMCommand();
  cmd->src_x = Truncate16(FifoPeek() & VRAM_COORD_MASK);
  cmd->src_y = Truncate16((FifoPop() >> 16) & VRAM_COORD_MASK);
  cmd->dst_x = Truncate16(FifoPeek() & VRAM_COORD_MASK);
  cmd->dst_y = Truncate16((FifoPop() >> 16) & VRAM_COORD_MASK);
  cmd->width = Truncate16(ReplaceZero(FifoPeek() & VRAM_WIDTH_MASK, 0x400));
  cmd->height = Truncate16(ReplaceZero((FifoPop() >> 16) & VRAM_HEIGHT_MASK, 0x200));

  Log_DebugPrintf("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u)", cmd->src_x, cmd->src_y,
                  cmd->dst_x, cmd->dst_y, cmd->width, cmd->height);

  AddCommandTicks(ZeroExtend32(cmd->width) * ZeroExtend32(cmd->height) * 2);

  g_gpu_backend->PushCommand(cmd);

  m_stats.num_vram_copies++;
  EndCommand();
  return true;
}
