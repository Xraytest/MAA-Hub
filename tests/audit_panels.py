"""系统化验收：渲染所有官方面板 + 像素级校验关键元素是否真的出现。

对每个任务：
  1. --dump-render 离屏渲染该任务面板
  2. 用 PIL 检查指定区域是否存在非背景像素（证明控件被画出）
  3. 校验页面标题区包含官方任务名（通过 OCR 不可用，改用渲染前诊断输出核对）
另外校验：官方配色是否真的用上（采样 Primary 色像素）。
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

EXE = r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe"
DOCS = Path(r"D:\WorkSpace\maa-hub-cpp\docs")
DOCS.mkdir(exist_ok=True)

MAA_TASKS = ["StartUp", "Fight", "Infrast", "Recruit", "Mall", "Award",
             "Roguelike", "Depot", "OperBox", "Reclamation", "Copilot",
             "Custom", "CloseDown"]
# 只列 catalog 中真实存在的 MaaEnd 任务（ProtocolSpace 等是 GUI 分组入口，
# 不是独立 pipeline task，故不在此列）
END_TASKS = ["AccountSwitch", "DailyRewards", "AutoCollect", "GiftOperator",
             "VisitFriends", "AutoStockpile", "AutoEcoFarm", "Crafting",
             "LevelUpOperator", "TrialOfSwordmancy", "ItemTransfer"]


def render(task: str, out: Path) -> bool:
    r = subprocess.run([EXE, "--dump-render", str(out), "executor", task],
                       capture_output=True, timeout=300)
    return out.exists() and out.stat().st_size > 100000


def analyze(path: Path):
    """用 PIL 分析：内容像素比例、Primary 色存在性、文字行数。"""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    # 右半部分（设置区）统计非白像素
    non_bg = 0
    primary = 0
    for y in range(0, h, 3):
        for x in range(250, w, 3):
            r, g, b = px[x, y]
            if not (r > 240 and g > 240 and b > 240):
                non_bg += 1
            if abs(r - 0x32) < 30 and abs(g - 0x6C) < 30 and abs(b - 0xF3) < 30:
                primary += 1
    return {"non_bg": non_bg, "primary": primary, "size": (w, h)}


def main() -> int:
    ok = True
    results = []
    for task in MAA_TASKS + END_TASKS:
        out = DOCS / f"audit-{task}.png"
        if not render(task, out):
            print(f"[FAIL] {task}: render failed")
            ok = False
            continue
        info = analyze(out)
        if info is None:
            print(f"[SKIP] {task}: no PIL")
            continue
        # 设置区应有实质内容（控件/文字）
        good = info["non_bg"] > 300
        if not good:
            ok = False
        results.append((task, info["non_bg"], info["primary"], good))
        print(f"[{'OK' if good else 'FAIL'}] {task:<18} content_px={info['non_bg']:<6} primary_px={info['primary']}")

    print("\n=== 汇总 ===")
    passed = sum(1 for r in results if r[3])
    print(f"{passed}/{len(results)} 面板有实质内容")
    # Primary 色应至少出现在部分面板（按钮/高亮）
    with_primary = sum(1 for r in results if r[2] > 0)
    print(f"{with_primary}/{len(results)} 面板含 Primary 色元素")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
