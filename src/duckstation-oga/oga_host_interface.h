#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include "go2/input.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class System;
class AudioStream;

class Controller;

class OGAHostInterface final : public HostInterface
{
public:
  OGAHostInterface();
  ~OGAHostInterface();

  static std::unique_ptr<OGAHostInterface> Create();

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  void Run();

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed();
  void OnControllerTypeChanged(u32 slot) override;

private:
  enum PadButtonCode : s32
  {
    A,
    B,
    X,
    Y,
    TopLeft,
    TopRight,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    Count
  };

  bool CreateDisplay();
  void DestroyDisplay();
  void CreateImGuiContext();
  void ClearImGuiFocus();

  /// Executes a callback later, after the UI has finished rendering. Needed to boot while rendering ImGui.
  void RunLater(std::function<void()> callback);

  void SaveSettings();
  void UpdateSettings();

  void UpdateControllerMapping();
  void UpdateInput();

  void DrawImGui();

  void DrawPoweredOffWindow();

  go2_input_t* m_input = nullptr;

  std::unique_ptr<HostDisplayTexture> m_app_icon_texture;

  bool m_quit_request = false;
  bool m_focus_main_menu_bar = false;

  // this copy of the settings is modified by imgui
  Settings m_settings_copy;
};
