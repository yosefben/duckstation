#include "gpu_hw.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "pgxp.h"
#include "settings.h"
#include "system.h"
#include <cmath>
#include <sstream>
#include <tuple>
#ifdef WITH_IMGUI
#include "imgui.h"
#endif
Log_SetChannel(GPU_HW);

ALWAYS_INLINE static bool ShouldUseUVLimits()
{
  // We only need UV limits if PGXP is enabled, or texture filtering is enabled.
  return g_settings.gpu_pgxp_enable || g_settings.gpu_texture_filter != GPUTextureFilter::Nearest;
}

GPU_HW::GPU_HW() : GPUBackend() {}

GPU_HW::~GPU_HW() = default;

bool GPU_HW::IsHardwareRenderer() const
{
  return true;
}

bool GPU_HW::Initialize()
{
  if (!GPUBackend::Initialize())
    return false;

  m_vram_ptr = m_vram_shadow.data();
  m_resolution_scale = CalculateResolutionScale();
  m_render_api = g_host_interface->GetDisplay()->GetRenderAPI();
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = ShouldUseUVLimits();
  PrintSettingsToLog();
  return true;
}

void GPU_HW::Reset()
{
  GPUBackend::Reset();

  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;

  m_vram_shadow.fill(0);

  m_batch = {};
  m_batch_ubo_data = {};
  m_batch_ubo_dirty = true;
  m_current_depth = 1;

  SetFullVRAMDirtyRectangle();
}

void GPU_HW::UpdateHWSettings(bool* framebuffer_changed, bool* shaders_changed)
{
  const u32 resolution_scale = CalculateResolutionScale();
  const bool use_uv_limits = ShouldUseUVLimits();

  *framebuffer_changed = (m_resolution_scale != resolution_scale);
  *shaders_changed = (m_resolution_scale != resolution_scale || m_true_color != g_settings.gpu_true_color ||
                      m_scaled_dithering != g_settings.gpu_scaled_dithering ||
                      m_texture_filtering != g_settings.gpu_texture_filter || m_using_uv_limits != use_uv_limits);

  if (m_resolution_scale != resolution_scale)
  {
#if FIXME
    g_host_interface->AddFormattedOSDMessage(10.0f, "Resolution scale set to %ux (display %ux%u, VRAM %ux%u)",
                                             resolution_scale, m_crtc_state.display_vram_width * resolution_scale,
                                             resolution_scale * m_crtc_state.display_vram_height,
                                             VRAM_WIDTH * resolution_scale, VRAM_HEIGHT * resolution_scale);
#endif
  }

  m_resolution_scale = resolution_scale;
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = use_uv_limits;
  PrintSettingsToLog();
}

u32 GPU_HW::CalculateResolutionScale() const
{
  if (g_settings.gpu_resolution_scale != 0)
    return std::clamp<u32>(g_settings.gpu_resolution_scale, 1, m_max_resolution_scale);

#if FIXME
  // auto scaling
  const s32 height = (m_crtc_state.display_height != 0) ? static_cast<s32>(m_crtc_state.display_height) : 480;
  const s32 preferred_scale =
    static_cast<s32>(std::ceil(static_cast<float>(m_host_display->GetWindowHeight()) / height));
  Log_InfoPrintf("Height = %d, preferred scale = %d", height, preferred_scale);

  return static_cast<u32>(std::clamp<s32>(preferred_scale, 1, m_max_resolution_scale));
#else
  return 1;
#endif
}

void GPU_HW::UpdateResolutionScale()
{
  GPUBackend::UpdateResolutionScale();

  if (CalculateResolutionScale() != m_resolution_scale)
    UpdateSettings();
}

std::tuple<u32, u32> GPU_HW::GetEffectiveDisplayResolution()
{
  return std::make_tuple(m_display_vram_width * m_resolution_scale, m_resolution_scale * m_display_vram_height);
}

void GPU_HW::PrintSettingsToLog()
{
  Log_InfoPrintf("Resolution Scale: %u (%ux%u), maximum %u", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                 VRAM_HEIGHT * m_resolution_scale, m_max_resolution_scale);
  Log_InfoPrintf("Dithering: %s%s", m_true_color ? "Disabled" : "Enabled",
                 (!m_true_color && m_scaled_dithering) ? " (Scaled)" : "");
  Log_InfoPrintf("Texture Filtering: %s", Settings::GetTextureFilterDisplayName(m_texture_filtering));
  Log_InfoPrintf("Dual-source blending: %s", m_supports_dual_source_blend ? "Supported" : "Not supported");
  Log_InfoPrintf("Using UV limits: %s", m_using_uv_limits ? "YES" : "NO");
}

