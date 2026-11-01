"""验证 .vscode 配置 JSON 语法有效性。"""
import json
import sys
from pathlib import Path

VSCODE = Path(r"D:\WorkSpace\maa-hub-cpp\.vscode")
ok = True
for f in ["tasks.json", "launch.json", "c_cpp_properties.json"]:
    p = VSCODE / f
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
        n = len(d.get("tasks") or d.get("configurations") or [])
        print(f"[OK] {f} ({n} entries)")
    except Exception as e:
        print(f"[FAIL] {f}: {e}")
        ok = False

# 关键字段检查
tasks = json.loads((VSCODE / "tasks.json").read_text(encoding="utf-8"))["tasks"]
labels = [t["label"] for t in tasks]
need = ["MaaHub: Build (Release)", "MaaHub: Build (Debug)", "MaaHub: Self-Check (no game)", "MaaHub: Run"]
for n in need:
    if n in labels:
        print(f"[OK] task present: {n}")
    else:
        print(f"[FAIL] task missing: {n}")
        ok = False
# problem matcher
build = next(t for t in tasks if t["label"] == "MaaHub: Build (Release)")
if build.get("problemMatcher") == ["$msCompile"]:
    print("[OK] $msCompile problem matcher")
else:
    print("[FAIL] problem matcher:", build.get("problemMatcher"))
    ok = False

launch = json.loads((VSCODE / "launch.json").read_text(encoding="utf-8"))["configurations"]
for c in launch:
    exe = c.get("program", "")
    if "MaaHub.exe" in exe:
        print(f"[OK] launch: {c['name']}")
    else:
        print(f"[FAIL] launch program: {exe}")
        ok = False

sys.exit(0 if ok else 1)
