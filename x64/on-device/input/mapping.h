#ifndef INPUT_MAPPING_H
#define INPUT_MAPPING_H
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#include <windows.gaming.input.h>
typedef struct __x_ABI_CWindows_CGaming_CInput_CGamepadReading GamepadReading;
#else
typedef struct {
    uint64_t Timestamp;
    uint32_t Buttons;
    double LeftTrigger, RightTrigger;
    double LeftThumbstickX, LeftThumbstickY, RightThumbstickX, RightThumbstickY;
} GamepadReading;
typedef struct {
    uint16_t wButtons;
    uint8_t bLeftTrigger, bRightTrigger;
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct { uint32_t dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
#endif
/* Stateless conversion: packet number is zero (caller tracks changes).
 * Nearest integer; negative sticks use 32768, positive sticks 32767.
 * No deadzone. Unsupported paddle bits are ignored; nonfinite input becomes 0. */
void wgi_to_xinput(const GamepadReading *reading, XINPUT_STATE *state);
#endif
