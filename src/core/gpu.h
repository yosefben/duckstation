#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/rectangle.h"
#include "gpu_types.h"
#include "timers.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

class StateWrapper;

class TimingEvent;
class Timers;

class GPUBackend;

class GPU final
{
public:
  enum class BlitterState : u8
  {
    Idle,
    ReadingVRAM,
    WritingVRAM,
    DrawingPolyLine
  };

  enum class DMADirection : u32
  {
    Off = 0,
    FIFO = 1,
    CPUtoGP0 = 2,
    GPUREADtoCPU = 3
  };

  enum : u32
  {
    MAX_FIFO_SIZE = 4096,
    DOT_TIMER_INDEX = 0,
    HBLANK_TIMER_INDEX = 1,
    MAX_RESOLUTION_SCALE = 16
  };

  enum : u16
  {
    NTSC_TICKS_PER_LINE = 3413,
    NTSC_HSYNC_TICKS = 200,
    NTSC_TOTAL_LINES = 263,
    PAL_TICKS_PER_LINE = 3406,
    PAL_HSYNC_TICKS = 200, // actually one more on odd lines
    PAL_TOTAL_LINES = 314,
  };

  // 4x4 dither matrix.
  static constexpr s32 DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = {{-4, +0, -3, +1},  // row 0
                                                                                {+2, -2, +3, -1},  // row 1
                                                                                {-3, +1, -4, +0},  // row 2
                                                                                {+4, -1, +2, -2}}; // row 3

  // Base class constructor.
  GPU();
  ~GPU();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  // Render statistics debug window.
  void DrawDebugStateWindow();

  // MMIO access
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  void DMARead(u32* words, u32 word_count);

  ALWAYS_INLINE bool BeginDMAWrite() const { return (m_GPUSTAT.dma_direction == DMADirection::CPUtoGP0); }
  ALWAYS_INLINE void DMAWrite(u32 address, u32 value)
  {
    m_fifo.Push((ZeroExtend64(address) << 32) | ZeroExtend64(value));
  }
  void EndDMAWrite();

  /// Returns false if the DAC is loading any data from VRAM.
  ALWAYS_INLINE bool IsDisplayDisabled() const
  {
    return m_GPUSTAT.display_disable || m_crtc_state.display_vram_width == 0 || m_crtc_state.display_vram_height == 0;
  }

  /// Returns true if scanout should be interlaced.
  ALWAYS_INLINE bool IsInterlacedDisplayEnabled() const
  {
    return (!m_force_progressive_scan) & m_GPUSTAT.vertical_interlace;
  }

  /// Returns true if interlaced rendering is enabled and force progressive scan is disabled.
  ALWAYS_INLINE bool IsInterlacedRenderingEnabled() const
  {
    return (!m_force_progressive_scan) & m_GPUSTAT.SkipDrawingToActiveField();
  }

  /// Returns the interlaced mode to use when scanning out/displaying.
  ALWAYS_INLINE GPUInterlacedDisplayMode GetInterlacedDisplayMode() const
  {
    if (IsInterlacedDisplayEnabled())
    {
      return m_GPUSTAT.vertical_resolution ? GPUInterlacedDisplayMode::InterleavedFields :
                                             GPUInterlacedDisplayMode::SeparateFields;
    }
    else
    {
      return GPUInterlacedDisplayMode::None;
    }
  }

  /// Returns the number of pending GPU ticks.
  TickCount GetPendingCRTCTicks() const;
  TickCount GetPendingCommandTicks() const;

  /// Returns true if enough ticks have passed for the raster to be on the next line.
  bool IsCRTCScanlinePending() const;

  /// Returns true if a raster scanline or command execution is pending.
  bool IsCommandCompletionPending() const;

  /// Synchronizes the CRTC, updating the hblank timer.
  void SynchronizeCRTC();

  /// Recompile shaders/recreate framebuffers when needed.
  void UpdateSettings();



  // Converts window coordinates into horizontal ticks and scanlines. Returns false if out of range. Used for lightguns.
  bool ConvertScreenCoordinatesToBeamTicksAndLines(s32 window_x, s32 window_y, u32* out_tick, u32* out_line) const;

