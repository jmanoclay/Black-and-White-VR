#include "core/audio_balance.h"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <intrin.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <MinHook.h>

#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/paths.h"

namespace bw::audio {

namespace {

// --- waveOutSetVolume: the game's audio library writes the volume it read at startup back as it
// closes (LHaudiodllR.dll 10015CFB); its own volume control always sets both sides equal.
using SetVolumeFn = MMRESULT(WINAPI*)(HWAVEOUT, DWORD);
SetVolumeFn o_set_volume = nullptr;

MMRESULT WINAPI set_volume_detour(HWAVEOUT h, DWORD v) {
    const DWORD left = v & 0xFFFF, right = v >> 16;
    if (left != right) {
        const DWORD even = left > right ? left : right;
        static int logged = 0;
        if (logged++ < 8)
            BWLOG("audio: waveOutSetVolume left %04X right %04X (asked for by %p): both set to %04X", unsigned(left),
                  unsigned(right), _ReturnAddress(), unsigned(even));
        v = even | (even << 16);
    }
    return o_set_volume(h, v);
}

// --- The game's audio session (its share of the output device: waveOut and DirectSound both).
// Our own stream joins it (no session id given) and is never started; through it the session's
// channel volumes are read and set.
GUID g_ours = {0x6a3c2f10, 0x4b1d, 0x4e57, {0x9a, 0x61, 0x2b, 0x7d, 0x40, 0x1e, 0x88, 0x35}};

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string device_name(IMMDevice* dev) {
    std::string name = "?";
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
            char buf[128];
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
            name = buf;
        }
        PropVariantClear(&pv);
        ps->Release();
    }
    return name;
}

std::string list(const float* v, UINT32 n) {
    std::string s;
    for (UINT32 i = 0; i < n; ++i) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%s%.2f", i ? "/" : "", v[i]);
        s += buf;
    }
    return s;
}

// Each channel to the loudest one's level; false if they were already even.
bool even_out(IChannelAudioVolume* cv, UINT32 n, std::string& before) {
    float v[16];
    if (FAILED(cv->GetAllVolumes(n, v))) return false;
    float lo = v[0], hi = v[0];
    for (UINT32 i = 1; i < n; ++i) {
        lo = v[i] < lo ? v[i] : lo;
        hi = v[i] > hi ? v[i] : hi;
    }
    before = list(v, n);
    if (hi - lo < 0.001f) return false;
    for (UINT32 i = 0; i < n; ++i) v[i] = hi;
    cv->SetAllVolumes(n, v, &g_ours);
    return true;
}

DWORD WINAPI session_thread(void*) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 0;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IChannelAudioVolume* cv = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IChannelAudioVolume), reinterpret_cast<void**>(&cv));
    UINT32 n = 0;
    if (SUCCEEDED(hr)) hr = cv->GetChannelCount(&n);
    if (FAILED(hr) || n == 0) {
        BWLOG("audio: the game's audio session can't be reached (%08X): its left/right balance is left as it is", unsigned(hr));
        return 0;
    }
    if (n > 16) n = 16;
    std::string before;
    const std::string name = device_name(dev);
    if (even_out(cv, n, before))
        BWLOG("audio: the game's sound on %s was uneven (channel volumes %s): all set to the loudest", name.c_str(),
              before.c_str());
    else
        BWLOG("audio: the game's sound on %s is even (channel volumes %s)", name.c_str(), before.c_str());
    // Something may set it again (the game, or another program): put back, and logged with the time.
    for (int logged = 0;;) {
        Sleep(1000);
        if (even_out(cv, n, before) && logged++ < 20)
            BWLOG("audio: the game's left/right balance was changed (channel volumes %s): evened again", before.c_str());
    }
}

}  // namespace

void install() {
    if (!paths::ini_int(L"audio", L"even_balance", 1)) {
        BWLOG("audio: [audio] even_balance=0: the game's left/right balance is left as it is");
        return;
    }
    if (MH_CreateHookApi(L"winmm.dll", "waveOutSetVolume", reinterpret_cast<void*>(&set_volume_detour),
                         reinterpret_cast<void**>(&o_set_volume)) != MH_OK)
        BWLOG("audio: waveOutSetVolume not hooked");
    // (Not from DllMain itself: the thread runs once the program's DLLs have started.)
    if (HANDLE t = CreateThread(nullptr, 0, &session_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace bw::audio
