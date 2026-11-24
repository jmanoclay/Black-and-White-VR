#include "core/exe_check.h"

#include <windows.h>

#include <cstring>

#include "core/log.h"
#include "game/bw_symbols.h"

namespace bw::exe {

static bool readable(uintptr_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return addr + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool matches_v120() {
    struct Anchor {
        uintptr_t addr;
        const char* name;
    };
    const Anchor anchors[] = {
        {sym::RTTI_GCamera, ".?AVGCamera@@"},
        {sym::RTTI_CHand, ".?AVCHand@@"},
        {sym::RTTI_GGame, ".?AVGGame@@"},
    };
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x00400000) {
        BWLOG("exe check: unexpected image base %p", GetModuleHandleW(nullptr));
        return false;
    }
    for (const Anchor& a : anchors) {
        const size_t len = strlen(a.name) + 1;
        const char* p = reinterpret_cast<const char*>(a.addr + 8);  // TypeDescriptor::name
        if (!readable(a.addr + 8, len) || memcmp(p, a.name, len) != 0) {
            BWLOG("exe check: RTTI anchor %s not found at %08X", a.name, unsigned(a.addr));
            return false;
        }
    }
    BWLOG("exe check: runblack.exe matches v1.20 layout");
    return true;
}

}  // namespace bw::exe
