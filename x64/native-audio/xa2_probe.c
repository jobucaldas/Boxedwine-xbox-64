#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

typedef HRESULT (WINAPI *create_fn)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

int main(void)
{
    IXAudio2 *engine = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *source = NULL;
    HMODULE dll = NULL;
    int result = 1;
    const unsigned rate = 48000;
    int16_t samples[48000];
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0};
    XAUDIO2_BUFFER buffer = {0};
    XAUDIO2_VOICE_STATE state = {0};
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("XA2 COM hr=0x%08lx\n", (unsigned long)hr); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Normal in-process DLL resolution: Wine supplies its builtin off-device. */
    dll = LoadLibraryW(L"xaudio2_9.dll");
    create_fn create = dll ? (create_fn)(void *)GetProcAddress(dll, "XAudio2Create") : NULL;
    if (!create) { printf("XA2 load error=%lu\n", GetLastError()); goto done; }
    hr = create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    printf("XA2 create hr=0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) goto done;
    hr = IXAudio2_CreateMasteringVoice(engine, &master, 1, rate, 0, NULL, NULL, AudioCategory_GameEffects);
    if (FAILED(hr)) { printf("XA2 mastering hr=0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IXAudio2_CreateSourceVoice(engine, &source, &format, 0, 2.0f, NULL, NULL, NULL);
    if (FAILED(hr)) { printf("XA2 source hr=0x%08lx\n", (unsigned long)hr); goto done; }
    puts("XA2 voices ok");
    for (unsigned i = 0; i < rate; ++i)
        samples[i] = (int16_t)(8192.0 * sin(2.0 * 3.141592653589793 * 440.0 * i / rate));
    /* Keep the sample counter after draining (END_OF_STREAM resets it). */
    buffer.Flags = 0;
    buffer.AudioBytes = sizeof(samples);
    buffer.pAudioData = (const BYTE *)samples;
    hr = IXAudio2SourceVoice_SubmitSourceBuffer(source, &buffer, NULL);
    if (FAILED(hr)) { printf("XA2 submit hr=0x%08lx\n", (unsigned long)hr); goto done; }
    hr = IXAudio2SourceVoice_Start(source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) { printf("XA2 start hr=0x%08lx\n", (unsigned long)hr); goto done; }
    ULONGLONG start = GetTickCount64();
    do {
        Sleep(10);
        IXAudio2SourceVoice_GetState(source, &state, 0);
    } while (state.BuffersQueued && GetTickCount64() - start < 5000);
    if (state.BuffersQueued || state.SamplesPlayed < rate) {
        printf("XA2 incomplete samples=%llu queued=%u\n", (unsigned long long)state.SamplesPlayed, state.BuffersQueued);
        goto done;
    }
    printf("XA2 played_ms=%llu samples=%llu\n", (unsigned long long)(state.SamplesPlayed * 1000 / rate), (unsigned long long)state.SamplesPlayed);
    result = 0;
done:
    if (source) IXAudio2SourceVoice_DestroyVoice(source);
    if (master) IXAudio2MasteringVoice_DestroyVoice(master);
    if (engine) IXAudio2_Release(engine);
    if (dll) FreeLibrary(dll);
    CoUninitialize();
    return result;
}
