#include "gpu_hw.h"
#include "system.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include <sstream>
Log_SetChannel(GPU_HW);

GPU_HW::GPU_HW() = default;

GPU_HW::~GPU_HW() = default;

void GPU_HW::Reset()
{
  GPU::Reset();

  m_batch = {};
}

bool GPU_HW::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers)
{
  if (!GPU::Initialize(system, dma, interrupt_controller, timers))
    return false;

  m_use_bilinear_filtering = m_system->GetSettings().gpu_bilinear_filtering;
  return true;
}

void GPU_HW::UpdateSettings()
{
  GPU::UpdateSettings();

  m_use_bilinear_filtering = m_system->GetSettings().gpu_bilinear_filtering;
}

void GPU_HW::LoadVertices(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  const u32 texpage =
    ZeroExtend32(m_render_state.texpage_attribute) | (ZeroExtend32(m_render_state.texlut_attribute) << 16);

  // TODO: Move this to the GPU..
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      bool restart_strip = (rc.quad_polygon && !m_batch.vertices.empty());
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        HWVertex hw_vert;
        hw_vert.color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;

        const VertexPosition vp{command_ptr[buffer_pos++]};
        hw_vert.x = vp.x;
        hw_vert.y = vp.y;
        hw_vert.texpage = texpage;

        if (textured)
        {
          const auto [texcoord_x, texcoord_y] = UnpackTexcoord(Truncate16(command_ptr[buffer_pos++]));
          hw_vert.texcoord = HWVertex::PackTexcoord(texcoord_x, texcoord_y);
        }
        else
        {
          hw_vert.texcoord = 0;
        }

        m_batch.vertices.push_back(hw_vert);
        if (restart_strip)
        {
          m_batch.vertices.push_back(m_batch.vertices.back());
          restart_strip = false;
        }
      }
    }
    break;

    case Primitive::Rectangle:
    {
      // if we're drawing quads, we need to create a degenerate triangle to restart the triangle strip
      const bool restart_strip = !m_batch.vertices.empty();
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());

      u32 buffer_pos = 1;
      const u32 color = rc.color_for_first_vertex;
      const VertexPosition vp{command_ptr[buffer_pos++]};
      const s32 pos_left = vp.x;
      const s32 pos_top = vp.y;
      const auto [texcoord_x, texcoord_y] =
        UnpackTexcoord(rc.texture_enable ? Truncate16(command_ptr[buffer_pos++]) : 0);
      const u16 tex_left = ZeroExtend16(texcoord_x);
      const u16 tex_top = ZeroExtend16(texcoord_y);
      u32 rectangle_width;
      u32 rectangle_height;
      switch (rc.rectangle_size)
      {
        case DrawRectangleSize::R1x1:
          rectangle_width = 1;
          rectangle_height = 1;
          break;
        case DrawRectangleSize::R8x8:
          rectangle_width = 8;
          rectangle_height = 8;
          break;
        case DrawRectangleSize::R16x16:
          rectangle_width = 16;
          rectangle_height = 16;
          break;
        default:
          rectangle_width = command_ptr[buffer_pos] & 0xFFFF;
          rectangle_height = command_ptr[buffer_pos] >> 16;
          break;
      }

      // TODO: This should repeat the texcoords instead of stretching
      const s32 pos_right = pos_left + static_cast<s32>(rectangle_width);
      const s32 pos_bottom = pos_top + static_cast<s32>(rectangle_height);
      const u16 tex_right = tex_left + static_cast<u16>(rectangle_width);
      const u16 tex_bottom = tex_top + static_cast<u16>(rectangle_height);

      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_top, color, texpage, HWVertex::PackTexcoord(tex_left, tex_top)});
      if (restart_strip)
        m_batch.vertices.push_back(m_batch.vertices.back());
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_top, color, texpage, HWVertex::PackTexcoord(tex_right, tex_top)});
      m_batch.vertices.push_back(
        HWVertex{pos_left, pos_bottom, color, texpage, HWVertex::PackTexcoord(tex_left, tex_bottom)});
      m_batch.vertices.push_back(
        HWVertex{pos_right, pos_bottom, color, texpage, HWVertex::PackTexcoord(tex_right, tex_bottom)});
    }
    break;

    case Primitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      u32 buffer_pos = 1;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 color = (shaded && i > 0) ? (command_ptr[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color;
        const VertexPosition vp{command_ptr[buffer_pos++]};
        m_batch.vertices.push_back(HWVertex{vp.x.GetValue(), vp.y.GetValue(), color});
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<u32>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<u32>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

static void DefineMacro(std::stringstream& ss, const char* name, bool enabled)
{
  if (enabled)
    ss << "#define " << name << " 1\n";
  else
    ss << "/* #define " << name << " 0 */\n";
}

void GPU_HW::GenerateShaderHeader(std::stringstream& ss)
{
  ss << "#version 330 core\n\n";
  ss << "const int RESOLUTION_SCALE = " << m_resolution_scale << ";\n";
  ss << "const ivec2 VRAM_SIZE = ivec2(" << VRAM_WIDTH << ", " << VRAM_HEIGHT << ") * RESOLUTION_SCALE;\n";
  ss << "const vec2 RCP_VRAM_SIZE = vec2(1.0, 1.0) / vec2(VRAM_SIZE);\n";
  ss << R"(

float fixYCoord(float y)
{
  return 1.0 - RCP_VRAM_SIZE.y - y;
}

int fixYCoord(int y)
{
  return VRAM_SIZE.y - y - 1;
}

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(v.r * 255.0) >> 3;
  uint g = uint(v.g * 255.0) >> 3;
  uint b = uint(v.b * 255.0) >> 3;
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

vec4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 0x1Fu);
  uint g = ((v >> 5) & 0x1Fu);
  uint b = ((v >> 10) & 0x1Fu);
  uint a = ((v >> 15) & 0x01u);

  return vec4(float(r) * 255.0, float(g) * 255.0, float(b) * 255.0, float(a) * 255.0);
}
)";
}

