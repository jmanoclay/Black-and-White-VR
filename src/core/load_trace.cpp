// What the game opens and reads from the registry while it plays, and how long each takes.
// See load_trace.h.
#include "core/load_trace.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace bw::load_trace {
namespace {

std::atomic<int> g_trace{0};     // log this many more calls, slow or not
std::atomic<int> g_slow_left{60};  // calls over kSlowMs logged (a few, then only the totals)
constexpr double kSlowMs = 2.0;
LARGE_INTEGER g_freq;

// Totals since the last report: opens, profile reads, and their time.
std::atomic<long> g_opens{0}, g_reads{0};
std::atomic<long long> g_open_ticks{0}, g_read_ticks{0};

double ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return double(b.QuadPart - a.QuadPart) * 1000.0 / double(g_freq.QuadPart); }

void note(bool open, const char* what, const char* name, const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    (open ? g_opens : g_reads)++;
    (open ? g_open_ticks : g_read_ticks) += t1.QuadPart - t0.QuadPart;
    const double d = ms(t0, t1);
    const bool traced = g_trace.load() > 0 && g_trace.fetch_sub(1) > 0;
    if (traced || (d > kSlowMs && g_slow_left.fetch_sub(1) > 0))
        BWLOG("load: %s %s: %.2f ms (thread %lu)", what, name ? name : "-", d, GetCurrentThreadId());
}

// LHOSFile::Open(this, name, mode): thiscall, as fastcall with edx unused.
using OpenFn = int(__fastcall*)(void*, void*, const char*, int);
OpenFn o_open = nullptr;
int __fastcall open_detour(void* self, void* edx, const char* name, int mode) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const int r = o_open(self, edx, name, mode);
    note(true, "open", name, t0);
    return r;
}

// The multiplayer library's profile reads (the registry): cdecl, the key first.
using Read3 = int(__cdecl*)(char*, void*, void*);
using Read2 = int(__cdecl*)(char*, void*);
struct Slot {
    uintptr_t imp;
    const char* name;
    void* original;
};
Slot g_slots[] = {
    {0x8A9638, "profile ulong", nullptr},   // LHNetGetCurrentProfileUlong(key, unsigned long*)
    {0x8A9608, "profile double", nullptr},  // LHNetGetCurrentProfileDouble(key, double*)
    {0x8A9614, "profile wstring", nullptr}, // LHNetGetCurrentProfileWString(key, wchar_t*, unsigned long*)
    {0x8A95F0, "profile string", nullptr},  // LHNetGetCurrentProfileString(key, char*, unsigned long*)
    {0x8A947C, "profile data", nullptr},    // LHNetGetCurrentProfileData(key, unsigned char*, unsigned long*)
};
template <int I>
int __cdecl read2(char* key, void* a) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const int r = reinterpret_cast<Read2>(g_slots[I].original)(key, a);
    note(false, g_slots[I].name, key, t0);
    return r;
}
template <int I>
int __cdecl read3(char* key, void* a, void* b) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const int r = reinterpret_cast<Read3>(g_slots[I].original)(key, a, b);
    note(false, g_slots[I].name, key, t0);
    return r;
}
void* const g_detours[] = {reinterpret_cast<void*>(&read2<0>), reinterpret_cast<void*>(&read2<1>), reinterpret_cast<void*>(&read3<2>),
                           reinterpret_cast<void*>(&read3<3>), reinterpret_cast<void*>(&read3<4>)};

// CreateFileA from the game's own imports (its C runtime's fopen, LHOSFile through it): the file
// name, and for a slow one the game's code that asked (the first few return addresses in the exe).
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn o_create = nullptr;
HANDLE WINAPI create_detour(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const HANDLE h = o_create(name, access, share, sa, disp, flags, tmpl);
    QueryPerformanceCounter(&t1);
    const double d = ms(t0, t1);
    const bool traced = g_trace.load() > 0 && g_trace.fetch_sub(1) > 0;
    if (traced || (d > kSlowMs && g_slow_left.fetch_sub(1) > 0)) {
        void* frames[24] = {};
        const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        char callers[160] = "";
        int k = 0;
        for (USHORT i = 0; i < n && k < 5; ++i) {
            const auto a = reinterpret_cast<uintptr_t>(frames[i]);
            if (a >= 0x400000 && a < 0x8A0000) {
                const size_t len = strlen(callers);
                snprintf(callers + len, sizeof(callers) - len, " %06X", unsigned(a)), ++k;
            }
        }
        BWLOG("load: CreateFileA %s (access %lX, disposition %lu): %.2f ms, from%s", name ? name : "-", access, disp, d, callers);
    }
    return h;
}

}  // namespace

void install() {
    QueryPerformanceFrequency(&g_freq);
    const bool open_ok = MH_CreateHook(reinterpret_cast<void*>(0x7BC730), reinterpret_cast<void*>(&open_detour), reinterpret_cast<void**>(&o_open)) ==
                             MH_OK &&
                         MH_EnableHook(reinterpret_cast<void*>(0x7BC730)) == MH_OK;
    int slots = 0;
    for (int i = 0; i < int(sizeof(g_slots) / sizeof(g_slots[0])); ++i) {
        auto** slot = reinterpret_cast<void**>(g_slots[i].imp);
        DWORD old = 0;
        if (IsBadReadPtr(slot, 4) || !*slot || !VirtualProtect(slot, 4, PAGE_READWRITE, &old)) continue;
        g_slots[i].original = *slot;
        *slot = g_detours[i];
        VirtualProtect(slot, 4, old, &old);
        ++slots;
    }
    // (The exe's import of CreateFileA: its C runtime's.)
    bool create_ok = false;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); dir.VirtualAddress && d->Name && !create_ok; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "KERNEL32.dll") != 0) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto** slots = reinterpret_cast<void**>(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
            if (strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name, "CreateFileA") != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&slots[i], 4, PAGE_READWRITE, &old)) {
                o_create = reinterpret_cast<CreateFileAFn>(slots[i]);
                slots[i] = reinterpret_cast<void*>(&create_detour);
                VirtualProtect(&slots[i], 4, old, &old);
                create_ok = true;
            }
            break;
        }
    }
    BWLOG("load: timing the game's file opens (%d, CreateFileA %d) and profile reads (%d of 5); over %.0f ms logged", open_ok, create_ok, slots,
          kSlowMs);
}

void trace(int n) { g_trace = n; }

void report() {
    const long o = g_opens.exchange(0), r = g_reads.exchange(0);
    const long long ot = g_open_ticks.exchange(0), rt = g_read_ticks.exchange(0);
    if (!o && !r) return;
    BWLOG("load: %ld file opens (%.1f ms), %ld profile reads (%.1f ms) (5 s)", o, double(ot) * 1000.0 / double(g_freq.QuadPart), r,
          double(rt) * 1000.0 / double(g_freq.QuadPart));
}

}  // namespace bw::load_trace
