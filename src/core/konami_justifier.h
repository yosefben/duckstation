#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class TimingEvent;

class KonamiJustifier final : public Controller
{
public:
  enum class Button : u8
  {
    Trigger = 0,
    Start = 1,
    Back = 2,
    Count
  };

  KonamiJustifier();
  ~KonamiJustifier() override;

  static std::unique_ptr<KonamiJustifier> Create();
  static std::optional<s32> StaticGetAxisCodeByName(std::string_view button_name);
  static std::optional<s32> StaticGetButtonCodeByName(std::string_view button_name);
  static AxisList StaticGetAxisNames();
  static ButtonList StaticGetButtonNames();
  static u32 StaticGetVibrationMotorCount();
  static SettingList StaticGetSettings();

  ControllerType GetType() const override;
  std::optional<s32> GetAxisCodeByName(std::string_view axis_name) const override;
  std::optional<s32> GetButtonCodeByName(std::string_view button_name) const override;

  void Reset() override;
  bool DoState(StateWrapper& sw) override;
  void LoadSettings(const char* section) override;
  bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale) override;

  void SetAxisState(s32 axis_code, float value) override;
  void SetButtonState(s32 button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void SetButtonState(Button button, bool pressed);

private:
  bool IsTriggerPressed() const;
  void UpdatePosition();
  void UpdateIRQEvent();
  void IRQEvent();

  enum : u32
  {
    HIT_LINE_OFFSET = 6
  };

  enum class TransferState : u8
  {
    Idle,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    XLSB,
    XMSB,
    YLSB,
    YMSB
  };

  std::unique_ptr<TimingEvent> m_irq_event;

  Common::RGBA8Image m_crosshair_image;
  std::string m_crosshair_image_path;
  float m_crosshair_image_scale = 1.0f;

  u32 m_position_line = 0;
  u32 m_position_tick = 0;
  u32 m_irq_start_line = 0;
  u32 m_irq_end_line = 0;
  u32 m_irq_current_line = 0;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  bool m_position_valid = false;

  TransferState m_transfer_state = TransferState::Idle;
};
