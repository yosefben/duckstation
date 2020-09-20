#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "frontend-common/opengl_host_display.h"
#include <go2/display.h>
#include <string>
#include <memory>

class Go2HostDisplay final : public FrontendCommon::OpenGLHostDisplay
{
public:
  Go2HostDisplay();
  ~Go2HostDisplay();

  RenderAPI GetRenderAPI() const override;

  bool CreateRenderDevice(const WindowInfo &wi, std::string_view adapter_name, bool debug_device) override;
  void DestroyRenderDevice() override;

  void SetVSync(bool enabled) override;

  bool Render() override;

private:
  go2_display_t* m_display = nullptr;
  go2_context_t* m_context = nullptr;
  go2_presenter_t* m_presenter = nullptr;
};
