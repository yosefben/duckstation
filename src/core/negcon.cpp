#include "negcon.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include <array>
#include <cmath>

NeGcon::NeGcon()
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
}

NeGcon::~NeGcon() = default;

ControllerType NeGcon::GetType() const
{
  return ControllerType::NeGcon;
}

std::optional<s32> NeGcon::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> NeGcon::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void NeGcon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::DoState(StateWrapper& sw)
{
  if (!Controller::DoState(sw))
    return false;

  sw.Do(&m_button_state);
  sw.Do(&m_transfer_state);
  return true;
}

void NeGcon::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // Steering Axis: -1..1 -> 0..255
  if (axis_code == static_cast<s32>(Axis::Steering))
  {
    const u8 u8_value = static_cast<u8>(std::clamp(((value + 1.0f) / 2.0f) * 255.0f, 0.0f, 255.0f));

    SetAxisState(static_cast<Axis>(axis_code), u8_value);

    return;
  }

  // I, II, L: 0..1 -> 0..255 or -1..0 -> 0..255 to support negative axis ranges,
  // e.g. if bound to analog stick instead of trigger
  const u8 u8_value = static_cast<u8>(std::clamp(std::abs(value) * 255.0f, 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void NeGcon::SetAxisState(Axis axis, u8 value)
{
  m_axis_state[static_cast<u8>(axis)] = value;
}

void NeGcon::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void NeGcon::SetButtonState(Button button, bool pressed)
{
  // Mapping of Button to index of corresponding bit in m_button_state
  static constexpr std::array<u8, static_cast<size_t>(Button::Count)> indices = {3, 4, 5, 6, 7, 11, 12, 13};

  if (pressed)
    m_button_state &= ~(u16(1) << indices[static_cast<u8>(button)]);
  else
    m_button_state |= u16(1) << indices[static_cast<u8>(button)];
}

void NeGcon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A23;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
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
      m_transfer_state = TransferState::AnalogSteering;
      return true;
    }

    case TransferState::AnalogSteering:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::Steering)]);
      m_transfer_state = TransferState::AnalogI;
      return true;
    }

    case TransferState::AnalogI:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::I)]);
      m_transfer_state = TransferState::AnalogII;
      return true;
    }

    case TransferState::AnalogII:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::II)]);
      m_transfer_state = TransferState::AnalogL;
      return true;
    }

    case TransferState::AnalogL:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::L)]);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<NeGcon> NeGcon::Create()
{
  return std::make_unique<NeGcon>();
}

std::optional<s32> NeGcon::StaticGetAxisCodeByName(std::string_view axis_name)
{
#define AXIS(name)                                                                                                     \
  if (axis_name == #name)                                                                                              \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Axis::name)));                                                \
  }

  AXIS(Steering);
  AXIS(I);
  AXIS(II);
  AXIS(L);

  return std::nullopt;

#undef AXIS
}

std::optional<s32> NeGcon::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Up);
  BUTTON(Down);
  BUTTON(Left);
  BUTTON(Right);
  BUTTON(A);
  BUTTON(B);
  BUTTON(R);
  BUTTON(Start);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList NeGcon::StaticGetAxisNames()
{
#define A(n, t)                                                                                                        \
  {                                                                                                                    \
    #n, static_cast <s32>(Axis::n), Controller::AxisType::t                                                            \
  }

  return {A(Steering, Full), A(I, Half), A(II, Half), A(L, Half)};

#undef A
}

Controller::ButtonList NeGcon::StaticGetButtonNames()
{
#define B(n)                                                                                                           \
  {                                                                                                                    \
    #n, static_cast <s32>(Button::n)                                                                                   \
  }

  return {B(Up), B(Down), B(Left), B(Right), B(A), B(B), B(R), B(Start)};
#undef B
}

u32 NeGcon::StaticGetVibrationMotorCount()
{
  return 0;
}
