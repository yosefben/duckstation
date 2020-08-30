#include "konami_justifier.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "resources.h"
#include "system.h"
#include <array>
Log_SetChannel(KonamiJustifier);

KonamiJustifier::KonamiJustifier()
{
  m_irq_event =
    TimingEvents::CreateTimingEvent("Konami Justifier IRQ", 1, 1, std::bind(&KonamiJustifier::IRQEvent, this), false);
}

KonamiJustifier::~KonamiJustifier() = default;

ControllerType KonamiJustifier::GetType() const
{
  return ControllerType::NamcoGunCon;
}

std::optional<s32> KonamiJustifier::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> KonamiJustifier::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void KonamiJustifier::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool KonamiJustifier::DoState(StateWrapper& sw)
{
  if (!Controller::DoState(sw))
    return false;

  sw.Do(&m_position_line);
  sw.Do(&m_position_tick);
  sw.Do(&m_irq_start_line);
  sw.Do(&m_irq_end_line);
  sw.Do(&m_irq_current_line);
  sw.Do(&m_button_state);
  sw.Do(&m_position_valid);
  sw.Do(&m_transfer_state);

  if (sw.IsReading())
    UpdateIRQEvent();

  return true;
}

void KonamiJustifier::SetAxisState(s32 axis_code, float value) {}

void KonamiJustifier::SetButtonState(Button button, bool pressed)
{
  static constexpr std::array<u8, static_cast<size_t>(Button::Count)> indices = {{15, 3, 14}};
  if (pressed)
    m_button_state &= ~(u16(1) << indices[static_cast<u8>(button)]);
  else
    m_button_state |= u16(1) << indices[static_cast<u8>(button)];
}

void KonamiJustifier::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

bool KonamiJustifier::IsTriggerPressed() const
{
  return ((m_button_state & (1u << 15)) != 0);
}

void KonamiJustifier::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool KonamiJustifier::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A31;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        UpdatePosition();
        return true;
      }
      else
      {
        *data_out = 0xFF;
        return (data_in == 0x01);
      }
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = Truncate8(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::Idle;
      return true;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

void KonamiJustifier::UpdatePosition()
{
  // get screen coordinates
  const HostDisplay* display = g_host_interface->GetDisplay();
  const s32 mouse_x = display->GetMousePositionX();
  const s32 mouse_y = display->GetMousePositionY();

  // are we within the active display area?
  if (mouse_x < 0 || mouse_y < 0 ||
      !g_gpu->ConvertScreenCoordinatesToBeamTicksAndLines(mouse_x, mouse_y, &m_position_tick, &m_position_line))
  {
    Log_DevPrintf("Lightgun out of range for window coordinates %d,%d", mouse_x, mouse_y);
    m_position_valid = false;
  }
  else
  {
    m_position_valid = true;
    m_irq_start_line = std::max<u32>((m_position_line >= HIT_LINE_OFFSET) ? (m_position_line - HIT_LINE_OFFSET) : 0, g_gpu->GetCRTCActiveStartLine());
    m_irq_end_line = std::min<u32>(m_position_line + HIT_LINE_OFFSET, g_gpu->GetCRTCActiveEndLine());

    Log_DevPrintf("Lightgun window coordinates %d,%d -> tick %u line %u [%u-%u]", mouse_x, mouse_y, m_position_tick,
                  m_position_line, m_irq_start_line, m_irq_end_line);

    m_position_tick = m_position_tick + 100;
  }

  UpdateIRQEvent();
}

void KonamiJustifier::UpdateIRQEvent()
{
  m_irq_event->Deactivate();

  if (!m_position_valid)
    return;

  u32 target_line = m_irq_current_line;
  if (target_line < m_irq_start_line || target_line > m_irq_end_line)
    target_line = m_irq_start_line;
  else
    m_irq_current_line++;

  const TickCount ticks_until_pos = g_gpu->GetSystemTicksUntilTicksAndLine(m_position_tick, target_line);
  Log_DevPrintf("Triggering IRQ in %d ticks @ tick %u line %u", ticks_until_pos, m_position_tick, target_line);
  m_irq_event->Schedule(ticks_until_pos);
}

void KonamiJustifier::IRQEvent()
{
  g_interrupt_controller.InterruptRequest(InterruptController::IRQ::IRQ10);
  UpdateIRQEvent();
}

std::unique_ptr<KonamiJustifier> KonamiJustifier::Create()
{
  return std::make_unique<KonamiJustifier>();
}

std::optional<s32> KonamiJustifier::StaticGetAxisCodeByName(std::string_view button_name)
{
  return std::nullopt;
}

std::optional<s32> KonamiJustifier::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Trigger);
  BUTTON(Start);
  BUTTON(Back);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList KonamiJustifier::StaticGetAxisNames()
{
  return {};
}

Controller::ButtonList KonamiJustifier::StaticGetButtonNames()
{
  return {{TRANSLATABLE("NamcoGunCon", "Trigger"), static_cast<s32>(Button::Trigger)},
          {TRANSLATABLE("NamcoGunCon", "Start"), static_cast<s32>(Button::Start)},
          {TRANSLATABLE("NamcoGunCon", "Back"), static_cast<s32>(Button::Back)}};
}

u32 KonamiJustifier::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList KonamiJustifier::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 2> settings = {
    {{SettingInfo::Type::Path, "CrosshairImagePath", "Crosshair Image Path",
      "Path to an image to use as a crosshair/cursor."},
     {SettingInfo::Type::Float, "CrosshairScale", "Crosshair Image Scale", "Scale of crosshair image on screen.", "1.0",
      "0.0001", "100.0"}}};

  return SettingList(settings.begin(), settings.end());
}

void KonamiJustifier::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);

  std::string path = g_host_interface->GetStringSettingValue(section, "CrosshairImagePath");
  if (path != m_crosshair_image_path)
  {
    m_crosshair_image_path = std::move(path);
    if (m_crosshair_image_path.empty() ||
        !Common::LoadImageFromFile(&m_crosshair_image, m_crosshair_image_path.c_str()))
    {
      m_crosshair_image.Invalidate();
    }
  }

  if (!m_crosshair_image.IsValid())
  {
    m_crosshair_image.SetPixels(Resources::CROSSHAIR_IMAGE_WIDTH, Resources::CROSSHAIR_IMAGE_HEIGHT,
                                Resources::CROSSHAIR_IMAGE_DATA.data());
  }

  m_crosshair_image_scale = g_host_interface->GetFloatSettingValue(section, "CrosshairScale", 1.0f);
}

bool KonamiJustifier::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale)
{
  if (!m_crosshair_image.IsValid())
    return false;

  *image = &m_crosshair_image;
  *image_scale = m_crosshair_image_scale;
  return true;
}