void GPU_HW::UpdateVRAMReadTexture()
{
  m_renderer_stats.num_vram_read_texture_updates++;
  ClearVRAMDirtyRectangle();
}

void GPU_HW::HandleFlippedQuadTextureCoordinates(BatchVertex* vertices)
{
  // Taken from beetle-psx gpu_polygon.cpp
  // For X/Y flipped 2D sprites, PSX games rely on a very specific rasterization behavior. If U or V is decreasing in X
  // or Y, and we use the provided U/V as is, we will sample the wrong texel as interpolation covers an entire pixel,
  // while PSX samples its interpolation essentially in the top-left corner and splats that interpolant across the
  // entire pixel. While we could emulate this reasonably well in native resolution by shifting our vertex coords by
  // 0.5, this breaks in upscaling scenarios, because we have several samples per native sample and we need NN rules to
  // hit the same UV every time. One approach here is to use interpolate at offset or similar tricks to generalize the
  // PSX interpolation patterns, but the problem is that vertices sharing an edge will no longer see the same UV (due to
  // different plane derivatives), we end up sampling outside the intended boundary and artifacts are inevitable, so the
  // only case where we can apply this fixup is for "sprites" or similar which should not share edges, which leads to
  // this unfortunate code below.

  // It might be faster to do more direct checking here, but the code below handles primitives in any order and
  // orientation, and is far more SIMD-friendly if needed.
  const float abx = vertices[1].x - vertices[0].x;
  const float aby = vertices[1].y - vertices[0].y;
  const float bcx = vertices[2].x - vertices[1].x;
  const float bcy = vertices[2].y - vertices[1].y;
  const float cax = vertices[0].x - vertices[2].x;
  const float cay = vertices[0].y - vertices[2].y;

  // Compute static derivatives, just assume W is uniform across the primitive and that the plane equation remains the
  // same across the quad. (which it is, there is no Z.. yet).
  const float dudx = -aby * static_cast<float>(vertices[2].u) - bcy * static_cast<float>(vertices[0].u) -
                     cay * static_cast<float>(vertices[1].u);
  const float dvdx = -aby * static_cast<float>(vertices[2].v) - bcy * static_cast<float>(vertices[0].v) -
                     cay * static_cast<float>(vertices[1].v);
  const float dudy = +abx * static_cast<float>(vertices[2].u) + bcx * static_cast<float>(vertices[0].u) +
                     cax * static_cast<float>(vertices[1].u);
  const float dvdy = +abx * static_cast<float>(vertices[2].v) + bcx * static_cast<float>(vertices[0].v) +
                     cax * static_cast<float>(vertices[1].v);
  const float area = bcx * cay - bcy * cax;

  // Detect and reject any triangles with 0 size texture area
  const s32 texArea = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
                      (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);

  // Leverage PGXP to further avoid 3D polygons that just happen to align this way after projection
  const bool is_3d = (vertices[0].w != vertices[1].w || vertices[0].w != vertices[2].w);

  // Shouldn't matter as degenerate primitives will be culled anyways.
  if (area == 0.0f || texArea == 0 || is_3d)
    return;

  // Use floats here as it'll be faster than integer divides.
  const float rcp_area = 1.0f / area;
  const float dudx_area = dudx * rcp_area;
  const float dudy_area = dudy * rcp_area;
  const float dvdx_area = dvdx * rcp_area;
  const float dvdy_area = dvdy * rcp_area;
  const bool neg_dudx = dudx_area < 0.0f;
  const bool neg_dudy = dudy_area < 0.0f;
  const bool neg_dvdx = dvdx_area < 0.0f;
  const bool neg_dvdy = dvdy_area < 0.0f;
  const bool zero_dudx = dudx_area == 0.0f;
  const bool zero_dudy = dudy_area == 0.0f;
  const bool zero_dvdx = dvdx_area == 0.0f;
  const bool zero_dvdy = dvdy_area == 0.0f;

  // If we have negative dU or dV in any direction, increment the U or V to work properly with nearest-neighbor in
  // this impl. If we don't have 1:1 pixel correspondence, this creates a slight "shift" in the sprite, but we
  // guarantee that we don't sample garbage at least. Overall, this is kinda hacky because there can be legitimate,
  // rare cases where 3D meshes hit this scenario, and a single texel offset can pop in, but this is way better than
  // having borked 2D overall.
  //
  // TODO: If perf becomes an issue, we can probably SIMD the 8 comparisons above,
  // create an 8-bit code, and use a LUT to get the offsets.
  // Case 1: U is decreasing in X, but no change in Y.
  // Case 2: U is decreasing in Y, but no change in X.
  // Case 3: V is decreasing in X, but no change in Y.
  // Case 4: V is decreasing in Y, but no change in X.
  if ((neg_dudx && zero_dudy) || (neg_dudy && zero_dudx))
  {
    vertices[0].u++;
    vertices[1].u++;
    vertices[2].u++;
    vertices[3].u++;
  }

  if ((neg_dvdx && zero_dvdy) || (neg_dvdy && zero_dvdx))
  {
    vertices[0].v++;
    vertices[1].v++;
    vertices[2].v++;
    vertices[3].v++;
  }
}

