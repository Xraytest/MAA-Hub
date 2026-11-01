"""窗口截图工具：用 PrintWindow 捕获 MaaHub 窗口客户区为 PNG。
用法：python shot.py <output.png> [wait_seconds]
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from pathlib import Path


def find_window(title_part):
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def enum_proc(hwnd, lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if title_part.lower() in buf.value.lower():
            found.append(hwnd)
        return True

    user32.EnumWindows(enum_proc, 0)
    return found[0] if found else None


def capture(hwnd, out_path):
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32

    # 客户区尺寸
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w == 0 or h == 0:
        return False

    hdc_screen = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    bmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, bmp)

    # PW_RENDERFULLCONTENT = 2 (Win 8.1+ 支持 DWM 合成内容)
    PW_RENDERFULLCONTENT = 2
    ok = user32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    if not ok:
        ok = user32.PrintWindow(hwnd, hdc_mem, 0)

    if not ok:
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(hwnd, hdc_screen)
        return False

    # 读位图数据
    bmpinfo = bytearray(40)
    struct.pack_into("<IiiHHIIiiII", bmpinfo, 0,
                     40, w, h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(hdc_mem, bmp, 0, h, buf, (ctypes.c_char * len(bmpinfo)).from_buffer(bmpinfo), 0)
    if got != h:
        print(f"GetDIBits returned {got} expected {h}")

    data = buf.raw  # bytes，索引得到 int

    # CreateCompatibleBitmap 产生 bottom-up DIB：内存第 0 行是图像最底部，
    # 写 PNG 时需要按 (h-1-y) 取行，否则图像上下颠倒。
    rows = []
    stride = w * 4
    for y in range(h):
        src = (h - 1 - y) * stride
        row = bytearray(w * 3 + 1)
        row[0] = 0  # filter: none
        for x in range(w):
            row[1 + x * 3] = data[src + x * 4 + 2]      # R
            row[2 + x * 3] = data[src + x * 4 + 1]      # G
            row[3 + x * 3] = data[src + x * 4]          # B
        rows.append(bytes(row))
    raw = b"".join(rows)

    import zlib
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")

    Path(out_path).write_bytes(png)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_screen)
    return True


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "window.png"
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 12
    time.sleep(wait)
    hwnd = find_window("MaaHub")
    if not hwnd:
        print("WINDOW NOT FOUND")
        sys.exit(1)
    if capture(hwnd, out):
        print(f"SAVED {out}")
    else:
        print("CAPTURE FAILED")
        sys.exit(1)
