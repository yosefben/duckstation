#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "go2/display.h"
#include <string>
#include <memory>

class OGAHostDisplay final : public HostDisplay
{
public:
  OGAHostDisplay();
  ~OGAHostDisplay();

  static std::unique_ptr<HostDisplay> Create(bool debug_device);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void* GetRenderWindow() const override;

  void ChangeRenderWindow(void* new_window) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;

  void SetVSync(bool enabled) override;

  std::tuple<u32, u32> GetWindowSize() const override;
  void WindowResized() override;

private:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool CreateDisplay();
  bool CreateGLContext(bool debug_device);
  bool CreateImGuiContext();
  bool CreateGLResources();

  void Render() override;
  void RenderDisplay();

  go2_display_t* m_display = nullptr;
  go2_context_t* m_context = nullptr;
  go2_presenter_t* m_presenter = nullptr;
  int m_display_width = 0;
  int m_display_height = 0;

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;
};
