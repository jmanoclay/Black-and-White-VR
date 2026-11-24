// Forwards every export of the system ddraw.dll. Each stub makes sure the real DLL is loaded
// (and the mod initialized) on first use, then jumps to the real function with all registers
// and the stack untouched, so calling conventions don't matter.
#include <windows.h>

#include "core/log.h"
#include "core/mod.h"
#include "core/paths.h"
#include "render/mipmaps.h"

#define BWVR_DDRAW_EXPORTS(X)                                                                      \
    X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand)                \
    X(DDGetAttachedSurfaceLcl) X(DDInternalLock) X(DDInternalUnlock) X(DSoundHelp)                 \
    X(DirectDrawCreate) X(DirectDrawCreateClipper) X(DirectDrawCreateEx) X(DirectDrawEnumerateA)   \
    X(DirectDrawEnumerateExA) X(DirectDrawEnumerateExW) X(DirectDrawEnumerateW) X(DllCanUnloadNow) \
    X(DllGetClassObject) X(GetDDSurfaceLocal) X(GetOLEThunkData) X(GetSurfaceFromDC)               \
    X(RegisterSpecialCase) X(ReleaseDDThreadLock) X(SetAppCompatData)

extern "C" {
#define DECLARE_REAL(name) FARPROC real_##name = nullptr;
BWVR_DDRAW_EXPORTS(DECLARE_REAL)
#undef DECLARE_REAL
}

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID*) {
    std::wstring from = bw::paths::real_ddraw();
    HMODULE real = LoadLibraryW(from.c_str());
    if (!real && from != bw::paths::system_ddraw()) {
        // (a [proxy] chain that isn't there, e.g. dxwrapper not installed: the system's instead)
        BWLOG("could not load %ls (error %lu): using the system's ddraw.dll", from.c_str(), GetLastError());
        from = bw::paths::system_ddraw();
        real = LoadLibraryW(from.c_str());
    }
    if (!real) {
        BWLOG("FATAL: could not load real ddraw.dll from %ls (error %lu)", from.c_str(), GetLastError());
        return TRUE;
    }
#define RESOLVE(name) real_##name = GetProcAddress(real, #name);
    BWVR_DDRAW_EXPORTS(RESOLVE)
#undef RESOLVE
    BWLOG("real ddraw loaded from %ls", from.c_str());
    bw::mipmaps::on_ddraw_loaded(real_DirectDrawCreate, real_DirectDrawCreateEx);
    return TRUE;
}

extern "C" void __cdecl bwvr_ensure_loaded() {
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
}

// pushad/pushfd preserve everything the caller set up; the jmp keeps the original return address.
#define DEFINE_STUB(name)                                                                          \
    extern "C" __declspec(naked) void name##_stub() {                                              \
        __asm pushad __asm pushfd __asm call bwvr_ensure_loaded __asm popfd __asm popad            \
        __asm jmp dword ptr [real_##name]                                                          \
    }
BWVR_DDRAW_EXPORTS(DEFINE_STUB)
#undef DEFINE_STUB

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        bw::paths::set_module(inst);
        // We are a static import of runblack.exe, so this runs before any game code: the hooks
        // must be in place before the game's startup checks. Loading the real ddraw.dll is
        // deferred to the first export call (LoadLibrary under the loader lock is unsafe).
        bw::paths::init();
        bw::log::init();
        bw::mod::init();
    } else if (reason == DLL_PROCESS_DETACH) {
        bw::log::shutdown();
    }
    return TRUE;
}