std::string GPU_HW::GenerateVertexShader(bool textured)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TEXTURED", textured);

  ss << R"(
in ivec2 a_pos;
in vec4 a_col0;
in int a_texcoord;
in int a_texpage;

out vec3 v_col0;
#if TEXTURED
  out vec2 v_tex0;
  flat out ivec4 v_texpage;
#endif

uniform ivec2 u_pos_offset;

void main()
{
  // 0..+1023 -> -1..1
  float pos_x = (float(a_pos.x + u_pos_offset.x) / 512.0) - 1.0;
  float pos_y = (float(a_pos.y + u_pos_offset.y) / -256.0) + 1.0;
  gl_Position = vec4(pos_x, pos_y, 0.0, 1.0);

  v_col0 = a_col0.rgb;
  #if TEXTURED
    v_tex0 = vec2(float(a_texcoord & 0xFFFF), float(a_texcoord >> 16));// / vec2(255.0);

    // base_x,base_y,palette_x,palette_y
    v_texpage.x = (a_texpage & 15) * 64;
    v_texpage.y = ((a_texpage >> 4) & 1) * 256;
    v_texpage.z = ((a_texpage >> 16) & 63) * 16;
    v_texpage.w = ((a_texpage >> 22) & 511);
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFragmentShader(TransparencyRenderMode transparency, bool textured,
                                           TextureColorMode texture_color_mode, bool blending)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "TRANSPARENT", transparency != TransparencyRenderMode::Off);
  DefineMacro(ss, "TRANSPARENT_ONLY_OPAQUE", transparency == TransparencyRenderMode::OnlyOpaque);
  DefineMacro(ss, "TRANSPARENT_ONLY_TRANSPARENT", transparency == TransparencyRenderMode::OnlyTransparent);
  DefineMacro(ss, "TEXTURED", textured);
  DefineMacro(ss, "PALETTE",
              textured && (texture_color_mode == GPU::TextureColorMode::Palette4Bit ||
                           texture_color_mode == GPU::TextureColorMode::Palette8Bit));
  DefineMacro(ss, "PALETTE_4_BIT", textured && texture_color_mode == GPU::TextureColorMode::Palette4Bit);
  DefineMacro(ss, "PALETTE_8_BIT", textured && texture_color_mode == GPU::TextureColorMode::Palette8Bit);
  DefineMacro(ss, "BLENDING", blending);
  DefineMacro(ss, "BILINEAR_FILTERING", textured && m_use_bilinear_filtering);
  DefineMacro(ss, "USE_DUAL_SOURCE_BLEND",
              transparency != TransparencyRenderMode::Off || (textured && m_use_bilinear_filtering));

  ss << R"(
