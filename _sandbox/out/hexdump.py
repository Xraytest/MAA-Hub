p = r"D:\WorkSpace\maa-hub-cpp\_sandbox\out\probe_buttons.png"
d = open(p, "rb").read()
print("size", len(d))
print(" ".join(f"{b:02x}" for b in d[:80]))
print("ascii:", "".join(chr(b) if 32 <= b < 127 else "." for b in d[:80]))
