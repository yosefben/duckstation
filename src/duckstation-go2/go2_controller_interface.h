#pragma once
#include "frontend-common/controller_interface.h"
#include "core/types.h"
#include <go2/input.h>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class Go2ControllerInterface final : public ControllerInterface
{
public:
  Go2ControllerInterface();
  ~Go2ControllerInterface() override;

  Backend GetBackend() const override;
  bool Initialize(CommonHostInterface* host_interface) override;
  void Shutdown() override;

  // Removes all bindings. Call before setting new bindings.
  void ClearBindings() override;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  bool BindControllerAxis(int controller_index, int axis_number, AxisCallback callback) override;
  bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) override;
  bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                  ButtonCallback callback) override;
  bool BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback) override;

  // Changing rumble strength.
  u32 GetControllerRumbleMotorCount(int controller_index) override;
  void SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors) override;

  // Set scaling that will be applied on axis-to-axis mappings
  bool SetControllerAxisScale(int controller_index, float scale = 1.00f) override;

  // Set deadzone that will be applied on axis-to-button mappings
  bool SetControllerDeadzone(int controller_index, float size = 0.25f) override;

  void PollEvents() override;

private:
  enum : u32
  {
    NUM_BUTTONS = Go2InputButton_TriggerRight + 1,
    NUM_AXISES = 2
  };
  enum class Axis : u32
  {
    X,
    Y
  };

  void CheckForStateChanges();
  bool HandleAxisEvent(Axis axis, float value);
  bool HandleButtonEvent(u32 button, bool pressed);

  go2_input_t* m_input = nullptr;
  go2_input_state_t* m_input_state = nullptr;

  std::array<bool, NUM_BUTTONS> m_last_button_state = {};
  std::array<float, NUM_AXISES> m_last_axis_state = {};

  // Scaling value of 1.30f to 1.40f recommended when using recent controllers
  float m_axis_scale = 1.00f;
  float m_deadzone = 0.25f;

  std::array<AxisCallback, NUM_AXISES> m_axis_mapping;
  std::array<ButtonCallback, NUM_BUTTONS> m_button_mapping;
  std::array<std::array<ButtonCallback, 2>, NUM_AXISES> m_axis_button_mapping;
  std::array<AxisCallback, NUM_BUTTONS> m_button_axis_mapping;
};
