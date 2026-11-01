import struct, sys, zlib
p = r"D:\WorkSpace\maa-hub-cpp\_sandbox\out\probe_buttons.png"
d = open(p, "rb").read()
print("size", len(d))
print("magic", d[:8])
assert d[:8] == b"\x89PNG\r\n\x1a\n", "bad magic"
pos = 8
chunks = []
while pos < len(d):
    n = struct.unpack(">I", d[pos:pos+4])[0]
    tag = d[pos+4:pos+8]
    body = d[pos+8:pos+8+n]
    crc = struct.unpack(">I", d[pos+8+n:pos+12+n])[0]
    calc = zlib.crc32(tag + body) & 0xFFFFFFFF
    chunks.append((tag.decode(), n, "CRC-OK" if crc == calc else f"CRC-BAD({crc}!={calc})"))
    pos += 12 + n
for c in chunks:
    print("  chunk", c)
w, h, bd, ct = struct.unpack(">IIBB", d[16:26])
print("IHDR", w, "x", h, "bitdepth", bd, "colortype", ct)
idat = b"".join(d[pos+8:pos+8+struct.unpack('>I', d[pos:pos+4])[0]]
                for pos in [8] ) # placeholder
# 正确取 IDAT
pos = 8
idat = b""
while pos < len(d):
    n = struct.unpack(">I", d[pos:pos+4])[0]
    tag = d[pos+4:pos+8]
    if tag == b"IDAT":
        idat += d[pos+8:pos+8+n]
    pos += 12 + n
raw = zlib.decompress(idat)
print("decompressed", len(raw), "expected", h * (w*3+1))
print("PNG VALID" if len(raw) == h * (w*3+1) else "PNG INVALID")
