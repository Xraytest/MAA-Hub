"""启动/更新设置持久化测试（--settings-dump 往返验证）。

用法: python tests/settings_persist_test.py
原理: 写入带特定值的 settings.json -> MaaHub --settings-dump 读出规范化 JSON
      -> 比对 startup_update 字段是否与写入值一致。
注意: update_force_github 已于 652b873 废弃（读入静默忽略，不再回写），
      本测试曾对其断言导致常红（r28 修复）；r28 新增 update_auto_maa/maaend。
      r28 测试卫生：夹具写入前备份真实 settings.json，结束后（含异常）原样恢复，
      且所有用例跑完不因首个失败短路。
"""
import json
import subprocess
import sys
from pathlib import Path

BUILD = Path(r"D:\WorkSpace\maa-hub-cpp\build")
STATE = BUILD / "state" / "settings.json"
EXE = str(BUILD / "MaaHub.exe")

CASES = [
    {"auto_start": True, "start_minimized": True, "start_run": False,
     "update_check_on_start": False, "update_auto_download": True,
     "update_auto_install": False,
     "update_auto_maa": False, "update_auto_maaend": True},
    {"auto_start": False, "start_minimized": False, "start_run": True,
     "update_check_on_start": True, "update_auto_download": False,
     "update_auto_install": True,
     "update_auto_maa": True, "update_auto_maaend": False},
]


def run_case(i, su):
    data = {
        "proxy": {"mode": "none", "host": "127.0.0.1", "port": 7890,
                  "user": "", "pass": ""},
        "mirrorchyan_cdk": "",
        "llm": {"enabled": False, "base_url": "https://api.deepseek.com",
                "api_key": "", "model": "deepseek-chat"},
        "startup_update": su,
    }
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    r = subprocess.run([EXE, "--settings-dump"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=900, cwd=str(BUILD))
    if r.returncode != 0:
        print(f"  用例 {i}: FAIL（退出码 {r.returncode}）")
        return False
    try:
        got = json.loads(r.stdout).get("startup_update", {})
    except Exception as e:
        print(f"  用例 {i}: FAIL（JSON 解析失败: {e}）")
        return False

    ok = all(got.get(k) == v for k, v in su.items())
    print(f"  用例 {i}: {'PASS' if ok else 'FAIL'}")
    if not ok:
        for k, v in su.items():
            g = got.get(k)
            if g != v:
                print(f"    {k}: 期望 {v} 实际 {g}")
    return ok


print("=== 启动/更新设置持久化测试 ===")
backup = STATE.read_bytes() if STATE.exists() else None
try:
    results = [run_case(i, su) for i, su in enumerate(CASES)]   # 全部跑完，不短路
    all_ok = all(results)
finally:
    # 恢复真实设置：夹具绝不留在盘上（r28 测试卫生）
    if backup is not None:
        STATE.write_bytes(backup)
    elif STATE.exists():
        STATE.unlink()
print("\n" + ("ALL PASS" if all_ok else "FAILED"))
sys.exit(0 if all_ok else 1)