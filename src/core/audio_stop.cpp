#include "core/audio_stop.h"

#include <windows.h>
#include <intrin.h>

#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/paths.h"

namespace bw::audio_stop {
namespace {

using PanRateFn = int(WINAPI*)(void* mixer, int channel, DWORD flags, UINT rate);
using FlushFn = int(WINAPI*)(void* mixer, int channel, DWORD flags);
using SleepFn = void(WINAPI*)(DWORD ms);
PanRateFn o_pan_rate = nullptr;
FlushFn o_flush = nullptr;
SleepFn o_sleep = nullptr;

// The three stops' sleeps in LHaudiodllR.dll (v1.20): the return addresses after their
// `push 14h; call [Sleep]` (relative to the DLL's base). LHSampleStop(LH_AudioBank*, ulong, ulong)
// has two, LHSampleStop(LH_SampleInfo*) one.
constexpr uintptr_t kStopSites[] = {0x12CEB, 0x12DAD, 0x12EA2};
uintptr_t g_sites[3] = {};

// After a skipped sleep, the same thread's next SetPanRate(100) and FlushChannel are the stop's
// end: they are put off.
thread_local int t_stopping = 0;  // 1: the sleep was skipped; 2: the pan rate was put off too

struct Pending {
    void* mixer;
    int channel;
    LONGLONG due;
};
CRITICAL_SECTION g_lock;
std::vector<Pending> g_pending;
volatile LONG g_count = 0;
LARGE_INTEGER g_freq;
volatile LONG g_deferred = 0, g_early = 0;

void finish(const Pending& p) {
    o_pan_rate(p.mixer, p.channel, 0, 100);
    o_flush(p.mixer, p.channel, 0);
}

// Before any other call on a channel (or, channel < 0, on all of the mixer's): its stop is finished
// first. (Called from the thunks in front of every QMixer import, with their first two arguments;
// for the few that take no channel, a stop finished a little early is all that can happen.)
void __stdcall complete_for(void* mixer, int channel) {
    if (!g_count) return;
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].mixer == mixer && (g_pending[i].channel == channel || channel < 0)) {
            finish(g_pending[i]);
            InterlockedIncrement(&g_early);
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        } else {
            ++i;
        }
    }
    g_count = LONG(g_pending.size());
    LeaveCriticalSection(&g_lock);
}

void WINAPI sleep_detour(DWORD ms) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ms == 20 && (ret == g_sites[0] || ret == g_sites[1] || ret == g_sites[2])) {
        t_stopping = 1;  // (the fade, already started, goes on in the mixer)
        return;
    }
    o_sleep(ms);
}

int WINAPI pan_rate_detour(void* mixer, int channel, DWORD flags, UINT rate) {
    if (t_stopping == 1 && rate == 100) {
        t_stopping = 2;
        complete_for(mixer, channel);  // (a stop still pending on it: finished now)
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        EnterCriticalSection(&g_lock);
        g_pending.push_back({mixer, channel, now.QuadPart + g_freq.QuadPart * 25 / 1000});
        g_count = LONG(g_pending.size());
        LeaveCriticalSection(&g_lock);
        InterlockedIncrement(&g_deferred);
        return 0;
    }
    t_stopping = 0;
    complete_for(mixer, channel);
    return o_pan_rate(mixer, channel, flags, rate);
}

int WINAPI flush_detour(void* mixer, int channel, DWORD flags) {
    if (t_stopping == 2) {
        t_stopping = 0;
        return 0;  // (done with the pan rate, when it is due)
    }
    t_stopping = 0;
    complete_for(mixer, channel);
    return o_flush(mixer, channel, flags);
}

// A thunk in front of a QMixer import: complete_for(its first two arguments), then the import.
//   push dword [esp+8]; push dword [esp+8]; call complete_for; jmp dword [slot]; slot: dd original
void* make_thunk(void* original) {
    static unsigned char* page = nullptr;
    static size_t used = 0;
    if (!page || used + 32 > 4096) {
        page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!page) return nullptr;
    }
    unsigned char* t = page + used;
    used += 32;
    unsigned char* slot = t + 24;
    memcpy(slot, &original, 4);
    unsigned char* p = t;
    const unsigned char push_arg[] = {0xFF, 0x74, 0x24, 0x08};
    memcpy(p, push_arg, 4), p += 4;
    memcpy(p, push_arg, 4), p += 4;
    *p++ = 0xE8;
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&complete_for) - reinterpret_cast<uintptr_t>(p + 4));
    memcpy(p, &rel, 4), p += 4;
    *p++ = 0xFF, *p++ = 0x25;
    const uint32_t abs = uint32_t(reinterpret_cast<uintptr_t>(slot));
    memcpy(p, &abs, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return t;
}

void write_slot(void** slot, void* value) {
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
}