const vec4 TRANSPARENT_PIXEL_COLOR = vec4(0.0, 0.0, 0.0, 0.0);

in vec3 v_col0;
uniform vec2 u_transparent_alpha;
#if TEXTURED
  in vec2 v_tex0;
  flat in ivec4 v_texpage;
  uniform sampler2D samp0;
  uniform uvec4 u_texture_window;
#endif

out vec4 o_col0;    // Written to the framebuffer.

#if USE_DUAL_SOURCE_BLEND
out vec4 o_col1;    // Used in the blend equation.
#endif

#if TEXTURED
ivec2 ApplyTextureWindow(ivec2 coords)
{
  uint x = (uint(coords.x) & ~(u_texture_window.x * 8u)) | ((u_texture_window.z & u_texture_window.x) * 8u);
  uint y = (uint(coords.y) & ~(u_texture_window.y * 8u)) | ((u_texture_window.w & u_texture_window.y) * 8u);
  return ivec2(int(x), int(y));
}

vec4 SampleVRAMCoordinates(ivec2 icoord)
{
  // adjust for tightly packed palette formats
  ivec2 index_coord = icoord;
  #if PALETTE_4_BIT
    index_coord.x /= 4;
  #elif PALETTE_8_BIT
    index_coord.x /= 2;
  #endif

  // fixup coords
  ivec2 vicoord = ivec2((v_texpage.x + index_coord.x) * RESOLUTION_SCALE,
                        fixYCoord((v_texpage.y + index_coord.y) * RESOLUTION_SCALE));

  // load colour/palette
  vec4 color = texelFetch(samp0, vicoord, 0);

  // apply palette
  #if PALETTE
    #if PALETTE_4_BIT
      int subpixel = int(icoord.x) & 3;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 4)) & 0x0Fu);
    #elif PALETTE_8_BIT
      int subpixel = int(icoord.x) & 1;
      uint vram_value = RGBA8ToRGBA5551(color);
      int palette_index = int((vram_value >> (subpixel * 8)) & 0xFFu);
    #endif
    ivec2 palette_icoord = ivec2((v_texpage.z + palette_index) * RESOLUTION_SCALE,
                                 fixYCoord(v_texpage.w * RESOLUTION_SCALE));
    color = texelFetch(samp0, palette_icoord, 0);
  #endif

  return color;
}

struct VRAMSampleResult
{
  vec4 color;
  float mask;
};

VRAMSampleResult SampleFromVRAM(vec2 coord)
{
  // from 0..1 to 0..255
  ivec2 icoord = ivec2(coord * vec2(255.0));
  icoord = ApplyTextureWindow(icoord);
  vec4 sample = SampleVRAMCoordinates(icoord);

  VRAMSampleResult res;
  res.color = vec4(sample.xyz, float(sample != TRANSPARENT_PIXEL_COLOR));
  res.mask = sample.a;
  return res;
}

