#pragma once
#include "common/heap_array.h"
#include "common/event.h"
#include "gpu_types.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class StateWrapper;

class GPUBackend
{
public:
  GPUBackend();
  virtual ~GPUBackend();

  ALWAYS_INLINE u16* GetVRAM() const { return m_vram_ptr; }

  static bool Create(GPURenderer backend);

  virtual bool Initialize();

  // Graphics API state reset/restore - call when drawing the UI etc.
  virtual void ResetGraphicsAPIState();
  virtual void RestoreGraphicsAPIState();

  virtual bool IsHardwareRenderer() const;

  /// Recompile shaders/recreate framebuffers when needed.
  virtual void UpdateSettings();

  /// Updates the resolution scale when it's set to automatic.
  virtual void UpdateResolutionScale();

  /// Returns the effective display resolution of the GPU.
  virtual std::tuple<u32, u32> GetEffectiveDisplayResolution();

  virtual void DrawRendererStats(bool is_idle_frame);

  bool DoState(StateWrapper& sw);

  GPUBackendResetCommand* NewResetCommand();
  GPUBackendUpdateSettingsCommand* NewUpdateSettingsCommand();
  GPUBackendUpdateResolutionScaleCommand* NewUpdateResolutionScaleCommand();
  GPUBackendReadVRAMCommand* NewReadVRAMCommand();
  GPUBackendFillVRAMCommand* NewFillVRAMCommand();
  GPUBackendUpdateVRAMCommand* NewUpdateVRAMCommand(u32 num_words);
  GPUBackendCopyVRAMCommand* NewCopyVRAMCommand();
  GPUBackendSetDrawingAreaCommand* NewSetDrawingAreaCommand();
  GPUBackendDrawPolygonCommand* NewDrawPolygonCommand(u32 num_vertices);
  GPUBackendDrawRectangleCommand* NewDrawRectangleCommand();
  GPUBackendDrawLineCommand* NewDrawLineCommand(u32 num_vertices);
  GPUBackendClearDisplayCommand* NewClearDisplayCommand();
  GPUBackendUpdateDisplayCommand* NewUpdateDisplayCommand();
  GPUBackendFlushRenderCommand* NewFlushRenderCommand();

  void PushCommand(GPUBackendCommand* cmd);
  void Sync();

  /// Processes all pending GPU commands.
  void ProcessGPUCommands();

  void CPUFrameDone();
  void RunGPUFrame();
  void EndGPUFrame();

protected:
  void* AllocateCommand(u32 size);
  u32 GetPendingCommandSize() const;
  void WakeGPUThread();

  virtual void Reset();
  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height) = 0;
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) = 0;
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                          GPUBackendCommandParameters params) = 0;
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                        GPUBackendCommandParameters params) = 0;
  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;
  virtual void SetScissorFromDrawingArea();
  virtual void ClearDisplay() = 0;
  virtual void UpdateDisplay() = 0;
  virtual void FlushRender() = 0;

  void HandleCommand(const GPUBackendCommand* cmd);

  void SoftwareFillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params);
  void SoftwareUpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params);
  void SoftwareCopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                        GPUBackendCommandParameters params);

  u16* m_vram_ptr = nullptr;

  Common::Rectangle<u32> m_drawing_area{};

  float m_display_aspect_ratio = 1.0f;

  // Size of the simulated screen in pixels. Depending on crop mode, this may include overscan area.
  u16 m_display_width = 0;
  u16 m_display_height = 0;

  // Top-left corner where the VRAM is displayed. Depending on the CRTC config, this may indicate padding.
  u16 m_display_origin_left = 0;
  u16 m_display_origin_top = 0;

  // Rectangle describing the displayed area of VRAM, in coordinates.
  u16 m_display_vram_left = 0;
  u16 m_display_vram_top = 0;
  u16 m_display_vram_width = 0;
  u16 m_display_vram_height = 0;
  u16 m_display_vram_start_x = 0;
  u16 m_display_vram_start_y = 0;

  GPUInterlacedDisplayMode m_display_interlace = GPUInterlacedDisplayMode::None;
  u8 m_display_interlace_field = 0;
  bool m_display_enabled = false;
  bool m_display_24bit = false;

  bool m_frame_done = false;

  Common::Event m_sync_event;
  std::atomic_bool m_gpu_thread_sleeping{ false };
  
  std::mutex m_sync_mutex;
  std::condition_variable m_sync_cpu_thread_cv;
  std::condition_variable m_wake_gpu_thread_cv;
  bool m_sync_done = false;

  enum : u32
  {
    COMMAND_QUEUE_SIZE = 8 * 1024 * 1024,
    THRESHOLD_TO_WAKE_GPU = 256
  };

  HeapArray<u8, COMMAND_QUEUE_SIZE> m_command_fifo_data;
  alignas(64) std::atomic<u32> m_command_fifo_read_ptr{0};
  alignas(64) std::atomic<u32> m_command_fifo_write_ptr{0};
};

extern std::unique_ptr<GPUBackend> g_gpu_backend;