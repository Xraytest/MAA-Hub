"""PNG 像素探针：解码 PNG 并采样若干坐标的 RGB，用于验证窗口截图内容。"""
import struct
import sys
import zlib


def decode_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    width = height = bitdepth = colortype = None
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", chunk[:10])
        elif tag == b"IDAT":
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(idat)
    assert colortype == 2 and bitdepth == 8, (colortype, bitdepth)
    stride = width * 3
    rows = []
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:  # Sub
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif f == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:  # Average
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, rows


def px(rows, w, x, y):
    o = x * 3
    return rows[y][o], rows[y][o + 1], rows[y][o + 2]


if __name__ == "__main__":
    path = sys.argv[1]
    w, h, rows = decode_png(path)
    print(f"{path}: {w}x{h}")
    probes = [
        (700, 10, "topbar area"),
        (30, 10, "sidebar top (logo zone)"),
        (30, 700, "sidebar mid-low"),
        (700, 100, "content top"),
        (700, 500, "content mid (cards)"),
        (740, 30, "topbar right (buttons)"),
    ]
    for x, y, label in probes:
        r, g, b = px(rows, w, x, y)
        print(f"  ({x:4d},{y:4d}) #{r:02X}{g:02X}{b:02X}  {label}")
    # 统计深色像素（文字）在每 100 行的数量，判断布局
    print("  dark-pixel rows (text presence):")
    for band in range(0, h, 100):
        cnt = 0
        for y in range(band, min(band + 100, h)):
            for x in range(0, w, 7):
                r, g, b = px(rows, w, x, y)
                if r + g + b < 300:
                    cnt += 1
        bar = "#" * min(60, cnt // 8)
        print(f"    y={band:4d}-{min(band+100,h):4d}: {cnt:5d} {bar}")