bool patch(HMODULE dll) {
    const auto base = reinterpret_cast<uintptr_t>(dll);
    // (Only this version of the library: the three sleeps where expected, `push 14h; call [..]`.)
    for (int i = 0; i < 3; ++i) {
        const auto* code = reinterpret_cast<const unsigned char*>(base + kStopSites[i] - 8);
        if (IsBadReadPtr(code, 8) || code[0] != 0x6A || code[1] != 0x14 || code[2] != 0xFF || code[3] != 0x15) {
            BWLOG("audio: the audio library isn't the one expected (stop %d): its sound stops are left to wait", i);
            return false;
        }
        g_sites[i] = base + kStopSites[i];
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int thunks = 0;
    bool sleep_ok = false, pan_ok = false, flush_ok = false;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* dll_name = reinterpret_cast<const char*>(base + d->Name);
        const bool qmixer = _stricmp(dll_name, "QMIXER.dll") == 0, kernel = _stricmp(dll_name, "KERNEL32.dll") == 0;
        if (!qmixer && !kernel) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto** slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
            const char* fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name;
            if (kernel) {
                if (strcmp(fn, "Sleep") == 0) o_sleep = reinterpret_cast<SleepFn>(slots[i]), write_slot(&slots[i], reinterpret_cast<void*>(&sleep_detour)), sleep_ok = true;
            } else if (strcmp(fn, "QSWaveMixSetPanRate") == 0) {
                o_pan_rate = reinterpret_cast<PanRateFn>(slots[i]), write_slot(&slots[i], reinterpret_cast<void*>(&pan_rate_detour)), pan_ok = true;
            } else if (strcmp(fn, "QSWaveMixFlushChannel") == 0) {
                o_flush = reinterpret_cast<FlushFn>(slots[i]), write_slot(&slots[i], reinterpret_cast<void*>(&flush_detour)), flush_ok = true;
            } else if (void* t = make_thunk(slots[i])) {
                write_slot(&slots[i], t), ++thunks;
            }
        }
    }
    BWLOG("audio: sound stops without the 20 ms wait (Sleep %d, SetPanRate %d, FlushChannel %d, %d other mixer calls ordered after them)", sleep_ok,
          pan_ok, flush_ok, thunks);
    return sleep_ok && pan_ok && flush_ok;
}

// Test runs only (the environment's BWVR_MUTE=1, set by the harness): the mixer is never opened,
// so the game runs without sound. (With Windows' audio service stalled, every mixer call waited on
// it and the game hung after loading; 2026-10-31.)
void* WINAPI mixer_init_muted(void*) { return nullptr; }
bool g_mute = false, g_async = false;
void mute(HMODULE dll) {
    const auto base = reinterpret_cast<uintptr_t>(dll);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "QMIXER.dll") != 0) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto** slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i)
            if (!IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal) &&
                strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name, "QSWaveMixInitEx") == 0) {
                write_slot(&slots[i], reinterpret_cast<void*>(&mixer_init_muted));
                BWLOG("audio: BWVR_MUTE: the game's mixer isn't opened (no sound in this test run)");
                return;
            }
    }
}

DWORD WINAPI worker(void*) {
    HMODULE dll = nullptr;
    for (int i = 0; i < 2400 && !(dll = GetModuleHandleW(L"LHaudiodllR.dll")); ++i) Sleep(50);
    if (!dll) {
        BWLOG("audio: the audio library never loaded: its sound stops are left to wait");
        return 0;
    }
    if (g_mute) mute(dll);
    if (!g_async || !patch(dll)) return 0;
    LONG logged = 0;
    LARGE_INTEGER logged_at;
    QueryPerformanceCounter(&logged_at);
    for (;;) {
        Sleep(2);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_count) {
            EnterCriticalSection(&g_lock);
            for (size_t i = 0; i < g_pending.size();) {
                if (g_pending[i].due <= now.QuadPart) {
                    finish(g_pending[i]);
                    g_pending.erase(g_pending.begin() + ptrdiff_t(i));
                } else {
                    ++i;
                }
            }
            g_count = LONG(g_pending.size());
            LeaveCriticalSection(&g_lock);
        }
        if (now.QuadPart - logged_at.QuadPart > g_freq.QuadPart * 60 && g_deferred != logged) {
            logged = g_deferred, logged_at = now;
            BWLOG("audio: %ld sound stops so far without the wait (%ld finished early, the channel used again)", g_deferred, g_early);
        }
    }
}

}  // namespace

void install() {
    char env[8] = {};
    g_mute = GetEnvironmentVariableA("BWVR_MUTE", env, sizeof(env)) > 0 && env[0] == '1';
    g_async = paths::ini_int(L"audio", L"async_stop", 1) != 0;
    if (!g_async) BWLOG("audio: [audio] async_stop=0: the game waits 20 ms for each sound it stops");
    if (!g_async && !g_mute) return;
    if (g_mute)  // (as early as possible: before the game opens its mixer)
        if (HMODULE dll = GetModuleHandleW(L"LHaudiodllR.dll")) mute(dll), g_mute = false;
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    if (HANDLE t = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace bw::audio_stop
