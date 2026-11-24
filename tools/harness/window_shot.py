"""Grab what the game's window actually shows on the desktop (not the mod's render target).

Usage: python tools/harness/window_shot.py <out.png>
"""
import ctypes
import sys
from ctypes import wintypes

from PIL import ImageGrab

user32 = ctypes.WinDLL("user32", use_last_error=True)
psapi = ctypes.WinDLL("psapi")
kernel32 = ctypes.WinDLL("kernel32")


def game_window():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            name = ctypes.create_unicode_buffer(260)
            size = wintypes.DWORD(260)
            if kernel32.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(size)) and name.value.lower().endswith("runblack.exe"):
                r = wintypes.RECT()
                user32.GetClientRect(hwnd, ctypes.byref(r))
                if r.right - r.left > 200:
                    found.append(hwnd)
            kernel32.CloseHandle(h)
        return True

    user32.EnumWindows(each, 0)
    return found[0] if found else None


def main():
    hwnd = game_window()
    if not hwnd:
        print("no game window")
        return 1
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    user32.SetForegroundWindow(hwnd)
    img = ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + r.right, pt.y + r.bottom), all_screens=True)
    img.save(sys.argv[1] if len(sys.argv) > 1 else "window.png")
    print(sys.argv[1] if len(sys.argv) > 1 else "window.png", img.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
