#include "oga_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/audio_stream.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "go2/input.h"
#include "oga_host_display.h"
#include <cinttypes>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
Log_SetChannel(OGAHostInterface);

OGAHostInterface::OGAHostInterface() = default;

OGAHostInterface::~OGAHostInterface()
{
  if (m_input)
    go2_input_destroy(m_input);

  if (m_display)
  {
    DestroyDisplay();
    ImGui::DestroyContext();
  }
}

bool OGAHostInterface::CreateDisplay()
{
  std::unique_ptr<HostDisplay> display = OGAHostDisplay::Create(m_settings.gpu_use_debug_device);
  if (!display)
    return false;

  m_app_icon_texture =
    display->CreateTexture(APP_ICON_WIDTH, APP_ICON_HEIGHT, APP_ICON_DATA, APP_ICON_WIDTH * sizeof(u32));
  if (!display)
    return false;

  m_display = display.release();
  return true;
}

void OGAHostInterface::DestroyDisplay()
{
  m_app_icon_texture.reset();
  delete m_display;
  m_display = nullptr;
}

void OGAHostInterface::CreateImGuiContext()
{
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  //ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f);
}

void OGAHostInterface::ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
}

bool OGAHostInterface::AcquireHostDisplay()
{
  return true;
}

void OGAHostInterface::ReleaseHostDisplay()
{
  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

std::unique_ptr<AudioStream> OGAHostInterface::CreateAudioStream(AudioBackend backend)
{
  return AudioStream::CreateNullAudioStream();
}

void OGAHostInterface::OnSystemCreated()
{
  HostInterface::OnSystemCreated();

  UpdateControllerMapping();
  ClearImGuiFocus();
}

void OGAHostInterface::OnSystemPaused(bool paused)
{
  HostInterface::OnSystemPaused(paused);

  if (!paused)
    ClearImGuiFocus();
}

void OGAHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();
}

void OGAHostInterface::OnControllerTypeChanged(u32 slot)
{
  HostInterface::OnControllerTypeChanged(slot);

  UpdateControllerMapping();
}

void OGAHostInterface::RunLater(std::function<void()> callback)
{
  // TODO
  Panic("RunLater");
}

void OGAHostInterface::SaveSettings()
{
  INISettingsInterface si(GetSettingsFileName().c_str());
  m_settings_copy.Save(si);
}

void OGAHostInterface::UpdateSettings()
{
  HostInterface::UpdateSettings([this]() { m_settings = m_settings_copy; });
}

void OGAHostInterface::UpdateControllerMapping() {}

void OGAHostInterface::UpdateInput() {}

std::unique_ptr<OGAHostInterface> OGAHostInterface::Create()
{
  std::unique_ptr<OGAHostInterface> intf = std::make_unique<OGAHostInterface>();

  // Settings need to be loaded prior to creating the window for OpenGL bits.
  INISettingsInterface si(intf->GetSettingsFileName().c_str());
  intf->m_settings_copy.Load(si);
  intf->m_settings = intf->m_settings_copy;

  intf->m_input = go2_input_create();
  if (!intf->m_input)
  {
    Log_ErrorPrintf("Failed to create go2 input");
    return nullptr;
  }

  intf->CreateImGuiContext();
  if (!intf->CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    return nullptr;
  }

  ImGui::NewFrame();

  return intf;
}

void OGAHostInterface::ReportError(const char* message)
{
  // TODO
  HostInterface::ReportError(message);
}

void OGAHostInterface::ReportMessage(const char* message)
{
  AddOSDMessage(message, 2.0f);
}

bool OGAHostInterface::ConfirmMessage(const char* message)
{
  // TODO
  return HostInterface::ConfirmMessage(message);
}

void OGAHostInterface::DrawImGui()
{
  if (m_system)
    DrawFPSWindow();
  else
    DrawPoweredOffWindow();

  DrawOSDMessages();

  ImGui::Render();
}

void OGAHostInterface::DrawPoweredOffWindow()
{
  static constexpr int WINDOW_WIDTH = 400;
  static constexpr int WINDOW_HEIGHT = 650;
  static constexpr int BUTTON_WIDTH = 200;
  static constexpr int BUTTON_HEIGHT = 40;

  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(WINDOW_WIDTH), static_cast<float>(WINDOW_HEIGHT)));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  if (!ImGui::Begin("Powered Off", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoBringToFrontOnFocus))
  {
    ImGui::End();
  }

  ImGui::SetCursorPosX(static_cast<float>((WINDOW_WIDTH - APP_ICON_WIDTH) / 2));
  ImGui::Image(m_app_icon_texture->GetHandle(),
               ImVec2(static_cast<float>(APP_ICON_WIDTH), static_cast<float>(APP_ICON_HEIGHT)));
  ImGui::SetCursorPosY(static_cast<float>(APP_ICON_HEIGHT + 32));

  const ImVec2 button_size(static_cast<float>(BUTTON_WIDTH), static_cast<float>(BUTTON_HEIGHT));
  const float button_left = static_cast<float>((WINDOW_WIDTH - BUTTON_WIDTH) / 2);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, 0xFF202020);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, 0xFF808080);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0xFF575757);

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Resume", button_size))
  {
    RunLater([this]() { ResumeSystemFromMostRecentState(); });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start Disc", button_size))
  {
    //RunLater([this]() { DoStartDisc(); });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Start BIOS", button_size))
  {
    RunLater([this]() { BootSystemFromFile(nullptr); });
    ClearImGuiFocus();
  }
  ImGui::NewLine();

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Load State", button_size))
    ImGui::OpenPopup("PowerOffWindow_LoadStateMenu");
  if (ImGui::BeginPopup("PowerOffWindow_LoadStateMenu"))
  {
    for (u32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "State %u", i);
      if (ImGui::MenuItem(buf))
      {
        RunLater([this, i]() { LoadState(true, i); });
        ClearImGuiFocus();
      }
    }
    ImGui::EndPopup();
  }
  ImGui::NewLine();

#if 0
  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Settings", button_size))
    m_settings_window_open = true;
  ImGui::NewLine();
#endif

  ImGui::SetCursorPosX(button_left);
  if (ImGui::Button("Exit", button_size))
    m_quit_request = true;

  ImGui::NewLine();

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  ImGui::End();
}

void OGAHostInterface::Run()
{
  while (!m_quit_request)
  {
    UpdateInput();

    if (m_system && !m_paused)
      m_system->RunFrame();

    // rendering
    {
      DrawImGui();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      m_display->Render();

      if (m_system)
      {
        m_system->GetGPU()->RestoreGraphicsAPIState();

        if (m_speed_limiter_enabled)
          m_system->Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (m_system)
  {
    if (m_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}
