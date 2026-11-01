"""本地验证：多个执行器共享同一份核心（同 core_id），并发加载不冲突。"""
import json
import shutil
import subprocess
from pathlib import Path

BUILD = Path(r"D:\WorkSpace\maa-hub-cpp\build")
STATE = BUILD / "state"
EXE = BUILD / "MaaHub.exe"


def run(args, timeout=300):
    r = subprocess.run([str(EXE)] + args, capture_output=True, timeout=timeout)
    out = (r.stdout or b"").decode("utf-8", "replace")
    err = (r.stderr or b"").decode("utf-8", "replace")
    return r.returncode, out, err


def main() -> int:
    # 干净起点
    shutil.rmtree(STATE, ignore_errors=True)
    shutil.rmtree(BUILD / "cores", ignore_errors=True)

    print("=== 导入两个核心（内部管理） ===")
    for kind, path in [("maa", r"D:\Application\MAA"), ("maaend", r"D:\Application\MaaEnd")]:
        rc, out, _ = run(["--import-core", kind, path], timeout=1800)
        print(out.strip()[-120:])

    cores = json.loads((STATE / "cores.json").read_text(encoding="utf-8"))["cores"]
    maa_core = next(c for c in cores if c["kind"] == "maa")
    print(f"\nMAA 核心: {maa_core['id']} -> {maa_core['path']}")

    # 手工构造 3 个 MAA 执行器 + 2 个 MaaEnd 执行器，全部共享同一核心
    print("\n=== 构造 5 个执行器（3 MAA + 2 MaaEnd，各自共享一份核心） ===")
    execs = {"executors": []}
    for i in range(3):
        execs["executors"].append({
            "id": f"exec-maa-{i}", "kind": "maa", "label": f"官服账号{i+1}",
            "account_id": "", "core_id": maa_core["id"], "enabled": True,
            "created_at": 1793515000 + i,
        })
    end_core = next(c for c in cores if c["kind"] == "maaend")
    for i in range(2):
        execs["executors"].append({
            "id": f"exec-end-{i}", "kind": "maaend", "label": f"终末地账号{i+1}",
            "account_id": "", "core_id": end_core["id"], "enabled": True,
            "created_at": 1793515100 + i,
        })
    (STATE).mkdir(parents=True, exist_ok=True)
    (STATE / "executors.json").write_text(
        json.dumps(execs, ensure_ascii=False, indent=2), encoding="utf-8")

    print("\n=== --check：5 个执行器并发加载 ===")
    rc, out, _ = run(["--check"], timeout=600)
    print(out)
    ok = "ALL OK" in out and "执行器数量: 5" in out

    print("\n=== cores 目录仍只有 2 份（未重复拉取） ===")
    core_dirs = sorted(p.name for p in (BUILD / "cores").iterdir() if p.is_dir())
    print(core_dirs)
    if len(core_dirs) != 2:
        print("FAIL: 核心份数不是 2")
        ok = False

    print("\n=== --self-test ===")
    rc2, out2, _ = run(["--self-test"], timeout=600)
    tail = [l for l in out2.splitlines() if l.strip()]
    print("\n".join(tail[-6:]))
    if "ALL OK" not in out2:
        ok = False

    print("\n" + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