VRAMSampleResult SampleFromVRAMFiltered(vec2 coords)
{
  vec2 ficoords = coords - vec2(0.5);
  vec2 s = fract(ficoords);
  ivec2 icoord = ivec2(ficoords);

  // Take 4 samples.
  vec4 tl = SampleVRAMCoordinates(ApplyTextureWindow(icoord));
  vec4 tr = SampleVRAMCoordinates(ApplyTextureWindow(icoord + ivec2(1, 0)));
  vec4 bl = SampleVRAMCoordinates(ApplyTextureWindow(icoord + ivec2(0, 1)));
  vec4 br = SampleVRAMCoordinates(ApplyTextureWindow(icoord + ivec2(1, 1)));

  // Don't interpolate the alpha channel, since this is used for masking.
  VRAMSampleResult res;
  res.mask = min(tl.a, min(tr.a, min(bl.a, br.a)));

  // Compute alpha from how many texels aren't pixel color 0000h.
  tl.a = float(tl != TRANSPARENT_PIXEL_COLOR);
  tr.a = float(tr != TRANSPARENT_PIXEL_COLOR);
  bl.a = float(bl != TRANSPARENT_PIXEL_COLOR);
  br.a = float(br != TRANSPARENT_PIXEL_COLOR);
  res.color = mix(mix(tl, tr, s.x), mix(bl, br, s.x), s.y);
  //res.color.a = min(tl.a, min(tr.a, min(bl.a, br.a)));

  //res.color.rgb = vec3(s.x, s.y, 0.0);
  return res;
}

#endif