void GPU_HW::ComputePolygonUVLimits(BatchVertex* vertices, u32 num_vertices)
{
  u16 min_u = vertices[0].u, max_u = vertices[0].u, min_v = vertices[0].v, max_v = vertices[0].v;
  for (u32 i = 1; i < num_vertices; i++)
  {
    min_u = std::min<u16>(min_u, vertices[i].u);
    max_u = std::max<u16>(max_u, vertices[i].u);
    min_v = std::min<u16>(min_v, vertices[i].v);
    max_v = std::max<u16>(max_v, vertices[i].v);
  }

  if (min_u != max_u)
    max_u--;
  if (min_v != max_v)
    max_v--;

  for (u32 i = 0; i < num_vertices; i++)
    vertices[i].SetUVLimits(min_u, max_u, min_v, max_v);
}

void GPU_HW::DrawLine(float x0, float y0, u32 col0, float x1, float y1, u32 col1, float depth)
{
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  std::array<BatchVertex, 4> output;
  if (dx == 0.0f && dy == 0.0f)
  {
    // Degenerate, render a point.
    output[0].Set(x0, y0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(x0 + 1.0f, y0, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(x1, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
    output[3].Set(x1 + 1.0f, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
  }
  else
  {
    const float abs_dx = std::fabs(dx);
    const float abs_dy = std::fabs(dy);
    float fill_dx, fill_dy;
    float dxdk, dydk;
    float pad_x0 = 0.0f;
    float pad_x1 = 0.0f;
    float pad_y0 = 0.0f;
    float pad_y1 = 0.0f;

    // Check for vertical or horizontal major lines.
    // When expanding to a rect, do so in the appropriate direction.
    // FIXME: This scheme seems to kinda work, but it seems very hard to find a method
    // that looks perfect on every game.
    // Vagrant Story speech bubbles are a very good test case here!
    if (abs_dx > abs_dy)
    {
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      dxdk = 1.0f;
      dydk = dy / abs_dx;

      if (dx > 0.0f)
      {
        // Right
        pad_x1 = 1.0f;
        pad_y1 = dydk;
      }
      else
      {
        // Left
        pad_x0 = 1.0f;
        pad_y0 = -dydk;
      }
    }
    else
    {
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      dydk = 1.0f;
      dxdk = dx / abs_dy;

      if (dy > 0.0f)
      {
        // Down
        pad_y1 = 1.0f;
        pad_x1 = dxdk;
      }
      else
      {
        // Up
        pad_y0 = 1.0f;
        pad_x0 = -dxdk;
      }
    }

    const float ox0 = x0 + pad_x0;
    const float oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1;
    const float oy1 = y1 + pad_y1;

    output[0].Set(ox0, oy0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(ox0 + fill_dx, oy0 + fill_dy, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(ox1, oy1, depth, 1.0f, col1, 0, 0, 0);
    output[3].Set(ox1 + fill_dx, oy1 + fill_dy, depth, 1.0f, col1, 0, 0, 0);
  }

  AddVertex(output[0]);
  AddVertex(output[1]);
  AddVertex(output[2]);
  AddVertex(output[3]);
  AddVertex(output[2]);
  AddVertex(output[1]);
}

void GPU_HW::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
  SetupDraw(cmd);
  if (cmd->params.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{cmd->rc.bits};
  const u32 texpage = ZeroExtend32(cmd->draw_mode.bits) | (ZeroExtend32(cmd->palette.bits) << 16);
  const float depth = GetCurrentNormalizedVertexDepth();

  DebugAssert(GetBatchVertexSpace() >= (rc.quad_polygon ? 6u : 3u));

  const u32 first_color = rc.color_for_first_vertex;
  const bool shaded = rc.shading_enable;
  const bool textured = rc.texture_enable;
  const bool pgxp = g_settings.gpu_pgxp_enable;

  std::array<BatchVertex, 4> vertices;
  for (u32 i = 0; i < cmd->num_vertices; i++)
  {
    const GPUBackendDrawPolygonCommand::Vertex& v = cmd->vertices[i];
    vertices[i].Set(v.precise_x, v.precise_y, depth, v.precise_w, v.color, texpage, v.texcoord, 0xFFFF0000u);
  }

  if (rc.quad_polygon && m_resolution_scale > 1)
    HandleFlippedQuadTextureCoordinates(vertices.data());

  if (m_using_uv_limits && textured)
    ComputePolygonUVLimits(vertices.data(), cmd->num_vertices);

  std::memcpy(m_batch_current_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
  m_batch_current_vertex_ptr += 3;

  // quads
  if (rc.quad_polygon)
  {
    AddVertex(vertices[2]);
    AddVertex(vertices[1]);
    AddVertex(vertices[3]);
  }

  IncludeVRAMDityRectangle(cmd->bounds);
}

void GPU_HW::DrawRectangle(const GPUBackendDrawRectangleCommand* cmd)
{
  SetupDraw(cmd);
  if (cmd->params.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{cmd->rc.bits};
  const u32 color = cmd->color;
  const u32 texpage = ZeroExtend32(cmd->draw_mode.bits) | (ZeroExtend32(cmd->palette.bits) << 16);
  const float depth = GetCurrentNormalizedVertexDepth();
  u16 orig_tex_left = cmd->texcoord & 0xFFu;
  u16 orig_tex_top = cmd->texcoord >> 8;

  // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
  u16 tex_top = orig_tex_top;
  for (u16 y_offset = 0; y_offset < cmd->height;)
  {
    const u16 quad_height = std::min<u16>(cmd->height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
    const float quad_start_y = static_cast<float>(cmd->y + y_offset);
    const float quad_end_y = quad_start_y + static_cast<float>(quad_height);
    const u16 tex_bottom = tex_top + static_cast<u16>(quad_height);

    u16 tex_left = orig_tex_left;
    for (u16 x_offset = 0; x_offset < cmd->width;)
    {
      const u16 quad_width = std::min<u16>(cmd->width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
      const float quad_start_x = static_cast<float>(cmd->x + x_offset);
      const float quad_end_x = quad_start_x + static_cast<float>(quad_width);
      const u16 tex_right = tex_left + static_cast<u16>(quad_width);
      const u32 uv_limits = BatchVertex::PackUVLimits(tex_left, tex_right - 1, tex_top, tex_bottom - 1);

      AddNewVertex(quad_start_x, quad_start_y, depth, 1.0f, color, texpage, tex_left, tex_top, uv_limits);
      AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
      AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);

      AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);
      AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
      AddNewVertex(quad_end_x, quad_end_y, depth, 1.0f, color, texpage, tex_right, tex_bottom, uv_limits);

      x_offset += quad_width;
      tex_left = 0;
    }

    y_offset += quad_height;
    tex_top = 0;
  }

  IncludeVRAMDityRectangle(cmd->bounds);
}

void GPU_HW::DrawLine(const GPUBackendDrawLineCommand* cmd)
{
  SetupDraw(cmd);
  if (cmd->params.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{cmd->rc.bits};
  const float depth = GetCurrentNormalizedVertexDepth();

  for (u32 i = 1; i < cmd->num_vertices; i++)
  {
    const GPUBackendDrawLineCommand::Vertex& start = cmd->vertices[i - 1u];
    const GPUBackendDrawLineCommand::Vertex& end = cmd->vertices[i];

    DrawLine(static_cast<float>(start.x), static_cast<float>(start.y), start.color, static_cast<float>(end.x),
             static_cast<float>(end.y), end.color, depth);
  }

  IncludeVRAMDityRectangle(cmd->bounds);
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<u32>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<u32>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

GPU_HW::VRAMFillUBOData GPU_HW::GetVRAMFillUBOData(u32 x, u32 y, u32 width, u32 height, u32 color,
                                                   GPUBackendCommandParameters params) const
{
  // drop precision unless true colour is enabled
  if (!m_true_color)
    color = RGBA5551ToRGBA8888(RGBA8888ToRGBA5551(color));

  VRAMFillUBOData uniforms;
  std::tie(uniforms.u_fill_color[0], uniforms.u_fill_color[1], uniforms.u_fill_color[2], uniforms.u_fill_color[3]) =
    RGBA8ToFloat(color);
  uniforms.u_interlaced_displayed_field = params.active_line_lsb;
  return uniforms;
}

Common::Rectangle<u32> GPU_HW::GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height) const
{
  Common::Rectangle<u32> out_rc = Common::Rectangle<u32>::FromExtents(x % VRAM_WIDTH, y % VRAM_HEIGHT, width, height);
  if (out_rc.right > VRAM_WIDTH)
  {
    out_rc.left = 0;
    out_rc.right = VRAM_WIDTH;
  }
  if (out_rc.bottom > VRAM_HEIGHT)
  {
    out_rc.top = 0;
    out_rc.bottom = VRAM_HEIGHT;
  }
  return out_rc;
}

GPU_HW::VRAMWriteUBOData GPU_HW::GetVRAMWriteUBOData(u32 x, u32 y, u32 width, u32 height, u32 buffer_offset,
                                                     GPUBackendCommandParameters params) const
{
  const VRAMWriteUBOData uniforms = {(x % VRAM_WIDTH),
                                     (y % VRAM_HEIGHT),
                                     ((x + width) % VRAM_WIDTH),
                                     ((y + height) % VRAM_HEIGHT),
                                     width,
                                     height,
                                     buffer_offset,
                                     params.set_mask_while_drawing ? 0x8000u : 0x00,
                                     GetCurrentNormalizedVertexDepth()};
  return uniforms;
}

bool GPU_HW::UseVRAMCopyShader(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                               GPUBackendCommandParameters params) const
{
  // masking enabled, oversized, or overlapping
  return (params.IsMaskingEnabled() || ((src_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((src_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT || ((dst_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((dst_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT ||
          Common::Rectangle<u32>::FromExtents(src_x, src_y, width, height)
            .Intersects(Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height)));
}

GPU_HW::VRAMCopyUBOData GPU_HW::GetVRAMCopyUBOData(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                                                   GPUBackendCommandParameters params) const
{
  const VRAMCopyUBOData uniforms = {(src_x % VRAM_WIDTH) * m_resolution_scale,
                                    (src_y % VRAM_HEIGHT) * m_resolution_scale,
                                    (dst_x % VRAM_WIDTH) * m_resolution_scale,
                                    (dst_y % VRAM_HEIGHT) * m_resolution_scale,
                                    ((dst_x + width) % VRAM_WIDTH) * m_resolution_scale,
                                    ((dst_y + height) % VRAM_HEIGHT) * m_resolution_scale,
                                    width * m_resolution_scale,
                                    height * m_resolution_scale,
                                    params.set_mask_while_drawing ? 1u : 0u,
                                    GetCurrentNormalizedVertexDepth()};

  return uniforms;
}

void GPU_HW::IncludeVRAMDityRectangle(const Common::Rectangle<u32>& rect)
{
  m_vram_dirty_rect.Include(rect);

#if FIXME
  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (!m_draw_mode.IsTexturePageChanged() &&
      (m_draw_mode.GetTexturePageRectangle().Intersects(rect) ||
       (m_draw_mode.IsUsingPalette() && m_draw_mode.GetTexturePaletteRectangle().Intersects(rect))))
  {
    m_draw_mode.SetTexturePageChanged();
  }
#endif
}

void GPU_HW::IncludeVRAMDityRectangle(const Common::Rectangle<u16>& rect)
{
  IncludeVRAMDityRectangle(Common::Rectangle<u32>(ZeroExtend32(rect.left), ZeroExtend32(rect.top),
                                                  ZeroExtend32(rect.right), ZeroExtend32(rect.bottom)));
}

void GPU_HW::EnsureVertexBufferSpace(u32 required_vertices)
{
  if (m_batch_current_vertex_ptr)
  {
    if (GetBatchVertexSpace() >= required_vertices)
      return;

    FlushRender();
  }

  MapBatchVertexPointer(required_vertices);
}

void GPU_HW::EnsureVertexBufferSpace(const GPUBackendDrawCommand* cmd)
{
  u32 required_vertices;
  switch (cmd->type)
  {
    case GPUBackendCommandType::DrawPolygon:
      required_vertices = cmd->rc.quad_polygon ? 6 : 3;
      break;
    case GPUBackendCommandType::DrawRectangle:
      required_vertices = MAX_VERTICES_FOR_RECTANGLE;
      break;
    case GPUBackendCommandType::DrawLine:
    default:
      required_vertices = static_cast<const GPUBackendDrawLineCommand*>(cmd)->num_vertices * 3u;
      break;
  }

  // can we fit these vertices in the current depth buffer range?
  if ((m_current_depth + required_vertices) > MAX_BATCH_VERTEX_COUNTER_IDS)
  {
    // implies FlushRender()
    ResetBatchVertexDepth();
  }
  else if (m_batch_current_vertex_ptr)
  {
    if (GetBatchVertexSpace() >= required_vertices)
      return;

    FlushRender();
  }

  MapBatchVertexPointer(required_vertices);
}

void GPU_HW::ResetBatchVertexDepth()
{
  Log_PerfPrint("Resetting batch vertex depth");
  FlushRender();
  UpdateDepthBufferFromMaskBit();

  m_current_depth = 1;
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(x, y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params)
{
  DebugAssert((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT);
  IncludeVRAMDityRectangle(Common::Rectangle<u32>::FromExtents(x, y, width, height));

  if (params.check_mask_before_draw)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                      GPUBackendCommandParameters params)
{
  IncludeVRAMDityRectangle(
    Common::Rectangle<u32>::FromExtents(dst_x, dst_y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));

  if (params.check_mask_before_draw)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::SetupDraw(const GPUBackendDrawCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  GPUTextureMode texture_mode;
  if (rc.IsTexturingEnabled())
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if ((cmd->draw_mode.bits & GPUDrawModeReg::TEXTURE_PAGE_MASK) !=
          (m_last_texture_page_bits.bits & GPUDrawModeReg::TEXTURE_PAGE_MASK) ||
        true)
    {
      m_last_texture_page_bits.bits = cmd->draw_mode.bits;

      if (m_vram_dirty_rect.Valid() &&
          (m_last_texture_page_bits.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
           (m_last_texture_page_bits.IsUsingPalette() &&
            m_last_texture_page_bits.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect))))
      {
        // Log_DevPrintf("Invalidating VRAM read cache due to drawing area overlap");
        if (!IsFlushed())
          FlushRender();

        UpdateVRAMReadTexture();
      }
    }

    texture_mode = cmd->draw_mode.texture_mode;
    if (rc.raw_texture_enable)
    {
      texture_mode =
        static_cast<GPUTextureMode>(static_cast<u8>(texture_mode) | static_cast<u8>(GPUTextureMode::RawTextureBit));
    }
  }
  else
  {
    texture_mode = GPUTextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const GPUTransparencyMode transparency_mode =
    rc.transparency_enable ? cmd->draw_mode.transparency_mode : GPUTransparencyMode::Disabled;
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? cmd->draw_mode.dither_enable : false;
  if (m_batch.texture_mode != texture_mode || m_batch.transparency_mode != transparency_mode ||
      dithering_enable != m_batch.dithering)
  {
    FlushRender();
  }

  EnsureVertexBufferSpace(cmd);

  // transparency mode change
  if (m_batch.transparency_mode != transparency_mode && transparency_mode != GPUTransparencyMode::Disabled)
  {
    static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};
    m_batch_ubo_data.u_src_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][0];
    m_batch_ubo_data.u_dst_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][1];
    m_batch_ubo_dirty = true;
  }

  if (m_batch.check_mask_before_draw != cmd->params.check_mask_before_draw ||
      m_batch.set_mask_while_drawing != cmd->params.set_mask_while_drawing)
  {
    m_batch.check_mask_before_draw = cmd->params.check_mask_before_draw;
    m_batch.set_mask_while_drawing = cmd->params.set_mask_while_drawing;
    m_batch_ubo_data.u_set_mask_while_drawing = BoolToUInt32(m_batch.set_mask_while_drawing);
    m_batch_ubo_dirty = true;
  }

  m_batch.interlacing = cmd->params.interlaced_rendering;
  if (m_batch.interlacing)
  {
    const u32 displayed_field = cmd->params.active_line_lsb;
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlaced_displayed_field != displayed_field);
    m_batch_ubo_data.u_interlaced_displayed_field = displayed_field;
  }

  // update state
  m_batch.texture_mode = texture_mode;
  m_batch.transparency_mode = transparency_mode;
  m_batch.dithering = dithering_enable;

  if (std::memcmp(&m_last_texture_window, &cmd->window, sizeof(m_last_texture_window)) != 0)
  {
    m_last_texture_window = cmd->window;
    m_batch_ubo_data.u_texture_window_and[0] = ZeroExtend32(cmd->window.and_x);
    m_batch_ubo_data.u_texture_window_and[1] = ZeroExtend32(cmd->window.and_y);
    m_batch_ubo_data.u_texture_window_or[0] = ZeroExtend32(cmd->window.or_x);
    m_batch_ubo_data.u_texture_window_or[1] = ZeroExtend32(cmd->window.or_y);
    m_batch_ubo_dirty = true;
  }
}

void GPU_HW::FlushRender()
{
  if (!m_batch_current_vertex_ptr)
    return;

  const u32 vertex_count = GetBatchVertexCount();
  UnmapBatchVertexPointer(vertex_count);

  if (vertex_count == 0)
    return;

  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    SetScissorFromDrawingArea();
  }

  if (m_batch_ubo_dirty)
  {
    UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    m_batch_ubo_dirty = false;
  }

  if (m_batch.NeedsTwoPassRendering())
  {
    m_renderer_stats.num_batches += 2;
    DrawBatchVertices(BatchRenderMode::OnlyTransparent, m_batch_base_vertex, vertex_count);
    DrawBatchVertices(BatchRenderMode::OnlyOpaque, m_batch_base_vertex, vertex_count);
  }
  else
  {
    m_renderer_stats.num_batches++;
    DrawBatchVertices(m_batch.GetRenderMode(), m_batch_base_vertex, vertex_count);
  }
}

void GPU_HW::DrawRendererStats(bool is_idle_frame)
{
  if (!is_idle_frame)
  {
    m_last_renderer_stats = m_renderer_stats;
    m_renderer_stats = {};
  }

#ifdef WITH_IMGUI
  if (ImGui::CollapsingHeader("Renderer Statistics", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
    static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};
    const auto& stats = m_last_renderer_stats;

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f * ImGui::GetIO().DisplayFramebufferScale.x);

    ImGui::TextUnformatted("Resolution Scale:");
    ImGui::NextColumn();
    ImGui::Text("%u (VRAM %ux%u)", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                VRAM_HEIGHT * m_resolution_scale);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Effective Display Resolution:");
    ImGui::NextColumn();
#if FIXME
    ImGui::Text("%ux%u", m_crtc_state.display_vram_width * m_resolution_scale,
                m_crtc_state.display_vram_height * m_resolution_scale);
#endif
    ImGui::NextColumn();

    ImGui::TextUnformatted("True Color:");
    ImGui::NextColumn();
    ImGui::TextColored(m_true_color ? active_color : inactive_color, m_true_color ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Scaled Dithering:");
    ImGui::NextColumn();
    ImGui::TextColored(m_scaled_dithering ? active_color : inactive_color, m_scaled_dithering ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Texture Filtering:");
    ImGui::NextColumn();
    ImGui::TextColored((m_texture_filtering != GPUTextureFilter::Nearest) ? active_color : inactive_color, "%s",
                       Settings::GetTextureFilterDisplayName(m_texture_filtering));
    ImGui::NextColumn();

    ImGui::TextUnformatted("PGXP:");
    ImGui::NextColumn();
    ImGui::TextColored(g_settings.gpu_pgxp_enable ? active_color : inactive_color, "Geom");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling) ? active_color : inactive_color,
                       "Cull");
    ImGui::SameLine();
    ImGui::TextColored(
      (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction) ? active_color : inactive_color, "Tex");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_vertex_cache) ? active_color : inactive_color,
                       "Cache");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Batches Drawn:");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_batches);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Read Texture Updates:");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_read_texture_updates);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Uniform Buffer Updates: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_uniform_buffer_updates);
    ImGui::NextColumn();

    ImGui::Columns(1);
  }
#endif
}
