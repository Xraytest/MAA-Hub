r"""无头测试套件（不创建任何可见窗口）。

用法：python tests\headless_tests.py
包含：
  1. --startup-bench  启动到执行器可见 < 200ms
  2. --check          全部执行器加载核心
  3. --self-test      设置/执行器/账号/目录缓存/快照性能/规划
  4. 核心内部管理      import -> cores 目录 -> 删除后回退
  5. 多执行器共享核心  3 MAA + 2 MaaEnd 共享 2 份核心并发加载
"""
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

BUILD = Path(r"D:\WorkSpace\maa-hub-cpp\build")
# 运行状态目录跟着 exe 走（exe_dir\state、exe_dir\cores），所以把 exe 复制进沙箱跑，
# 否则这套测试会把开发机上正在用的 state/cores 连根删掉。
SANDBOX = BUILD / "_headless"
EXE = str(SANDBOX / "MaaHub.exe")
STATE = SANDBOX / "state"
CORES = SANDBOX / "cores"

results = []


def run(args, timeout=900):
    t0 = time.time()
    r = subprocess.run([EXE] + args, capture_output=True, timeout=timeout)
    out = (r.stdout or b"").decode("utf-8", "replace")
    err = (r.stderr or b"").decode("utf-8", "replace")
    return r.returncode, out, err, (time.time() - t0) * 1000


def check(name, ok, extra=""):
    results.append((name, ok))
    print(f"  [{'OK' if ok else 'FAIL'}] {name}" + (f" — {extra}" if extra else ""))


def kill_stale():
    # 只杀本沙箱里起的进程：按映像名 taskkill 会连带杀掉开发者正在用的正式实例，
    # 而 worker 与核心附属 exe 都落在 SANDBOX 下，路径过滤足够覆盖。
    ps = ("Get-Process MaaHub,go-service,cpp-algo -ErrorAction SilentlyContinue"
          "| Where-Object { $_.Path -like '%s*' } | ForEach-Object { Stop-Process -Id $_.Id -Force }"
          % str(SANDBOX))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    time.sleep(0.8)


def reset_state():
    kill_stale()
    shutil.rmtree(SANDBOX, ignore_errors=True)
    SANDBOX.mkdir(parents=True)
    shutil.copy2(BUILD / "MaaHub.exe", EXE)
    src_cfg = BUILD / "config.json"
    if src_cfg.exists():
        shutil.copy2(src_cfg, SANDBOX / "config.json")


def main() -> int:
    print("=== 1. 启动性能（无头） ===")
    # 先准备核心与执行器
    reset_state()
    for kind, path in [("maa", r"D:\Application\MAA"), ("maaend", r"D:\Application\MaaEnd")]:
        run(["--import-core", kind, path], timeout=1800)
    run(["--check"], timeout=600)   # 创建默认执行器

    rc, out, _, _ = run(["--startup-bench"], timeout=600)
    line = next((l for l in out.splitlines() if l.startswith("[startup]")), "")
    print(" ", line)
    fp = 99999
    if "first_paint=" in line:
        fp = float(line.split("first_paint=")[1].split("ms")[0])
    check("启动到执行器可见 < 200ms", fp < 200, f"{fp:.0f}ms")
    check("启动基准 PASS", "BENCH PASS" in out)

    print("\n=== 2. --check 全部执行器加载核心 ===")
    rc, out, _, _ = run(["--check"], timeout=600)
    check("ALL OK", "ALL OK" in out, out.strip().splitlines()[-1] if out.strip() else "")
    n = 0
    for l in out.splitlines():
        if l.startswith("执行器数量"):
            n = int(l.split(":")[1].strip())
    check("执行器已创建", n >= 2, f"{n} 个")

    print("\n=== 3. --self-test ===")
    rc, out, _, _ = run(["--self-test"], timeout=600)
    fails = [l for l in out.splitlines() if "[FAIL]" in l]
    check("self-test 全部通过", "ALL OK" in out and not fails,
          "; ".join(f.strip() for f in fails[:3]))

    print("\n=== 4. 核心内部管理 ===")
    core_dirs = sorted(p.name for p in CORES.iterdir() if p.is_dir()) if CORES.exists() else []
    check("核心在内部 cores 目录", len(core_dirs) == 2, str(core_dirs))
    cores = json.loads((STATE / "cores.json").read_text(encoding="utf-8"))["cores"]
    check("cores.json 路径均在内部",
          all(str(CORES) in c["path"] or "cores" in c["path"] for c in cores))

    print("\n=== 5. 多执行器共享核心 ===")
    # 构造 3 MAA + 2 MaaEnd，共享同一核心
    maa = next(c for c in cores if c["kind"] == "maa")
    end = next(c for c in cores if c["kind"] == "maaend")
    execs = {"executors": []}
    for i in range(3):
        execs["executors"].append({"id": f"exec-m{i}", "kind": "maa",
                                   "label": f"官服{i+1}", "account_id": "",
                                   "core_id": maa["id"], "enabled": True, "created_at": 1})
    for i in range(2):
        execs["executors"].append({"id": f"exec-e{i}", "kind": "maaend",
                                   "label": f"末末{i+1}", "account_id": "",
                                   "core_id": end["id"], "enabled": True, "created_at": 1})
    (STATE / "executors.json").write_text(json.dumps(execs, ensure_ascii=False, indent=2),
                                          encoding="utf-8")
    rc, out, _, ms = run(["--check"], timeout=600)
    check("5 执行器并发加载 ALL OK", "ALL OK" in out and "执行器数量: 5" in out,
          f"{ms:.0f}ms")
    core_dirs = sorted(p.name for p in CORES.iterdir() if p.is_dir())
    check("磁盘仍只有 2 份核心", len(core_dirs) == 2, str(core_dirs))

    # 清理：整个沙箱一次性删除，正式 build\state 从头到尾没被碰过
    kill_stale()
    shutil.rmtree(SANDBOX, ignore_errors=True)

    print("\n" + "=" * 50)
    passed = sum(1 for _, ok in results if ok)
    print(f"{passed}/{len(results)} 通过")
    for name, ok in results:
        if not ok:
            print(f"  FAIL: {name}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
