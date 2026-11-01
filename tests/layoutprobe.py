"""侧栏布局探针：分段统计侧栏 (x<232) 与主区 (x>=252) 的暗像素分布。"""
import sys
sys.path.insert(0, r"D:\WorkSpace\maa-hub-cpp\tests")
from pxprobe import decode_png, px

path = sys.argv[1]
w, h, rows = decode_png(path)
print(f"{path}: {w}x{h}")


def dark_in(x0, x1, y0, y1, step=3):
    cnt = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w), step):
            r, g, b = px(rows, w, x, y)
            if r + g + b < 300:
                cnt += 1
    return cnt


print("sidebar (x 0-232) dark pixels per 40px band:")
for y in range(0, h, 40):
    c = dark_in(0, 232, y, y + 40)
    print(f"  y={y:4d}: {'#' * min(50, c // 4)}{c}")

print("main area (x 252-1480) dark pixels per 40px band:")
for y in range(0, h, 40):
    c = dark_in(252, 1480, y, y + 40, step=5)
    print(f"  y={y:4d}: {'#' * min(50, c // 6)}{c}")

# 特色颜色检测：主题蓝 #2E7CF6 = (46,124,246)
print("primary-blue pixel locations (first 20):")
found = 0
for y in range(0, h, 2):
    for x in range(0, w, 2):
        r, g, b = px(rows, w, x, y)
        if abs(r - 46) < 25 and abs(g - 124) < 25 and abs(b - 246) < 25:
            print(f"  ({x},{y})")
            found += 1
            if found >= 20:
                break
    if found >= 20:
        break