void main()
{
  #if TEXTURED
    #if BILINEAR_FILTERING
      VRAMSampleResult texcol = SampleFromVRAMFiltered(v_tex0);
    #else
      VRAMSampleResult texcol = SampleFromVRAM(v_tex0);
    #endif

    // Alpha culling for fully transparent pixels.
    float alpha = texcol.color.a;
    if (alpha == 0.0)
      discard;

    float new_alpha = texcol.mask;
    vec3 color;
    #if BLENDING
      color = vec3((ivec3(v_col0 * 255.0) * ivec3(texcol.color.rgb * 255.0)) >> 7) / 255.0;
    #else
      color = texcol.color.rgb;
    #endif

    #if TRANSPARENT
      // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.
      if (texcol.mask != 0.0)
      {
        #if TRANSPARENT_ONLY_OPAQUE
          discard;
        #endif
 
        // Blend the destination weight with the alpha from the incoming pixel.
        float src_alpha_factor = u_transparent_alpha.x * alpha;
        float dst_alpha_factor = u_transparent_alpha.y / alpha;
        o_col0 = vec4(color * src_alpha_factor, new_alpha);
        o_col1 = vec4(0.0, 0.0, 0.0, dst_alpha_factor);
      }
      else
      {
        #if TRANSPARENT_ONLY_TRANSPARENT
          discard;
        #endif

        o_col0 = vec4(color * alpha, new_alpha);
        o_col1 = vec4(0.0, 0.0, 0.0, 1.0 - alpha);
      }
    #else
      // Mask bit from texture.
      o_col0 = vec4(color * alpha, new_alpha);
      #if USE_DUAL_SOURCE_BLEND
        o_col1 = vec4(0.0, 0.0, 0.0, 1.0 - alpha);
      #endif
    #endif
  #else
    // Mask bit is cleared for untextured polygons.
    float new_alpha = 0.0;
    #if TRANSPARENT
      o_col0 = vec4(v_col0 * u_transparent_alpha.x, new_alpha);
      o_col1 = vec4(0.0, 0.0, 0.0, u_transparent_alpha.y);
    #else
      o_col0 = vec4(v_col0, new_alpha);
    #endif
  #endif
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateScreenQuadVertexShader()
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  ss << R"(

out vec2 v_tex0;

void main()
{
  v_tex0 = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(v_tex0 * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  gl_Position.y = -gl_Position.y;
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateFillFragmentShader()
{
  std::stringstream ss;
  GenerateShaderHeader(ss);

  ss << R"(
uniform vec4 fill_color;
out vec4 o_col0;

void main()
{
  o_col0 = fill_color;
}
)";

  return ss.str();
}

std::string GPU_HW::GenerateDisplayFragmentShader(bool depth_24bit, bool interlaced)
{
  std::stringstream ss;
  GenerateShaderHeader(ss);
  DefineMacro(ss, "DEPTH_24BIT", depth_24bit);
  DefineMacro(ss, "INTERLACED", interlaced);

  ss << R"(
in vec2 v_tex0;
out vec4 o_col0;

uniform sampler2D samp0;
uniform ivec3 u_base_coords;

ivec2 GetCoords(vec2 fragcoord)
{
  ivec2 icoords = ivec2(fragcoord);
  #if INTERLACED
    if (((icoords.y - u_base_coords.z) & 1) != 0)
      discard;
  #endif
  return icoords;
}

void main()
{
  ivec2 icoords = GetCoords(gl_FragCoord.xy);

  #if DEPTH_24BIT
    // compute offset in dwords from the start of the 24-bit values
    ivec2 base = ivec2(u_base_coords.x, u_base_coords.y + icoords.y);
    int xoff = int(icoords.x);
    int dword_index = (xoff / 2) + (xoff / 4);

    // sample two adjacent dwords, or four 16-bit values as the 24-bit value will lie somewhere between these
    uint s0 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + dword_index * 2 + 0, base.y), 0));
    uint s1 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + dword_index * 2 + 1, base.y), 0));
    uint s2 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + (dword_index + 1) * 2 + 0, base.y), 0));
    uint s3 = RGBA8ToRGBA5551(texelFetch(samp0, ivec2(base.x + (dword_index + 1) * 2 + 1, base.y), 0));

    // select the bit for this pixel depending on its offset in the 4-pixel block
    uint r, g, b;
    int block_offset = xoff & 3;
    if (block_offset == 0)
    {
      r = s0 & 0xFFu;
      g = s0 >> 8;
      b = s1 & 0xFFu;
    }
    else if (block_offset == 1)
    {
      r = s1 >> 8;
      g = s2 & 0xFFu;
      b = s2 >> 8;
    }
    else if (block_offset == 2)
    {
      r = s1 & 0xFFu;
      g = s1 >> 8;
      b = s2 & 0xFFu;
    }
    else
    {
      r = s2 >> 8;
      g = s3 & 0xFFu;
      b = s3 >> 8;
    }

    // and normalize
    o_col0 = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255, 1.0);
  #else
    // load and return
    o_col0 = texelFetch(samp0, u_base_coords.xy + icoords, 0);
  #endif
}
)";

  return ss.str();
}

GPU_HW::HWRenderBatch::Primitive GPU_HW::GetPrimitiveForCommand(RenderCommand rc)
{
  if (rc.primitive == Primitive::Line)
    return rc.polyline ? HWRenderBatch::Primitive::LineStrip : HWRenderBatch::Primitive::Lines;
  else if ((rc.primitive == Primitive::Polygon && rc.quad_polygon) || rc.primitive == Primitive::Rectangle)
    return HWRenderBatch::Primitive::TriangleStrip;
  else
    return HWRenderBatch::Primitive::Triangles;
}

void GPU_HW::InvalidateVRAMReadCache() {}

