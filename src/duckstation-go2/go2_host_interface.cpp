#include "go2_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "go2_host_display.h"
#include "go2_controller_interface.h"
#include "scmversion/scmversion.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
Log_SetChannel(Go2HostInterface);

Go2HostInterface::Go2HostInterface() = default;

Go2HostInterface::~Go2HostInterface() = default;

const char* Go2HostInterface::GetFrontendName() const
{
  return "DuckStation ODROID-Go Advance Frontend";
}

ALWAYS_INLINE static TinyString GetWindowTitle()
{
  return TinyString::FromFormat("DuckStation %s (%s)", g_scm_tag_str, g_scm_branch_str);
}

bool Go2HostInterface::CreateDisplay()
{
  WindowInfo wi;

  std::unique_ptr<Go2HostDisplay> display = std::make_unique<Go2HostDisplay>();
  if (!display->CreateRenderDevice(wi, g_settings.gpu_adapter, g_settings.gpu_use_debug_device) ||
      !display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device))
  {
    ReportError("Failed to create/initialize display render device");
    return false;
  }

  m_display = std::move(display);
  return true;
}

void Go2HostInterface::DestroyDisplay()
{
  m_display->DestroyRenderDevice();
  m_display.reset();
}

void Go2HostInterface::CreateImGuiContext()
{
  const float framebuffer_scale = 1.0f;

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  //ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetIO().DisplayFramebufferScale.x = framebuffer_scale;
  ImGui::GetIO().DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);
}

bool Go2HostInterface::AcquireHostDisplay()
{
  return true;
}

void Go2HostInterface::ReleaseHostDisplay()
{
  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

std::optional<CommonHostInterface::HostKeyCode> Go2HostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  return std::nullopt;
}

void Go2HostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(*m_settings_interface.get());
}

void Go2HostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  Settings old_settings(std::move(g_settings));
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::ApplyGameSettings(true);
  CommonHostInterface::FixIncompatibleSettings(true);
  CheckForSettingsChanges(old_settings);
}

void Go2HostInterface::RequestExit()
{
  Log_ErrorPrintf("TODO");
}

bool Go2HostInterface::IsFullscreen() const
{
  return true;
}

bool Go2HostInterface::SetFullscreen(bool enabled)
{
  return false;
}

std::unique_ptr<Go2HostInterface> Go2HostInterface::Create()
{
  return std::make_unique<Go2HostInterface>();
}

bool Go2HostInterface::Initialize()
{
  if (!CommonHostInterface::Initialize())
    return false;

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  CreateImGuiContext();
  if (!CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return false;
  }

  ImGui::NewFrame();

  // process events to pick up controllers before updating input map
  UpdateInputMap();
  return true;
}

void Go2HostInterface::Shutdown()
{
  DestroySystem();

  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }

  CommonHostInterface::Shutdown();
}

std::string Go2HostInterface::GetStringSettingValue(const char* section, const char* key,
                                                    const char* default_value /*= ""*/)
{
  return m_settings_interface->GetStringValue(section, key, default_value);
}

bool Go2HostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return m_settings_interface->GetBoolValue(section, key, default_value);
}

int Go2HostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return m_settings_interface->GetIntValue(section, key, default_value);
}

float Go2HostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return m_settings_interface->GetFloatValue(section, key, default_value);
}

void Go2HostInterface::LoadSettings()
{
  // Settings need to be loaded prior to creating the window for OpenGL bits.
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  if (!CommonHostInterface::CheckSettings(*m_settings_interface.get()))
    m_settings_interface->Save();

  CommonHostInterface::LoadSettings(*m_settings_interface.get());
  CommonHostInterface::FixIncompatibleSettings(false);
}

void Go2HostInterface::SetDefaultSettings(SettingsInterface &si)
{
  CommonHostInterface::SetDefaultSettings(si);

  si.SetStringValue("Controller1", "ButtonUp", "Controller0/Button0");
  si.SetStringValue("Controller1", "ButtonDown", "Controller0/Button1");
  si.SetStringValue("Controller1", "ButtonLeft", "Controller0/Button2");
  si.SetStringValue("Controller1", "ButtonRight", "Controller0/Button3");
  si.SetStringValue("Controller1", "ButtonSelect", "Controller0/Button8");
  si.SetStringValue("Controller1", "ButtonStart", "Controller0/Button9");
  si.SetStringValue("Controller1", "ButtonTriangle", "Controller0/Button6");
  si.SetStringValue("Controller1", "ButtonCross", "Controller0/Button7");
  si.SetStringValue("Controller1", "ButtonSquare", "Controller0/Button5");
  si.SetStringValue("Controller1", "ButtonCircle", "Controller0/Button4");
  si.SetStringValue("Controller1", "ButtonL1", "Controller0/Button16");
  si.SetStringValue("Controller1", "ButtonL2", "Controller0/Button14");
  si.SetStringValue("Controller1", "ButtonR1", "Controller0/Button17");
  si.SetStringValue("Controller1", "ButtonR2", "Controller0/Button15");
  si.SetStringValue("Controller1", "LeftX", "Controller0/Axis0");
  si.SetStringValue("Controller1", "LeftY", "Controller0/Axis1");

  si.SetStringValue("Logging", "LogLevel", "Info");
  si.SetBoolValue("Logging", "LogToConsole", true);

  si.SetBoolValue("Display", "ShowOSDMessages", true);
  si.SetBoolValue("Display", "ShowFPS", false);
  si.SetBoolValue("Display", "ShowVPS", false);
  si.SetBoolValue("Display", "ShowSpeed", false);
  si.SetBoolValue("Display", "ShowResolution", false);
}

void Go2HostInterface::UpdateControllerInterface()
{
  if (m_controller_interface)
    return;

  m_controller_interface = std::make_unique<Go2ControllerInterface>();
  if (!m_controller_interface->Initialize(this))
  {
    Log_WarningPrintf("Failed to initialize go2 controller interface");
    m_controller_interface.reset();
  }
}

void Go2HostInterface::Run()
{
  while (!m_quit_request)
  {
    PollAndUpdate();

    if (System::IsRunning())
    {
      System::RunFrame();
      UpdateControllerRumble();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      DrawImGuiWindows();

      m_display->Render();
      ImGui::NewFrame();

      if (System::IsRunning())
      {
        System::EndFrame();

        if (m_speed_limiter_enabled)
          System::Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (!System::IsShutdown())
  {
    if (g_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}

