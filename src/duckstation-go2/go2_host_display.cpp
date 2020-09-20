#include "common/assert.h"
#include "common/log.h"
#include "go2_host_display.h"
#include "imgui.h"
#include "frontend-common/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <drm/drm_fourcc.h>
#include <array>
#include <tuple>
Log_SetChannel(Go2HostDisplay);

Go2HostDisplay::Go2HostDisplay() = default;

Go2HostDisplay::~Go2HostDisplay()
{
  Assert(!m_display && !m_context && !m_presenter);
}

HostDisplay::RenderAPI Go2HostDisplay::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::OpenGLES;
}

void Go2HostDisplay::SetVSync(bool enabled)
{
  // Window framebuffer has to be bound to call SetSwapInterval.
  GLint current_fbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  eglSwapInterval(go2_context_egldisplay_get(m_context), enabled ? 1 : 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current_fbo);
}

bool Go2HostDisplay::CreateRenderDevice(const WindowInfo &wi, std::string_view adapter_name, bool debug_device)
{
  m_display = go2_display_create();
  if (!m_display)
    return false;

  m_presenter = go2_presenter_create(m_display, DRM_FORMAT_RGB565, 0xff080808);
  if (!m_presenter)
    return false;

  m_window_info = wi;
  m_window_info.surface_width = go2_display_height_get(m_display);
  m_window_info.surface_height = go2_display_width_get(m_display);

  static constexpr std::array<std::tuple<int, int>, 4> versions_to_try = { {{3, 2}, {3, 1}, {3, 0}, {2, 0}} };

  go2_context_attributes_t attributes = {};
  attributes.red_bits = 5;
  attributes.green_bits = 6;
  attributes.blue_bits = 5;
  attributes.alpha_bits = 0;
  attributes.depth_bits = 0;
  attributes.stencil_bits = 0;

  for (const auto [major, minor] : versions_to_try)
  {
    attributes.major = major;
    attributes.minor = minor;

    Log_InfoPrintf("Trying a OpenGL ES %d.%d context", major, minor);
    m_context = go2_context_create(m_display, m_window_info.surface_width, m_window_info.surface_height, &attributes);
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

  // start with vsync on
  eglSwapInterval(go2_context_egldisplay_get(m_context), 1);
  return true;
}

void Go2HostDisplay::DestroyRenderDevice()
{
  if (ImGui::GetCurrentContext())
    DestroyImGuiContext();

  DestroyResources();

  if (m_context)
  {
    go2_context_make_current(nullptr);
    go2_context_destroy(m_context);
  }

  if (m_presenter)
    go2_presenter_destroy(m_presenter);

  if (m_display)
    go2_display_destroy(m_display);
}

bool Go2HostDisplay::Render()
{
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(m_window_info.surface_width, m_window_info.surface_height, 0, false);
    RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                  m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                  m_display_texture_view_height, m_display_linear_filtering);
  }

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  GL::Program::ResetLastProgram();

  go2_context_swap_buffers(m_context);
  go2_surface_t* gles_surface = go2_context_surface_lock(m_context);
  go2_presenter_post(m_presenter, gles_surface, 0, 0, m_window_info.surface_width, m_window_info.surface_height, 0, 0, m_window_info.surface_height, m_window_info.surface_width, GO2_ROTATION_DEGREES_270);
  go2_context_surface_unlock(m_context, gles_surface);

  ImGui_ImplOpenGL3_NewFrame();
  return true;
}

