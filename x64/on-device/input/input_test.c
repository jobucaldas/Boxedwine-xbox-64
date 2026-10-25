#include "mapping.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
    const uint16_t expected[] = {16,32,4096,8192,16384,32768,1,2,4,8,256,512,64,128};
    GamepadReading r = {0}; XINPUT_STATE s;
    for (unsigned i = 0; i < 14; ++i) {
        r.Buttons = 1u << i; wgi_to_xinput(&r, &s);
        CHECK(s.Gamepad.wButtons == expected[i]); CHECK(s.dwPacketNumber == 0);
    }
    r.Buttons = 0x3ffff; wgi_to_xinput(&r, &s); CHECK(s.Gamepad.wButtons == 0xf3ff);
    r.Buttons = 0x3c000; wgi_to_xinput(&r, &s); CHECK(s.Gamepad.wButtons == 0);
    const double values[] = {-2,-1,-0.5,-0.001,0,0.001,0.5,1,2,NAN,INFINITY};
    const int sticks[] = {-32768,-32768,-16384,-33,0,33,16384,32767,32767,0,0};
    const unsigned triggers[] = {0,0,0,0,0,0,128,255,255,0,0};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        r.LeftTrigger = r.RightTrigger = values[i];
        r.LeftThumbstickX = r.LeftThumbstickY = r.RightThumbstickX = r.RightThumbstickY = values[i];
        wgi_to_xinput(&r, &s);
        CHECK(s.Gamepad.bLeftTrigger == triggers[i] && s.Gamepad.bRightTrigger == triggers[i]);
        CHECK(s.Gamepad.sThumbLX == sticks[i] && s.Gamepad.sThumbLY == sticks[i]);
        CHECK(s.Gamepad.sThumbRX == sticks[i] && s.Gamepad.sThumbRY == sticks[i]);
    }
    r = (GamepadReading){0}; wgi_to_xinput(&r, &s);
    CHECK(s.Gamepad.wButtons == 0 && s.Gamepad.sThumbLX == 0);
    puts("PASS (28 mapping cases: 14 buttons, combined, paddles, 11 analog, neutral)");
    return 0;
}
