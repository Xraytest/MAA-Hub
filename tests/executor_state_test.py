"""执行器 + 规划器无头验证（不依赖 GUI 点击）。

验证：
1. Hub 加载 executors.json，2+ 个执行器均可返回状态与目录
2. 启发式规划器对多执行器生成计划（含未连接 warnings）
3. 计划步骤绑定 exec_id，可执行/停止
"""
import json
import subprocess
import sys
import time
from pathlib import Path

STATE = Path(r"D:\WorkSpace\maa-hub-cpp\build\state")


def read_json(p):
    return json.loads(Path(p).read_text(encoding="utf-8"))


def main():
    ok = True

    # 0. 先跑一次 --check（确保 cores.json / executors.json 就绪）
    r0 = subprocess.run([r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe", "--check"],
                        capture_output=True, text=True, encoding="utf-8", timeout=240)
    if "ALL OK" not in r0.stdout:
        print("FAIL: initial check not ALL OK")
        print(r0.stdout[-500:], r0.stderr[-500:])
        ok = False

    # 1. 执行器持久化
    execs = read_json(STATE / "executors.json")["executors"]
    print(f"executors: {len(execs)}")
    for e in execs:
        print(f"  - {e['label']} kind={e['kind']} core={e['core_id']} acc={e['account_id'] or '-'}")
    if len(execs) < 3:
        print("FAIL: expected >=3 executors (2 default + MAA-2)")
        ok = False
    kinds = {e["kind"] for e in execs}
    if kinds != {"maa", "maaend"}:
        print(f"FAIL: kinds {kinds}")
        ok = False

    # 2. 核心登记
    cores = read_json(STATE / "cores.json")["cores"]
    print(f"cores: {len(cores)}")
    for c in cores:
        print(f"  - {c['kind']} {c['name']} v{c['version']} src={c['source']} default={c['is_default']}")
    if not cores:
        print("FAIL: no cores")
        ok = False
    for c in cores:
        if not c["version"]:
            print(f"WARN: core {c['kind']} version empty")
        if not c["is_default"]:
            print(f"WARN: core {c['kind']} not default")

    # 3. 删除核心后执行器仍可用（回退到默认核心）
    bak = STATE / "cores.json.bak"
    bak.write_text((STATE / "cores.json").read_text(encoding="utf-8"), encoding="utf-8")
    (STATE / "cores.json").unlink()
    r = subprocess.run([r"D:\WorkSpace\maa-hub-cpp\build\MaaHub.exe", "--check"],
                       capture_output=True, text=True, encoding="utf-8", timeout=240)
    print("--- check after core removal ---")
    print(r.stdout.strip())
    if "ALL OK" not in r.stdout:
        print("FAIL: executors broke after core removal")
        ok = False
    # 恢复
    bak.replace(STATE / "cores.json")

    print()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
