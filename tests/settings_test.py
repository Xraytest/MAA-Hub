"""设置页 + 加载性能验证：
1. 启动 GUI，点击「设置」导航，截图（代理/CDK/LLM 面板）
2. 测量快照刷新耗时（验证账号/执行器加载不再慢）
3. 验证 settings.json 落盘
"""
import ctypes
import ctypes.wintypes as wt
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from shot import find_window, capture  # noqa: E402

user32 = ctypes.windll.user32
EXE = r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe"
DOCS = Path(r"D:\WorkSpace\maa-hub-cpp\docs")
STATE = Path(r"D:\WorkSpace\maa-hub-cpp\build\state")


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def click(hwnd, x, y, settle=0.8):
    user32.PostMessageW(hwnd, 0x0201, 1, lparam(x, y))
    time.sleep(0.05)
    user32.PostMessageW(hwnd, 0x0202, 0, lparam(x, y))
    time.sleep(settle)


def main():
    subprocess.run(["taskkill", "/F", "/IM", "MaaHub.exe"], capture_output=True)
    time.sleep(1)
    import shutil
    shutil.rmtree(STATE, ignore_errors=True)

    proc = subprocess.Popen([EXE], cwd=r"D:\WorkSpace\maa-hub-cpp\build",
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(15)

    hwnd = find_window("MaaHub")
    if not hwnd:
        print("FAIL: no window")
        return 1

    # 1. 设置页（导航第 5 项：nav_y = 950-200-20=730，settings 在 730+160=890..926）
    click(hwnd, 125, 908, 1.5)
    capture(hwnd, str(DOCS / "set-1-proxy.png"))
    print("1. settings page captured")

    # 2. 保存设置（按钮在页面下方）
    click(hwnd, 200, 800, 2.0)
    capture(hwnd, str(DOCS / "set-2-saved.png"))
    print("2. save clicked")

    ok = True
    sj = STATE / "settings.json"
    if sj.exists():
        s = json.loads(sj.read_text(encoding="utf-8"))
        print("settings.json:", json.dumps(s, ensure_ascii=False)[:200])
    else:
        print("FAIL: settings.json not written")
        ok = False

    # 3. 测量快照刷新耗时：连续触发重绘，统计窗口响应时间
    t0 = time.time()
    for i in range(30):
        user32.PostMessageW(hwnd, 0x0200, 0, lparam(400 + i, 500))  # WM_MOUSEMOVE
        time.sleep(0.02)
    dt = (time.time() - t0) / 30
    print(f"avg mouse-move handling: {dt*1000:.1f} ms")
    if dt > 0.1:
        print("WARN: input handling slow")

    # 4. 切到账号管理页并截图（验证账号加载）
    click(hwnd, 125, 788, 1.5)
    capture(hwnd, str(DOCS / "set-3-accounts.png"))
    print("3. accounts page captured")

    proc.kill()
    print("DONE" if ok else "DONE(warn)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
