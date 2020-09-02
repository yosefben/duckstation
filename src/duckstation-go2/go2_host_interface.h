#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include "frontend-common/common_host_interface.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class AudioStream;

class INISettingsInterface;

class Go2HostInterface final : public CommonHostInterface
{
public:
  Go2HostInterface();
  ~Go2HostInterface();

  static std::unique_ptr<Go2HostInterface> Create();

  const char* GetFrontendName() const override;

  bool Initialize() override;
  void Shutdown() override;

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;

  void Run();

protected:
  void LoadSettings() override;
  void SetDefaultSettings(SettingsInterface &si) override;
  void UpdateControllerInterface() override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  void OnRunningGameChanged() override;

  void RequestExit() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;
  void UpdateInputMap() override;

private:
  bool CreateDisplay();
  void DestroyDisplay();
  void CreateImGuiContext();

  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

  std::unique_ptr<INISettingsInterface> m_settings_interface;

  bool m_quit_request = false;
};
