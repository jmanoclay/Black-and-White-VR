#pragma once
#include <windows.h>

#include <string>

namespace bw::paths {

void set_module(HINSTANCE inst);
void init();

const std::wstring& game_dir();     // folder containing runblack.exe (with trailing backslash)
const std::wstring& bwvr_dir();     // <game>\bwvr\  : config, logs, captures, control files
const std::wstring& ini_file();     // <game>\bwvr\bwvr.ini
const std::wstring& real_ddraw();   // system ddraw.dll, or [proxy] chain= override
const std::wstring& system_ddraw(); // the system's ddraw.dll

int ini_int(const wchar_t* section, const wchar_t* key, int fallback);

}  // namespace bw::paths
