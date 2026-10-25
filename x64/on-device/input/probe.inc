/* Included by rungs.c: shares its verified activation gate and log/pump. */
static void rung_n8(void)
{
    if (!g_device || !g_packaged || !g_local_state) {
        stage("n8", "gamepad", "SKIP", "requires verified Xbox UWP activation and LocalState");
        return;
    }
    const WCHAR name[] = L"Windows.Gaming.Input.Gamepad";
    HSTRING cls = NULL;
    __x_ABI_CWindows_CGaming_CInput_CIGamepadStatics *factory = NULL;
    HRESULT hr = WindowsCreateString(name, (UINT32)(sizeof(name)/sizeof(name[0])-1), &cls);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls,
        &IID___x_ABI_CWindows_CGaming_CInput_CIGamepadStatics, (void **)&factory);
    if (cls) WindowsDeleteString(cls);
    if (FAILED(hr)) {
        stage("n8", "gamepad", "FAIL", "RoGetActivationFactory hr=0x%08lx", hx(hr));
        return;
    }
    double start = now_ms();
    for (unsigned second = 0; second < 10 && SUCCEEDED(hr); ++second) {
        __FIVectorView_1_Windows__CGaming__CInput__CGamepad *pads = NULL;
        UINT32 count = 0;
        hr = factory->lpVtbl->get_Gamepads(factory, &pads);
        if (SUCCEEDED(hr)) hr = pads->lpVtbl->get_Size(pads, &count);
        note("n8 second=%u count=%u hr=0x%08lx", second, (unsigned)count, hx(hr));
        for (UINT32 i = 0; i < count && SUCCEEDED(hr); ++i) {
            __x_ABI_CWindows_CGaming_CInput_CIGamepad *pad = NULL;
            GamepadReading reading;
            hr = pads->lpVtbl->GetAt(pads, i, &pad);
            if (SUCCEEDED(hr)) hr = pad->lpVtbl->GetCurrentReading(pad, &reading);
            if (SUCCEEDED(hr)) {
                XINPUT_STATE state;
                wgi_to_xinput(&reading, &state);
                note("n8 second=%u pad=%u timestamp=%llu packet=%lu buttons=0x%04x triggers=%u,%u sticks=%d,%d,%d,%d",
                     second, (unsigned)i, (unsigned long long)reading.Timestamp,
                     (unsigned long)state.dwPacketNumber, state.Gamepad.wButtons,
                     state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                     state.Gamepad.sThumbLX, state.Gamepad.sThumbLY,
                     state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);
            }
            if (pad) pad->lpVtbl->Release(pad);
        }
        if (pads) pads->lpVtbl->Release(pads);
        while (SUCCEEDED(hr) && now_ms() - start < (second + 1) * 1000.0) {
            pump(); Sleep(10);
        }
    }
    factory->lpVtbl->Release(factory);
    stage("n8", "gamepad", FAILED(hr) ? "FAIL" : "PASS",
          "10-second Gamepads enumeration/read probe hr=0x%08lx; zero controllers is valid; packet=0 stateless; no Wine xinput wiring", hx(hr));
}
