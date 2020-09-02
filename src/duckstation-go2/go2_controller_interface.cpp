#include "go2_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include <cmath>
Log_SetChannel(Go2ControllerInterface);

Go2ControllerInterface::Go2ControllerInterface() = default;

Go2ControllerInterface::~Go2ControllerInterface()
{
  if (m_input_state)
    go2_input_state_destroy(m_input_state);
  if (m_input)
    go2_input_destroy(m_input);
}

ControllerInterface::Backend Go2ControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::None;
}

bool Go2ControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  m_input = go2_input_create();
  m_input_state = go2_input_state_create();
  if (!m_input || !m_input_state)
    return false;

  if (!ControllerInterface::Initialize(host_interface))
    return false;

  return true;
}

void Go2ControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

void Go2ControllerInterface::PollEvents()
{
  go2_input_state_read(m_input, m_input_state);
  CheckForStateChanges();
}

void Go2ControllerInterface::CheckForStateChanges()
{
  for (u32 i = 0; i < NUM_BUTTONS; i++)
  {
    const bool new_state = go2_input_state_button_get(m_input_state, static_cast<go2_input_button_t>(i)) == ButtonState_Pressed;
    if (m_last_button_state[i] == new_state)
      continue;

    HandleButtonEvent(i, new_state);
    m_last_button_state[i] = new_state;
  }

  go2_thumb_t thumb = go2_input_state_thumbstick_get(m_input_state, Go2InputThumbstick_Left);
  if (thumb.x != m_last_axis_state[0])
  {
    HandleAxisEvent(Axis::X, thumb.x);
    m_last_axis_state[0] = thumb.x;
  }
  if (thumb.y != m_last_axis_state[1])
  {
    HandleAxisEvent(Axis::Y, thumb.y);
    m_last_axis_state[1] = thumb.y;
  }
}

void Go2ControllerInterface::ClearBindings()
{
  for (AxisCallback& ac : m_axis_mapping)
    ac = {};
  for (ButtonCallback& bc : m_button_mapping)
    bc = {};
}

bool Go2ControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisCallback callback)
{
  if (controller_index != 0)
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_axis_mapping[axis_number] = std::move(callback);
  return true;
}

bool Go2ControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  if (controller_index != 0)
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_button_mapping[button_number] = std::move(callback);
  return true;
}

bool Go2ControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                           ButtonCallback callback)
{
  if (controller_index != 0)
    return false;

  if (axis_number < 0 || axis_number >= NUM_AXISES)
    return false;

  m_axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool Go2ControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                           AxisCallback callback)
{
  if (controller_index != 0)
    return false;

  if (button_number < 0 || button_number >= NUM_BUTTONS)
    return false;

  m_button_axis_mapping[button_number] = std::move(callback);
  return true;
}

bool Go2ControllerInterface::HandleAxisEvent(Axis axis, float value)
{
  Log_DevPrintf("axis %u %f", static_cast<u32>(axis), value);
  if (DoEventHook(Hook::Type::Axis, 0, static_cast<u32>(axis), value))
    return true;

  const AxisCallback& cb = m_axis_mapping[static_cast<u32>(axis)];
  if (cb)
  {
    // Apply axis scaling only when controller axis is mapped to an axis
    cb(std::clamp(m_axis_scale * value, -1.0f, 1.0f));
    return true;
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= m_deadzone);
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb =
    m_axis_button_mapping[static_cast<u32>(axis)][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb =
    m_axis_button_mapping[static_cast<u32>(axis)][BoolToUInt8(positive)];
  if (button_cb)
  {
    button_cb(outside_deadzone);
    if (other_button_cb)
      other_button_cb(false);
    return true;
  }
  else if (other_button_cb)
  {
    other_button_cb(false);
    return true;
  }
  else
  {
    return false;
  }
}

bool Go2ControllerInterface::HandleButtonEvent(u32 button, bool pressed)
{
  Log_DevPrintf("button %u %s", button, pressed ? "pressed" : "released");
  if (DoEventHook(Hook::Type::Button, 0, button, pressed ? 1.0f : 0.0f))
    return true;

  const ButtonCallback& cb = m_button_mapping[button];
  if (cb)
  {
    cb(pressed);
    return true;
  }

  // Assume a half-axis, i.e. in 0..1 range
  const AxisCallback& axis_cb = m_button_axis_mapping[button];
  if (axis_cb)
  {
    axis_cb(pressed ? 1.0f : 0.0f);
  }
  return true;
}

u32 Go2ControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  return 0;
}

void Go2ControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths,
                                                            u32 num_motors)
{
}

bool Go2ControllerInterface::SetControllerAxisScale(int controller_index, float scale /* = 1.00f */)
{
  if (controller_index != 0)
    return false;

  m_axis_scale = std::clamp(std::abs(scale), 0.01f, 1.50f);
  Log_InfoPrintf("Controller %d axis scale set to %f", controller_index, m_axis_scale);
  return true;
}

bool Go2ControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  if (controller_index != 0)
    return false;

  m_deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index, m_deadzone);
  return true;
}
