#pragma once
#include "gpu.h"
#include "gpu_backend.h"
#include <array>
#include <memory>
#include <vector>

class HostDisplayTexture;

class GPU_SW final : public GPUBackend
{
public:
  GPU_SW();
  ~GPU_SW() override;

  bool IsHardwareRenderer() const override;

  bool Initialize() override;
  void Reset() override;

  u16 GetPixel(u32 x, u32 y) const { return m_vram[VRAM_WIDTH * y + x]; }
  const u16* GetPixelPtr(u32 x, u32 y) const { return &m_vram[VRAM_WIDTH * y + x]; }
  u16* GetPixelPtr(u32 x, u32 y) { return &m_vram[VRAM_WIDTH * y + x]; }
  void SetPixel(u32 x, u32 y, u16 value) { m_vram[VRAM_WIDTH * y + x] = value; }

  // this is actually (31 * 255) >> 4) == 494, but to simplify addressing we use the next power of two (512)
  static constexpr u32 DITHER_LUT_SIZE = 512;
  using DitherLUT = std::array<std::array<std::array<u8, 512>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
  static constexpr DitherLUT ComputeDitherLUT();

protected:
  static constexpr u8 Convert5To8(u8 x5) { return (x5 << 3) | (x5 & 7); }
  static constexpr u8 Convert8To5(u8 x8) { return (x8 >> 3); }

  union VRAMPixel
  {
    u16 bits;

    BitField<u16, u8, 0, 5> r;
    BitField<u16, u8, 5, 5> g;
    BitField<u16, u8, 10, 5> b;
    BitField<u16, bool, 15, 1> c;

    u8 GetR8() const { return Convert5To8(r); }
    u8 GetG8() const { return Convert5To8(g); }
    u8 GetB8() const { return Convert5To8(b); }

    void Set(u8 r_, u8 g_, u8 b_, bool c_ = false)
    {
      bits = (ZeroExtend16(r_)) | (ZeroExtend16(g_) << 5) | (ZeroExtend16(b_) << 10) | (static_cast<u16>(c_) << 15);
    }

    void ClampAndSet(u8 r_, u8 g_, u8 b_, bool c_ = false)
    {
      Set(std::min<u8>(r_, 0x1F), std::min<u8>(g_, 0x1F), std::min<u8>(b_, 0x1F), c_);
    }

    void SetRGB24(u32 rgb24, bool c_ = false)
    {
      bits = Truncate16(((rgb24 >> 3) & 0x1F) | (((rgb24 >> 11) & 0x1F) << 5) | (((rgb24 >> 19) & 0x1F) << 10)) |
             (static_cast<u16>(c_) << 15);
    }

    void SetRGB24(u8 r8, u8 g8, u8 b8, bool c_ = false)
    {
      bits = (ZeroExtend16(r8 >> 3)) | (ZeroExtend16(g8 >> 3) << 5) | (ZeroExtend16(b8 >> 3) << 10) |
             (static_cast<u16>(c_) << 15);
    }

    void SetRGB24Dithered(u32 x, u32 y, u8 r8, u8 g8, u8 b8, bool c_ = false)
    {
      const s32 offset = GPU::DITHER_MATRIX[y & 3][x & 3];
      r8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(r8)) + offset, 0, 255));
      g8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(g8)) + offset, 0, 255));
      b8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(b8)) + offset, 0, 255));
      SetRGB24(r8, g8, b8, c_);
    }

    u32 ToRGB24() const
    {
      const u32 r_ = ZeroExtend32(r.GetValue());
      const u32 g_ = ZeroExtend32(g.GetValue());
      const u32 b_ = ZeroExtend32(b.GetValue());

      return ((r_ << 3) | (r_ & 7)) | (((g_ << 3) | (g_ & 7)) << 8) | (((b_ << 3) | (b_ & 7)) << 16);
    }
  };

  //////////////////////////////////////////////////////////////////////////
  // Scanout
  //////////////////////////////////////////////////////////////////////////
  void CopyOut15Bit(u32 src_x, u32 src_y, u32* dst_ptr, u32 dst_stride, u32 width, u32 height, bool interlaced,
                    bool interleaved);
  void CopyOut24Bit(u32 src_x, u32 src_y, u32* dst_ptr, u32 dst_stride, u32 width, u32 height, bool interlaced,
                    bool interleaved);
  void ClearDisplay() override;
  void UpdateDisplay() override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                GPUBackendCommandParameters params) override;

  //////////////////////////////////////////////////////////////////////////
  // Rasterization
  //////////////////////////////////////////////////////////////////////////

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) override;
  void FlushRender() override;

  static bool IsClockwiseWinding(const GPUBackendDrawPolygonCommand::Vertex* v0, const GPUBackendDrawPolygonCommand::Vertex* v1,
                                 const GPUBackendDrawPolygonCommand::Vertex* v2);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
  void ShadePixel(const GPUBackendDrawCommand* cmd, u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x,
                  u8 texcoord_y);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawTriangle(const GPUBackendDrawPolygonCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                    const GPUBackendDrawPolygonCommand::Vertex* v1, const GPUBackendDrawPolygonCommand::Vertex* v2);

  using DrawTriangleFunction = void (GPU_SW::*)(const GPUBackendDrawPolygonCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                                                const GPUBackendDrawPolygonCommand::Vertex* v1,
                                                const GPUBackendDrawPolygonCommand::Vertex* v2);
  DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable, bool raw_texture_enable,
                                               bool transparency_enable, bool dithering_enable);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd);

  using DrawRectangleFunction = void (GPU_SW::*)(const GPUBackendDrawRectangleCommand* cmd);
  DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                 bool transparency_enable);

  template<bool shading_enable, bool transparency_enable, bool dithering_enable>
  void DrawLine(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0, const GPUBackendDrawLineCommand::Vertex* p1);

  using DrawLineFunction = void (GPU_SW::*)(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                                            const GPUBackendDrawLineCommand::Vertex* p1);
  DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable, bool dithering_enable);

  std::vector<u32> m_display_texture_buffer;
  std::unique_ptr<HostDisplayTexture> m_display_texture;

  std::array<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram;
};
