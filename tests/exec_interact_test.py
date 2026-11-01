"""执行器架构交互测试：
1. 点击第一个执行器 → 进入 MAA 风格实例页（任务列表）
2. 点击「＋ 新建」→ 填表单 → 创建第二个 MAA 执行器（验证多实例）
3. 打开核心管理页 → 检查更新（验证内置更新检测）
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from shot import find_window, capture  # noqa: E402

user32 = ctypes.windll.user32
EXE = r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe"
DOCS = Path(r"D:\WorkSpace\maa-hub-cpp\docs")


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def click(hwnd, x, y, settle=0.8):
    user32.PostMessageW(hwnd, 0x0201, 1, lparam(x, y))
    time.sleep(0.05)
    user32.PostMessageW(hwnd, 0x0202, 0, lparam(x, y))
    time.sleep(settle)


def type_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, 0x0102, ord(ch), 0)
        time.sleep(0.02)


def main():
    subprocess.run(["taskkill", "/F", "/IM", "MaaHub.exe"], capture_output=True)
    time.sleep(1)
    import shutil
    shutil.rmtree(r"D:\WorkSpace\maa-hub-cpp\build\state", ignore_errors=True)

    proc = subprocess.Popen([EXE], cwd=r"D:\WorkSpace\maa-hub-cpp\build",
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(16)

    hwnd = find_window("MaaHub")
    if not hwnd:
        print("FAIL: no window")
        return 1
    print(f"window {hwnd:#x}")

    # 1. 点击第一个执行器（MaaEnd 默认实例，y≈92）
    click(hwnd, 125, 112, 2.0)
    capture(hwnd, str(DOCS / "ex-1-maaend-instance.png"))
    print("1. maaend instance page")

    # 2. 点击第二个执行器（MAA 默认实例，y≈138）
    click(hwnd, 125, 158, 2.0)
    capture(hwnd, str(DOCS / "ex-2-maa-instance.png"))
    print("2. maa instance page")

    # 3. 点击「＋ 新建」（按钮 rect x=206..238, y=64..86）
    click(hwnd, 222, 75, 0.8)
    capture(hwnd, str(DOCS / "ex-3-new-form.png"))
    print("3. new executor form")

    # 4. 输入实例名 + 创建
    # 表单从 y=188 开始：kind strip 196-220，label 226-250，acc 256-280，按钮 286-312
    click(hwnd, 130, 238, 0.6)          # label 输入框
    type_text(hwnd, "MAA-2")
    time.sleep(0.3)
    click(hwnd, 125, 299, 4.0)          # 创建执行器按钮
    capture(hwnd, str(DOCS / "ex-4-second-exec.png"))
    print("4. second executor created")

    # 5. 核心管理页
    click(hwnd, 125, 810, 1.5)
    capture(hwnd, str(DOCS / "ex-5-cores.png"))
    print("5. cores page")

    # 6. 点击「检查更新」（x≈286, y≈84）
    click(hwnd, 286, 84, 15.0)
    capture(hwnd, str(DOCS / "ex-6-update-check.png"))
    print("6. update check done")

    # 7. 智能规划页
    click(hwnd, 125, 730, 1.5)
    capture(hwnd, str(DOCS / "ex-7-planner.png"))
    print("7. planner page")

    # 8. 生成计划
    click(hwnd, 1370, 85, 8.0)
    capture(hwnd, str(DOCS / "ex-8-plan.png"))
    print("8. plan proposed")

    proc.kill()
    print("DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
