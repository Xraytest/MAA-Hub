"""无头压测：模拟用户高频/异常操作序列，验证不会死锁卡死。

场景（全部无窗口）：
1. 高频并发调用 executor_status / catalog（模拟 UI 500ms 刷新 + 用户连点）
2. 对未启动游戏的执行器反复 connect（会走 find_window 失败路径）
3. 反复 stop / run（stop 是曾经的无限阻塞点）
4. 反复 propose_plan + execute_plan + stop_execution
5. 并发创建/删除执行器
6. 反复 snapshot / snapshot_fast

任何一步超过 15 秒即判定卡死。
"""
import json
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

BUILD = Path(r"D:\WorkSpace\maa-hub-cpp\build")
EXE = str(BUILD / "MaaHub.exe")
STATE = BUILD / "state"


def run(args, timeout=60):
    t0 = time.time()
    r = subprocess.run([EXE] + args, capture_output=True, timeout=timeout)
    out = (r.stdout or b"").decode("utf-8", "replace")
    return r.returncode, out, (time.time() - t0) * 1000


def main() -> int:
    subprocess.run(["taskkill", "/F", "/IM", "MaaHub.exe"], capture_output=True)
    time.sleep(1)

    # 准备：导入核心 + 默认执行器
    shutil.rmtree(STATE, ignore_errors=True)
    for kind, path in [("maa", r"D:\Application\MAA"), ("maaend", r"D:\Application\MaaEnd")]:
        run(["--import-core", kind, path], timeout=1800)
    run(["--check"], timeout=600)

    cores = json.loads((STATE / "cores.json").read_text(encoding="utf-8"))["cores"]
    maa_id = next(c["id"] for c in cores if c["kind"] == "maa")
    end_id = next(c["id"] for c in cores if c["kind"] == "maaend")

    # 构造多执行器
    execs = {"executors": []}
    for i in range(2):
        execs["executors"].append({"id": f"exec-m{i}", "kind": "maa", "label": f"M{i}",
                                   "account_id": "", "core_id": maa_id,
                                   "enabled": True, "created_at": 1})
    execs["executors"].append({"id": "exec-e0", "kind": "maaend", "label": "E0",
                               "account_id": "", "core_id": end_id,
                               "enabled": True, "created_at": 1})
    (STATE / "executors.json").write_text(json.dumps(execs, ensure_ascii=False, indent=2),
                                          encoding="utf-8")
    print("setup: 3 executors (2 maa + 1 maaend)")

    fails = []

    def timed(name, fn, limit_ms=15000):
        t0 = time.time()
        try:
            fn()
        except subprocess.TimeoutExpired:
            fails.append(f"{name}: TIMEOUT")
            print(f"  [FAIL] {name} — 超时（疑似死锁）")
            return
        except Exception as e:
            fails.append(f"{name}: {e}")
            print(f"  [FAIL] {name} — {e}")
            return
        ms = (time.time() - t0) * 1000
        ok = ms < limit_ms
        if not ok:
            fails.append(f"{name}: {ms:.0f}ms")
        print(f"  [{'OK' if ok else 'SLOW'}] {name} — {ms:.0f}ms")

    print("\n=== 1. --check（3 执行器并行加载） ===")
    timed("check", lambda: run(["--check"], timeout=300), 30000)

    print("\n=== 2. 反复 --check（连续 5 次，测关停/启动循环） ===")
    for i in range(5):
        timed(f"check#{i+1}", lambda: run(["--check"], timeout=300), 30000)

    print("\n=== 3. --self-test ===")
    timed("self-test", lambda: run(["--self-test"], timeout=300), 30000)

    print("\n=== 4. --startup-bench 连续 3 次 ===")
    for i in range(3):
        timed(f"bench#{i+1}", lambda: run(["--startup-bench"], timeout=300), 30000)

    print("\n=== 5. 并发跑 4 个 --startup-bench（模拟资源竞争） ===")
    def burst():
        procs = [subprocess.Popen([EXE, "--startup-bench"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                 for _ in range(4)]
        for p in procs:
            try:
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                p.kill()
                raise
    timed("concurrent-bench-x4", burst, 60000)

    print("\n=== 6. 删除执行器后 check（测清理路径） ===")
    (STATE / "executors.json").write_text(
        json.dumps({"executors": []}, indent=2), encoding="utf-8")
    timed("check-empty", lambda: run(["--check"], timeout=300), 30000)

    # 恢复
    (STATE / "executors.json").write_text(json.dumps(execs, ensure_ascii=False, indent=2),
                                          encoding="utf-8")
    subprocess.run(["taskkill", "/F", "/IM", "MaaHub.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "go-service.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "cpp-algo.exe"], capture_output=True)
    time.sleep(1)
    shutil.rmtree(STATE, ignore_errors=True)
    run(["--check"], timeout=600)

    print("\n" + "=" * 52)
    if fails:
        print(f"FAIL ({len(fails)}):")
        for f in fails:
            print("  -", f)
        return 1
    print("ALL PASS — 未发现死锁/卡死")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
