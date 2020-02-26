#include "common/assert.h"
#include "common/log.h"
#include "oga_host_display.h"
#include <EGL/egl.h>
#include <drm/drm_fourcc.h>
#include <array>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <tuple>
Log_SetChannel(OGAHostDisplay);

class OGAHostDIsplayTexture : public HostDisplayTexture
{
public:
  OGAHostDIsplayTexture(GLuint id, u32 width, u32 height) : m_id(id), m_width(width), m_height(height) {}
  ~OGAHostDIsplayTexture() override { glDeleteTextures(1, &m_id); }

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_id)); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  GLuint GetGLID() const { return m_id; }

  static std::unique_ptr<OGAHostDIsplayTexture> Create(u32 width, u32 height, const void* initial_data,
                                                       u32 initial_data_stride)
  {
    GLuint id;
    glGenTextures(1, &id);

    GLint old_texture_binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

    // TODO: Set pack width
    Assert(!initial_data || initial_data_stride == (width * sizeof(u32)));

    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, id);
    return std::make_unique<OGAHostDIsplayTexture>(id, width, height);
  }

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

OGAHostDisplay::OGAHostDisplay() = default;

OGAHostDisplay::~OGAHostDisplay()
{
  if (m_context)
  {
    if (m_display_vao != 0)
      glDeleteVertexArrays(1, &m_display_vao);
    if (m_display_linear_sampler != 0)
      glDeleteSamplers(1, &m_display_linear_sampler);
    if (m_display_nearest_sampler != 0)
      glDeleteSamplers(1, &m_display_nearest_sampler);

    m_display_program.Destroy();
    ImGui_ImplOpenGL3_Shutdown();

    go2_context_make_current(nullptr);
    go2_context_destroy(m_context);
  }

  if (m_presenter)
    go2_presenter_destroy(m_presenter);

  if (m_display)
    go2_display_destroy(m_display);
}

HostDisplay::RenderAPI OGAHostDisplay::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::OpenGLES;
}

void* OGAHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* OGAHostDisplay::GetRenderContext() const
{
  return m_context;
}

void* OGAHostDisplay::GetRenderWindow() const
{
  return m_display;
}

void OGAHostDisplay::ChangeRenderWindow(void* new_window)
{
  Panic("Not implemented");
}

std::unique_ptr<HostDisplayTexture> OGAHostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                  u32 data_stride, bool dynamic)
{
  return OGAHostDIsplayTexture::Create(width, height, data, data_stride);
}

void OGAHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                   u32 data_stride)
{
  OGAHostDIsplayTexture* tex = static_cast<OGAHostDIsplayTexture*>(texture);
  Assert(data_stride == (width * sizeof(u32)));

  GLint old_texture_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture_binding);

  glBindTexture(GL_TEXTURE_2D, tex->GetGLID());
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

  glBindTexture(GL_TEXTURE_2D, old_texture_binding);
}

