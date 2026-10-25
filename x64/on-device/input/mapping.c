#include "mapping.h"
#include <math.h>
#include <string.h>
static unsigned char trigger(double v)
{
    if (!isfinite(v) || v <= 0) return 0;
    if (v >= 1) return 255;
    return (unsigned char)(v * 255 + 0.5);
}
static int16_t stick(double v)
{
    if (!isfinite(v)) return 0;
    if (v <= -1) return -32768;
    if (v >= 1) return 32767;
    return (int16_t)(v < 0 ? v * 32768 - 0.5 : v * 32767 + 0.5);
}
void wgi_to_xinput(const GamepadReading *r, XINPUT_STATE *s)
{
    static const uint16_t buttons[] = {
        0x0010, 0x0020, 0x1000, 0x2000, 0x4000, 0x8000,
        0x0001, 0x0002, 0x0004, 0x0008, 0x0100, 0x0200, 0x0040, 0x0080
    };
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        if ((uint32_t)r->Buttons & (1u << i)) s->Gamepad.wButtons |= buttons[i];
    s->Gamepad.bLeftTrigger = trigger(r->LeftTrigger);
    s->Gamepad.bRightTrigger = trigger(r->RightTrigger);
    s->Gamepad.sThumbLX = stick(r->LeftThumbstickX);
    s->Gamepad.sThumbLY = stick(r->LeftThumbstickY);
    s->Gamepad.sThumbRX = stick(r->RightThumbstickX);
    s->Gamepad.sThumbRY = stick(r->RightThumbstickY);
}
