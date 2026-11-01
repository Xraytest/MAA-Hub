"""最终全面审计（自带状态恢复，避免 state 被清理后渲染出欢迎页）。"""
import subprocess
import sys
from pathlib import Path

EXE = r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe"
BUILD = Path(r"D:\WorkSpace\maa-hub-cpp\build")
DOCS = Path(r"D:\WorkSpace\maa-hub-cpp\docs")
DOCS.mkdir(exist_ok=True)


def check_ready() -> bool:
    r = subprocess.run([EXE, "--check"], capture_output=True, timeout=600,
                       encoding="utf-8", errors="replace")
    return "ALL OK" in (r.stdout or "")


def ensure_cores() -> None:
    if check_ready():
        return
    print("[setup] cores 未就绪，重新导入…")
    for kind, src in (("maa", r"D:\Application\MAA"),
                      ("maaend", r"D:\Application\MaaEnd")):
        subprocess.run([EXE, "--import-core", kind, src],
                       capture_output=True, timeout=1800)
    if not check_ready():
        print("[setup] 核心导入后仍不可用")
        raise SystemExit(2)


PAGES = ["welcome", "accounts", "cores", "planner", "events", "settings"]
MAA = ["StartUp", "CloseDown", "Fight", "Infrast", "Recruit", "Mall", "Award",
       "Roguelike", "Depot", "OperBox", "Reclamation", "Copilot", "Custom"]
END = ["AccountSwitch", "DailyRewards", "AutoCollect", "GiftOperator",
       "VisitFriends", "AutoStockpile", "RealTimeTask", "ItemTransfer"]

ensure_cores()
ok = fail = 0
for kind, names in (("page", PAGES), ("task", MAA), ("task", END)):
    for n in names:
        out = DOCS / f"final-{kind}-{n}.png"
        subprocess.run([EXE, "--dump-render", str(out),
                        kind if kind == "page" else "executor", n],
                       capture_output=True, timeout=300)
        if out.exists() and out.stat().st_size > 100000:
            ok += 1
        else:
            fail += 1
            print(f"[FAIL] {kind}/{n}")
print(f"=== {ok} ok / {fail} fail ===")
raise SystemExit(1 if fail else 0)
