#include "context_egl_wayland.h"
#include "../log.h"
#include <wayland-egl.h>
Log_SetChannel(GL::ContextEGLWayland);

namespace GL {
ContextEGLWayland::ContextEGLWayland(const WindowInfo& wi) : ContextEGL(wi) {}
ContextEGLWayland::~ContextEGLWayland() = default;

std::unique_ptr<Context> ContextEGLWayland::Create(const WindowInfo& wi, const Version* versions_to_try,
                                                   size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLWayland> context = std::make_unique<ContextEGLWayland>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLWayland::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLWayland> context = std::make_unique<ContextEGLWayland>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLNativeWindowType ContextEGLWayland::GetNativeWindow(EGLConfig config)
{
  wl_egl_window* window =
    wl_egl_window_create(static_cast<wl_surface*>(m_wi.window_handle), m_wi.surface_width, m_wi.surface_height);
  if (!window)
    return {};

  return reinterpret_cast<EGLNativeWindowType>(window);
}
} // namespace GL
