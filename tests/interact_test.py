"""GUI 交互测试：模拟鼠标/键盘操作验证原生窗口交互。
- 点击导航切换页面并截图
- 在账号管理页输入文本并点击添加
- 在任务页勾选任务
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


def click(hwnd, x, y, settle=0.6):
    user32.PostMessageW(hwnd, 0x0201, 1, lparam(x, y))      # WM_LBUTTONDOWN
    time.sleep(0.05)
    user32.PostMessageW(hwnd, 0x0202, 0, lparam(x, y))      # WM_LBUTTONUP
    time.sleep(settle)


def type_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, 0x0102, ord(ch), 0)       # WM_CHAR
        time.sleep(0.02)


def key(hwnd, vk):
    user32.PostMessageW(hwnd, 0x0100, vk, 0)                # WM_KEYDOWN
    time.sleep(0.05)


def main():
    # 清理旧实例与状态
    subprocess.run(["taskkill", "/F", "/IM", "MaaHub.exe"],
                   capture_output=True)
    time.sleep(1)
    import shutil
    shutil.rmtree(r"D:\WorkSpace\maa-hub-cpp\build\state", ignore_errors=True)

    proc = subprocess.Popen([EXE], cwd=r"D:\WorkSpace\maa-hub-cpp\build",
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(14)  # 等 discover+load

    hwnd = find_window("MaaHub")
    if not hwnd:
        print("FAIL: window not found")
        return 1
    print(f"window hwnd={hwnd:#x}")

    # 1. 总览（已在）
    capture(hwnd, str(DOCS / "it-1-overview.png"))
    print("1. overview captured")

    # 2. 点击"明日方舟"导航（y=654）
    click(hwnd, 116, 654, 1.0)
    capture(hwnd, str(DOCS / "it-2-tasks-maa.png"))
    print("2. maa task page captured")

    # 3. 勾选第一个任务复选框（列表项 y=120..162；复选框 x=264..492, y=133..153）
    click(hwnd, 290, 143, 0.8)
    capture(hwnd, str(DOCS / "it-3-task-checked.png"))
    print("3. checkbox clicked")

    # 4. 点击齿轮展开参数（齿轮 rect x=1416..1446, y=129..153）
    click(hwnd, 1431, 141, 0.8)
    capture(hwnd, str(DOCS / "it-4-task-params.png"))
    print("4. gear clicked")

    # 5. 切换到"账号管理"（y=774）
    click(hwnd, 116, 774, 1.0)
    capture(hwnd, str(DOCS / "it-5-accounts.png"))
    print("5. accounts page captured")

    # 6. 在备注名输入框输入（表单 y = content.top+10 = 80，x=270 起）
    click(hwnd, 300, 93, 0.5)
    type_text(hwnd, "TestAcc01")
    time.sleep(0.5)
    capture(hwnd, str(DOCS / "it-6-form-typing.png"))
    print("6. typed label")

    # 7. 标识输入框（x = 18+210+190 = 418 起，实际从 670 开始）
    click(hwnd, 700, 93, 0.5)
    type_text(hwnd, "1381234")
    time.sleep(0.3)

    # 8. 点击"添加账号"按钮（x = 1190..1300, y=93）
    click(hwnd, 1245, 93, 1.5)
    capture(hwnd, str(DOCS / "it-7-account-added.png"))
    print("7. account add clicked")

    # 9. 切回总览看日常表
    click(hwnd, 116, 614, 1.0)
    capture(hwnd, str(DOCS / "it-8-overview-with-account.png"))
    print("8. overview with account captured")

    # 10. 智能规划页
    click(hwnd, 116, 734, 1.0)
    capture(hwnd, str(DOCS / "it-9-planner.png"))
    print("9. planner page captured")

    # 11. 点击"生成计划"（按钮 x = content.right-160..content.right = 1288-1460, y=70..100）
    click(hwnd, 1370, 85, 8.0)
    capture(hwnd, str(DOCS / "it-10-plan-result.png"))
    print("10. plan proposed")

    # 12. 事件日志页
    click(hwnd, 116, 814, 1.0)
    capture(hwnd, str(DOCS / "it-11-events.png"))
    print("11. events page captured")

    proc.kill()
    print("DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