  // Returns the video clock frequency.
  TickCount GetCRTCFrequency() const;

private:
  TickCount CRTCTicksToSystemTicks(TickCount crtc_ticks, TickCount fractional_ticks) const;
  TickCount SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks) const;

  // The GPU internally appears to run at 2x the system clock.
  ALWAYS_INLINE static constexpr TickCount GPUTicksToSystemTicks(TickCount gpu_ticks)
  {
    return std::max<TickCount>(gpu_ticks >> 1, 1);
  }
  ALWAYS_INLINE static constexpr TickCount SystemTicksToGPUTicks(TickCount sysclk_ticks) { return sysclk_ticks << 1; }

  static bool DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer,
                             bool remove_alpha);

  void SoftReset();

  // Sets dots per scanline
  float ComputeHorizontalFrequency() const;
  float ComputeVerticalFrequency() const;
  void UpdateCRTCConfig();
  void UpdateCRTCDisplayParameters();

  // Update ticks for this execution slice
  void UpdateCRTCTickEvent();
  void UpdateCommandTickEvent();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateDMARequest();
  void UpdateGPUIdle();

  // Ticks for hblank/vblank.
  void CRTCTickEvent(TickCount ticks);
  void CommandTickEvent(TickCount ticks);

  u32 ReadGPUREAD();
  void FinishVRAMWrite();

  /// Returns the number of vertices in the buffered poly-line.
  ALWAYS_INLINE u32 GetPolyLineVertexCount() const
  {
    return (static_cast<u32>(m_blit_buffer.size()) + BoolToUInt32(m_render_command.shading_enable)) >>
           BoolToUInt8(m_render_command.shading_enable);
  }

  /// Returns true if the drawing area is valid (i.e. left <= right, top <= bottom).
  ALWAYS_INLINE bool IsDrawingAreaIsValid() const { return m_drawing_area.Valid(); }

  void AddCommandTicks(TickCount ticks);

  void WriteGP1(u32 value);
  void EndCommand();
  void ExecuteCommands();
  void HandleGetGPUInfoCommand(u32 value);

  // These are **very** approximate.
  ALWAYS_INLINE void AddDrawTriangleTicks(u32 width, u32 height, bool shaded, bool textured, bool semitransparent)
  {
    const u32 average_width = ((width + 2) / 3);
    u32 ticks_per_row = average_width;
    if (textured)
      ticks_per_row += average_width;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (average_width + 1u) / 2u;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(ticks_per_row * height);
  }
  ALWAYS_INLINE void AddDrawRectangleTicks(u32 width, u32 height, bool textured, bool semitransparent)
  {
    u32 ticks_per_row = width;
    if (textured)
      ticks_per_row += width;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (width + 1u) / 2u;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(ticks_per_row * height);
  }
  ALWAYS_INLINE void AddDrawLineTicks(u32 width, u32 height, bool shaded)
  {
    if (m_GPUSTAT.SkipDrawingToActiveField())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(std::max(width, height));
  }

  std::unique_ptr<TimingEvent> m_crtc_tick_event;
  std::unique_ptr<TimingEvent> m_command_tick_event;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, GPUTransparencyMode, 5, 2> semi_transparency_mode;
    BitField<u32, GPUTextureMode, 7, 2> texture_color_mode;
    BitField<u32, bool, 9, 1> dither_enable;
    BitField<u32, bool, 10, 1> draw_to_displayed_field;
    BitField<u32, bool, 11, 1> set_mask_while_drawing;
    BitField<u32, bool, 12, 1> check_mask_before_draw;
    BitField<u32, u8, 13, 1> interlaced_field;
    BitField<u32, bool, 14, 1> reverse_flag;
    BitField<u32, bool, 15, 1> texture_disable;
    BitField<u32, u8, 16, 1> horizontal_resolution_2;
    BitField<u32, u8, 17, 2> horizontal_resolution_1;
    BitField<u32, bool, 19, 1> vertical_resolution;
    BitField<u32, bool, 20, 1> pal_mode;
    BitField<u32, bool, 21, 1> display_area_color_depth_24;
    BitField<u32, bool, 22, 1> vertical_interlace;
    BitField<u32, bool, 23, 1> display_disable;
    BitField<u32, bool, 24, 1> interrupt_request;
    BitField<u32, bool, 25, 1> dma_data_request;
    BitField<u32, bool, 26, 1> gpu_idle;
    BitField<u32, bool, 27, 1> ready_to_send_vram;
    BitField<u32, bool, 28, 1> ready_to_recieve_dma;
    BitField<u32, DMADirection, 29, 2> dma_direction;
    BitField<u32, bool, 31, 1> display_line_lsb;

    bool IsMaskingEnabled() const
    {
      static constexpr u32 MASK = ((1 << 11) | (1 << 12));
      return ((bits & MASK) != 0);
    }
    bool SkipDrawingToActiveField() const
    {
      static constexpr u32 MASK = (1 << 19) | (1 << 22) | (1 << 10);
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & MASK) == ACTIVE);
    }
    bool InInterleaved480iMode() const
    {
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & ACTIVE) == ACTIVE);
    }

    // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
    u16 GetMaskAND() const
    {
      // return check_mask_before_draw ? 0x8000 : 0x0000;
      return Truncate16((bits << 3) & 0x8000);
    }
    u16 GetMaskOR() const
    {
      // return set_mask_while_drawing ? 0x8000 : 0x0000;
      return Truncate16((bits << 4) & 0x8000);
    }
  } m_GPUSTAT = {};

  struct DrawingOffset
  {
    s32 x;
    s32 y;
  } m_drawing_offset = {};

  Common::Rectangle<u32> m_drawing_area{0, 0, VRAM_WIDTH, VRAM_HEIGHT};
  GPUDrawModeReg m_draw_mode{};
  GPUTextureWindow m_texture_window{};

  bool m_console_is_pal = false;
  bool m_set_texture_disable_mask = false;
  bool m_force_progressive_scan = false;
  bool m_force_ntsc_timings = false;

  struct CRTCState
  {
    struct Regs
    {
      static constexpr u32 DISPLAY_ADDRESS_START_MASK = 0b111'11111111'11111110;
      static constexpr u32 HORIZONTAL_DISPLAY_RANGE_MASK = 0b11111111'11111111'11111111;
      static constexpr u32 VERTICAL_DISPLAY_RANGE_MASK = 0b1111'11111111'11111111;

      union
      {
        u32 display_address_start;
        BitField<u32, u16, 0, 10> X;
        BitField<u32, u16, 10, 9> Y;
      };
      union
      {
        u32 horizontal_display_range;
        BitField<u32, u16, 0, 12> X1;
        BitField<u32, u16, 12, 12> X2;
      };

      union
      {
        u32 vertical_display_range;
        BitField<u32, u16, 0, 10> Y1;
        BitField<u32, u16, 10, 10> Y2;
      };
    } regs;

    u16 dot_clock_divider;

    // Size of the simulated screen in pixels. Depending on crop mode, this may include overscan area.
    u16 display_width;
    u16 display_height;

    // Top-left corner where the VRAM is displayed. Depending on the CRTC config, this may indicate padding.
    u16 display_origin_left;
    u16 display_origin_top;

    // Rectangle describing the displayed area of VRAM, in coordinates.
    u16 display_vram_left;
    u16 display_vram_top;
    u16 display_vram_width;
    u16 display_vram_height;

    u16 horizontal_total;
    u16 horizontal_sync_start; // <- not currently saved to state, so we don't have to bump the version
    u16 horizontal_active_start;
    u16 horizontal_active_end;
    u16 horizontal_display_start;
    u16 horizontal_display_end;
    u16 vertical_total;
    u16 vertical_active_start;
    u16 vertical_active_end;
    u16 vertical_display_start;
    u16 vertical_display_end;

    TickCount fractional_ticks;
    TickCount current_tick_in_scanline;
    u32 current_scanline;

    float display_aspect_ratio;
    bool in_hblank;
    bool in_vblank;

    u8 interlaced_field; // 0 = odd, 1 = even
    u8 interlaced_display_field;
    u8 active_line_lsb;
  } m_crtc_state = {};

  BlitterState m_blitter_state = BlitterState::Idle;
  u32 m_command_total_words = 0;
  TickCount m_pending_command_ticks = 0;

  /// GPUREAD value for non-VRAM-reads.
  u32 m_GPUREAD_latch = 0;

  /// True if currently executing/syncing.
  bool m_syncing = false;
  bool m_fifo_pushed = false;

  struct VRAMTransfer
  {
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 col;
    u16 row;
  } m_vram_transfer = {};

  HeapFIFOQueue<u64, MAX_FIFO_SIZE> m_fifo;
  std::vector<u32> m_blit_buffer;
  u32 m_blit_remaining_words;
  GPURenderCommand m_render_command{};

  ALWAYS_INLINE u32 FifoPop() { return Truncate32(m_fifo.Pop()); }
  ALWAYS_INLINE u32 FifoPeek() { return Truncate32(m_fifo.Peek()); }
  ALWAYS_INLINE u32 FifoPeek(u32 i) { return Truncate32(m_fifo.Peek(i)); }

  TickCount m_max_run_ahead = 128;
  u32 m_fifo_size = 128;

  struct Stats
  {
    u32 num_vram_reads;
    u32 num_vram_fills;
    u32 num_vram_writes;
    u32 num_vram_copies;
    u32 num_vertices;
    u32 num_polygons;
  };
  Stats m_stats = {};
  Stats m_last_stats = {};

  using GP0CommandHandler = bool (GPU::*)();
  using GP0CommandHandlerTable = std::array<GP0CommandHandler, 256>;
  static GP0CommandHandlerTable GenerateGP0CommandHandlerTable();

  void ClearDisplay();
  void UpdateDisplay();
  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;
  void UpdateDrawingArea();
  void FlushRender();

  // Rendering commands, returns false if not enough data is provided
  bool HandleUnknownGP0Command();
  bool HandleNOPCommand();
  bool HandleClearCacheCommand();
  bool HandleInterruptRequestCommand();
  bool HandleSetDrawModeCommand();
  bool HandleSetTextureWindowCommand();
  bool HandleSetDrawingAreaTopLeftCommand();
  bool HandleSetDrawingAreaBottomRightCommand();
  bool HandleSetDrawingOffsetCommand();
  bool HandleSetMaskBitCommand();
  bool HandleRenderPolygonCommand();
  bool HandleRenderRectangleCommand();
  bool HandleRenderLineCommand();
  bool HandleRenderPolyLineCommand();
  void FinishPolyLineRenderCommand();
  bool HandleFillRectangleCommand();
  bool HandleCopyRectangleCPUToVRAMCommand();
  bool HandleCopyRectangleVRAMToCPUCommand();
  bool HandleCopyRectangleVRAMToVRAMCommand();

  static const GP0CommandHandlerTable s_GP0_command_handler_table;
};

extern GPU g_gpu;
