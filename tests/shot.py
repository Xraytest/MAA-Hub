"""Win32 窗口截图：按标题（子串）或进程名定位窗口，PrintWindow 抓客户区存 PNG。

用法：
  python tests/shot.py --title MAA --out docs/ref-maa.png
  python tests/shot.py --exe MaaHub.exe --out docs/cur.png --client-only
"""
import argparse
import subprocess
import sys
import time
from ctypes import wintypes

import ctypes
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# 物理像素坐标：否则跨屏/缩放机器上 GetWindowRect 与 BitBlt 会对不上
try:
    ctypes.windll.shcore.SetProcessDpiAwarenessContext(-4)  # PER_MONITOR_AWARE_V2
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

user32.GetWindowDC.restype = wintypes.HDC
user32.GetWindowDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                            ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]

PW_RENDERFULLCONTENT = 0x00000002


def enum_windows():
    out = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lparam):
        out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out


def win_title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def win_pid(hwnd):
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def pick(title_sub, exe_name):
    pids = set()
    if exe_name:
        q = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe_name}", "/FO", "CSV", "/NH"],
                           capture_output=True, text=True)
        for line in q.stdout.splitlines():
            parts = line.split(",")
            if len(parts) >= 2 and exe_name.lower() in parts[0].strip('"').lower():
                pids.add(int(parts[4].strip('"')))
    cands = []
    for hwnd in enum_windows():
        if not user32.IsWindowVisible(hwnd):
            continue
        if pids and win_pid(hwnd) not in pids:
            continue
        t = win_title(hwnd)
        if title_sub and title_sub.lower() not in t.lower():
            continue
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if (r.right - r.left) < 200 or (r.bottom - r.top) < 150:
            continue
        cands.append((hwnd, t, win_pid(hwnd)))
    return cands


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


def capture(hwnd, out_path, client_only=True):
    if client_only:
        r = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
    else:
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return False
    hwnd_dc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hwnd_dc)
    bmp = gdi32.CreateCompatibleBitmap(hwnd_dc, w, h)
    gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    gdi32.DeleteDC(mem)
    gdi32.DeleteObject(bmp)
    user32.ReleaseDC(hwnd, hwnd_dc)
    if not got:
        return False
    im = Image.frombytes("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")
    im.save(out_path)
    print(f"saved {out_path} {w}x{h} printwindow={ok}")
    return True


def capture_screen(hwnd, out_path):
    """WPF/DirectX 窗口 PrintWindow 会拿到空白，改为置前后从屏幕 BitBlt。"""
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # NOSIZE|NOMOVE|NOZORDER
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return False
    sdc = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, sdc, r.left, r.top, 0x00CC0020 | 0x00100000)  # SRCCOPY|CAPTUREBLT
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    gdi32.DeleteDC(mem)
    gdi32.DeleteObject(bmp)
    user32.ReleaseDC(None, sdc)
    if not got:
        return False
    im = Image.frombytes("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")
    im.save(out_path)
    print(f"saved {out_path} {w}x{h} at ({r.left},{r.top})")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", default="")
    ap.add_argument("--exe", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--whole", action="store_true", help="capture the full window, not only the client area")
    ap.add_argument("--index", type=int, default=0)
    ap.add_argument("--screen", action="store_true", help="bring to front and BitBlt from screen (for WPF/DirectX windows)")
    ap.add_argument("--hwnd", default="", help="capture this exact hwnd (hex), skipping the filter")
    ap.add_argument("--wait", type=float, default=0.0)
    a = ap.parse_args()
    if a.wait:
        time.sleep(a.wait)
    if a.hwnd:
        hwnd = int(a.hwnd, 16)
        print(f"capturing hwnd={a.hwnd} title=[{win_title(hwnd)}]")
        return 0 if (capture_screen(hwnd, a.out) if a.screen
                     else capture(hwnd, a.out, client_only=not a.whole)) else 1
    cands = pick(a.title, a.exe)
    if not cands:
        print("no matching window")
        for hwnd in enum_windows():
            if user32.IsWindowVisible(hwnd):
                t = win_title(hwnd)
                if t:
                    print(f"  visible: [{t}] pid={win_pid(hwnd)}")
        return 1
    for i, (hwnd, t, pid) in enumerate(cands):
        print(f"  cand[{i}] hwnd={hwnd:#x} pid={pid} title=[{t}]")
    hwnd, t, pid = cands[min(a.index, len(cands) - 1)]
    print(f"capturing [{t}] pid={pid}")
    if a.screen:
        return 0 if capture_screen(hwnd, a.out) else 1
    return 0 if capture(hwnd, a.out, client_only=not a.whole) else 1


if __name__ == "__main__":
    raise SystemExit(main())