void OGAHostDisplay::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  eglSwapInterval(go2_context_egldisplay_get(m_context), enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

std::tuple<u32, u32> OGAHostDisplay::GetWindowSize() const
{
  return std::make_tuple(static_cast<u32>(m_display_width), static_cast<u32>(m_display_height));
}

void OGAHostDisplay::WindowResized() {}

const char* OGAHostDisplay::GetGLSLVersionString() const
{
  if (GLAD_GL_ES_VERSION_3_0)
    return "#version 300 es";
  else
    return "#version 100";
}

std::string OGAHostDisplay::GetGLSLVersionHeader() const
{
  std::string header = GetGLSLVersionString();
  header += "\n\n";
  header += "precision highp float;\n";
  header += "precision highp int;\n\n";

  return header;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrintf(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      // Log_DebugPrint(message);
      break;
  }
}

bool OGAHostDisplay::CreateDisplay()
{
  m_display = go2_display_create();
  if (!m_display)
    return false;

  m_presenter = go2_presenter_create(m_display, DRM_FORMAT_RGB565, 0xff080808);
  if (!m_presenter)
    return false;

  m_display_width = go2_display_width_get(m_display);
  m_display_height = go2_display_height_get(m_display);
  return true;
}

bool OGAHostDisplay::CreateGLContext(bool debug_device)
{
  static constexpr std::array<std::tuple<int, int>, 4> versions_to_try = { {{3, 2}, {3, 1}, {3, 0}, {2, 0}} };

  go2_context_attributes_t attributes = {};
  attributes.red_bits = 8;
  attributes.green_bits = 8;
  attributes.blue_bits = 8;
  attributes.alpha_bits = 0;
  attributes.depth_bits = 0;
  attributes.stencil_bits = 0;

  for (const auto [major, minor] : versions_to_try)
  {
    attributes.major = major;
    attributes.minor = minor;

    Log_InfoPrintf("Trying a OpenGL ES %d.%d context", major, minor);
    m_context = go2_context_create(m_display, m_display_width, m_display_height, &attributes);
    if (m_context)
    {
      Log_InfoPrintf("Got a OpenGL ES %d.%d context", major, minor);
      break;
    }
  }

  if (!m_context)
  {
    Log_ErrorPrintf("Failed to create any GL context");
    return false;
  }

  // Load GLAD.
  go2_context_make_current(m_context);
  if (!gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress)))
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

  if (debug_device && GLAD_GL_KHR_debug)
  {
    glad_glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  }

  // start with vsync on
  eglSwapInterval(go2_context_egldisplay_get(m_context), 1);
  return true;
}

bool OGAHostDisplay::CreateImGuiContext()
{
  if (!ImGui_ImplOpenGL3_Init(GetGLSLVersionString()))
    return false;

  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_display_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_display_height);

  ImGui_ImplOpenGL3_NewFrame();
  return true;
}

bool OGAHostDisplay::CreateGLResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
uniform vec4 u_src_rect;
out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader,
                                 GetGLSLVersionHeader() + display_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  if (!m_display_program.Link())
  {
    Log_ErrorPrintf("Failed to link display program");
    return false;
  }

  m_display_program.Bind();
  m_display_program.RegisterUniform("u_src_rect");
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(1, 0);

  glGenVertexArrays(1, &m_display_vao);

  // samplers
  glGenSamplers(1, &m_display_nearest_sampler);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenSamplers(1, &m_display_linear_sampler);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return true;
}

std::unique_ptr<HostDisplay> OGAHostDisplay::Create(bool debug_device)
{
  std::unique_ptr<OGAHostDisplay> display = std::make_unique<OGAHostDisplay>();
  if (!display->CreateDisplay() || !display->CreateGLContext(debug_device) || !display->CreateImGuiContext() ||
      !display->CreateGLResources())
  {
    return nullptr;
  }

  return display;
}

void OGAHostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  RenderDisplay();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  go2_context_swap_buffers(m_context);
  go2_surface_t* gles_surface = go2_context_surface_lock(m_context);
  go2_presenter_post(m_presenter, gles_surface, 0, 0, m_display_width, m_display_height, 0, 0, m_display_width, m_display_height, GO2_ROTATION_DEGREES_270);
  go2_context_surface_unlock(m_context, gles_surface);

  ImGui::NewFrame();
  ImGui_ImplOpenGL3_NewFrame();

  GL::Program::ResetLastProgram();
}

void OGAHostDisplay::RenderDisplay()
{
  if (!m_display_texture_handle)
    return;

  // - 20 for main menu padding
  const auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_display_width, std::max(m_display_height - m_display_top_margin, 1), m_display_aspect_ratio);

  glViewport(vp_left, m_display_height - (m_display_top_margin + vp_top) - vp_height, vp_width, vp_height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  m_display_program.Bind();
  m_display_program.Uniform4f(
    0, static_cast<float>(m_display_offset_x) / static_cast<float>(m_display_texture_width),
    static_cast<float>(m_display_offset_y) / static_cast<float>(m_display_texture_height),
    (static_cast<float>(m_display_width) - 0.5f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_height) - 0.5f) / static_cast<float>(m_display_texture_height));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(m_display_texture_handle)));
  glBindSampler(0, m_display_linear_filtering ? m_display_linear_sampler : m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}