void GPU_HW::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr)
{
  if (rc.texture_enable)
  {
    // extract texture lut/page
    switch (rc.primitive)
    {
      case Primitive::Polygon:
      {
        if (rc.shading_enable)
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[5]);
        else
          m_render_state.SetFromPolygonTexcoord(command_ptr[2], command_ptr[4]);
      }
      break;

      case Primitive::Rectangle:
      {
        m_render_state.SetFromRectangleTexcoord(command_ptr[2]);
        m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
      }
      break;

      default:
        break;
    }
  }
  else
  {
    m_render_state.SetFromPageAttribute(Truncate16(m_GPUSTAT.bits));
  }

  // has any state changed which requires a new batch?
  const bool rc_transparency_enable = rc.IsTransparencyEnabled();
  const bool rc_texture_enable = rc.IsTextureEnabled();
  const bool rc_texture_blend_enable = rc.IsTextureBlendingEnabled();
  const HWRenderBatch::Primitive rc_primitive = GetPrimitiveForCommand(rc);
  const u32 max_added_vertices = num_vertices + 2;
  const bool buffer_overflow = (m_batch.vertices.size() + max_added_vertices) >= MAX_BATCH_VERTEX_COUNT;
  const bool rc_changed =
    m_batch.render_command_bits != rc.bits &&
    (m_batch.transparency_enable != rc_transparency_enable || m_batch.texture_enable != rc_texture_enable ||
     m_batch.texture_blending_enable != rc_texture_blend_enable || m_batch.primitive != rc_primitive);
  const bool restart_line_strip = (rc_primitive == HWRenderBatch::Primitive::LineStrip);
  const bool needs_flush =
    !IsFlushed() && (m_render_state.IsTextureColorModeChanged() || m_render_state.IsTransparencyModeChanged() ||
                     m_render_state.IsTextureWindowChanged() || buffer_overflow || rc_changed || restart_line_strip);
  if (needs_flush)
    FlushRender();

  // update state
  if (rc_changed)
  {
    m_batch.render_command_bits = rc.bits;
    m_batch.primitive = rc_primitive;
    m_batch.transparency_enable = rc_transparency_enable;
    m_batch.texture_enable = rc_texture_enable;
    m_batch.texture_blending_enable = rc_texture_blend_enable;
  }

  if (m_render_state.IsTexturePageChanged())
  {
    // we only need to update the copy texture if the render area intersects with the texture page
    const u32 texture_page_left = m_render_state.texture_page_x;
    const u32 texture_page_right = m_render_state.texture_page_y + TEXTURE_PAGE_WIDTH;
    const u32 texture_page_top = m_render_state.texture_page_y;
    const u32 texture_page_bottom = texture_page_top + TEXTURE_PAGE_HEIGHT;
    const bool texture_page_overlaps =
      (texture_page_left < m_drawing_area.right && texture_page_right > m_drawing_area.left &&
       texture_page_top > m_drawing_area.bottom && texture_page_bottom < m_drawing_area.top);

    // TODO: Check palette too.
    if (texture_page_overlaps)
    {
      Log_DebugPrintf("Invalidating VRAM read cache due to drawing area overlap");
      InvalidateVRAMReadCache();
    }

    m_batch.texture_page_x = m_render_state.texture_page_x;
    m_batch.texture_page_y = m_render_state.texture_page_y;
    m_batch.texture_palette_x = m_render_state.texture_palette_x;
    m_batch.texture_palette_y = m_render_state.texture_palette_y;
    m_render_state.ClearTexturePageChangedFlag();
  }

  if (m_render_state.IsTextureColorModeChanged())
  {
    m_batch.texture_color_mode = m_render_state.texture_color_mode;
    m_render_state.ClearTextureColorModeChangedFlag();
  }

  if (m_render_state.IsTransparencyModeChanged())
  {
    m_batch.transparency_mode = m_render_state.transparency_mode;
    m_render_state.ClearTransparencyModeChangedFlag();
  }

  if (m_render_state.IsTextureWindowChanged())
  {
    m_batch.texture_window_values[0] = m_render_state.texture_window_mask_x;
    m_batch.texture_window_values[1] = m_render_state.texture_window_mask_y;
    m_batch.texture_window_values[2] = m_render_state.texture_window_offset_x;
    m_batch.texture_window_values[3] = m_render_state.texture_window_offset_y;
    m_render_state.ClearTextureWindowChangedFlag();
  }

  LoadVertices(rc, num_vertices, command_ptr);
}
