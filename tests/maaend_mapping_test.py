r"""终末地（MaaEnd）侧的无头映射测试 —— 不需要游戏客户端。

覆盖的是 MaaHub 自己的代码路径（MaaEndAdapter::scan_tasks / task_option_specs 建的
catalog 与 routes），对应历史缺陷：
  #33 展示名没换成 entry 节点，post 到不存在的节点上秒失败
  #34 option 没按 pipeline_type 出类型，整份 override parse 失败
  #35 空输入把默认值覆盖掉

同时把 catalog 的 params 与核心 interface.json + 全部 import 里的 option 定义对账。
新发现的建模缺陷单独列在「已知缺陷」区（默认不影响退出码，--strict 才计入失败）。

用法：python tests/maaend_mapping_test.py [--strict]
"""
import json
import os
import subprocess
import sys
from collections import Counter

BS = chr(92)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
EXE = os.path.join(BUILD, "MaaHub.exe")
CORE = os.path.join(BUILD, "cores", "maaend-v5.15.0-beta.1")

results = []
known = []


def check(name, ok, extra=""):
    results.append((name, bool(ok)))
    print(f"  [{'OK' if ok else 'FAIL'}] {name}" + (f" — {extra}" if extra else ""))


def defect(name, hits, extra=""):
    known.append((name, len(hits), hits))
    print(f"  [DEFECT] {name} — {len(hits)} 处" + (f"：{extra}" if extra else ""))


def strip_json_comments(text):
    """核心任务文件是 JSONC（带 // 与 /* */ 注释）：C++ 侧 mj::try_parse 会跳注释，
    Python 的 json 不会。对账要用同一口径，否则 7 份文件读不到、任务数对不上。"""
    out = []
    i, n = 0, len(text)
    in_str = esc = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == BS:
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != chr(10) and text[i] != chr(13):
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def load_jsonc(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.loads(strip_json_comments(f.read()))


def maaend_exec_id():
    out = subprocess.run([EXE, "--cli", "devices"], capture_output=True, timeout=300)
    for line in out.stdout.decode("utf-8", "replace").splitlines():
        if "kind=maaend" in line:
            return line.split("exec=")[1].split()[0]
    return None


def core_docs():
    old = os.getcwd()
    os.chdir(CORE)
    try:
        iface = load_jsonc("interface.json")
        docs = [("interface.json", iface)]
        for rel in iface.get("import", []):
            p = rel.replace("/", BS)
            if os.path.exists(p):
                docs.append((rel, load_jsonc(p)))
        return docs
    finally:
        os.chdir(old)


def pipeline_nodes():
    nodes = set()
    for rel in ("resource/pipeline/nodes.json", "resource/default_pipeline.json"):
        p = os.path.join(CORE, rel.replace("/", BS))
        if not os.path.exists(p):
            continue
        data = load_jsonc(p)
        sections = data.values() if rel.endswith("default_pipeline.json") else [data]
        for section in sections:
            if isinstance(section, dict):
                nodes.update(section.keys())
    return nodes


def main():
    strict = "--strict" in sys.argv
    if not os.path.exists(EXE):
        print(f"缺少 {EXE}，先跑 build.bat release")
        return 2
    if not os.path.exists(CORE):
        print(f"缺少 MaaEnd 核心 {CORE}")
        return 2

    exec_id = maaend_exec_id()
    check("找到 maaend 执行器", exec_id is not None, exec_id or "")
    if not exec_id:
        return 1

    proc = subprocess.run([EXE, "--cli", "tasks", exec_id, "--json"],
                          capture_output=True, timeout=600)
    catalog = json.loads(proc.stdout.decode("utf-8", "replace"))

    docs = core_docs()
    imports = len(docs[0][1].get("import", []))
    check("核心 import 全部可读（JSONC）", len(docs) == imports + 1,
          f"{len(docs)}/{imports + 1} 份文档")

    task_names = {t.get("name") for _, d in docs for t in (d.get("task") or [])
                  if isinstance(t, dict) and t.get("name")}
    check("catalog 覆盖核心全部任务", len(catalog) == len(task_names),
          f"catalog {len(catalog)} / 核心 {len(task_names)}")
    missing = sorted(task_names - {t["name"] for t in catalog})
    check("无任务被漏扫", not missing, ",".join(missing[:6]))

    nodes = pipeline_nodes()
    check("核心 pipeline 节点表可读", len(nodes) > 1000, f"{len(nodes)} 个节点")

    # ---- #33：每个任务都要有 entry，且 entry 必须是核心里真实存在的节点 ----
    missing_entry = [t["name"] for t in catalog if not t.get("entry")]
    check("全部任务有 entry", not missing_entry, ",".join(missing_entry[:5]))
    bad_entry = [f"{t['name']}->{t['entry']}" for t in catalog
                 if t.get("entry") and t["entry"] not in nodes]
    check("entry 均存在于核心 pipeline", not bad_entry, "; ".join(bad_entry[:6]))

    # ---- option 定义与 catalog params 的覆盖率 ----
    opts = {}
    for _, d in docs:
        for k, v in (d.get("option") or {}).items():
            opts.setdefault(k, v)
    check("核心 option 定义可读", len(opts) > 400, f"{len(opts)} 条")

    specs = [(t["name"], p) for t in catalog for p in (t.get("params") or [])]
    check("option 规格非空", len(specs) > 0, f"{len(specs)} 条")
    refs = {}
    for _, d in docs:
        for t in (d.get("task") or []):
            if isinstance(t, dict) and t.get("name"):
                refs.setdefault(t["name"], set()).update(
                    o for o in (t.get("option") or []) if isinstance(o, str))
    have = {}
    for n, p in specs:
        have.setdefault(n, set()).add(p["name"])
    dropped = [f"{n}.{o}" for n, rl in refs.items() for o in rl if o not in have.get(n, set())]
    check("任务引用的 option 全部进 catalog", not dropped, "; ".join(dropped[:6]))

    # 核心 checkbox 是「多选」，MaaHub 现在塌成单选 bool：这一类不计入硬断言，
    # 单独在下面作为待决策缺陷上报，否则同一问题会被数两次
    multi_keys = {p["name"] for _, p in specs
                  if opts.get(p["name"], {}).get("type") == "checkbox"}
    plain = [(n, p) for n, p in specs if p["name"] not in multi_keys]
    multi = [(n, p) for n, p in specs if p["name"] in multi_keys]

    # ---- #34：option 类型必须是面板认得的三种，且 select/bool 要带 cases ----
    allowed = {"bool", "select", "string"}
    bad_type = [f"{n}.{p['name']}={p.get('type')}" for n, p in specs
                if p.get("type") not in allowed]
    check("option 类型合法", not bad_type, "; ".join(bad_type[:6]))
    no_cases = [f"{n}.{p['name']}" for n, p in specs
                if p.get("type") in ("bool", "select") and not p.get("cases")]
    check("bool/select 均带 cases", not no_cases, "; ".join(no_cases[:6]))
    bool_bad = [f"{n}.{p['name']}={p.get('cases')}" for n, p in plain
                if p.get("type") == "bool" and set(p.get("cases") or []) != {"Yes", "No"}]
    check("bool 的 cases 只有 Yes/No 两个值", not bool_bad, "; ".join(bool_bad[:6]))

    # ---- #35：核心给了默认值的规格要带 default，select 的 default 必须落在 cases 里 ----
    has_default_case = {k for k, v in opts.items() if "default_case" in v}
    no_default = [f"{n}.{p['name']}[{opts.get(p['name'], {}).get('type', '?')}]"
                  for n, p in plain
                  if p["name"] in has_default_case and "default" not in p]
    check("核心给了 default_case 的规格都带 default", not no_default,
          "; ".join(no_default[:6]))
    bad_default = [f"{n}.{p['name']}={p.get('default')!r}" for n, p in specs
                   if p.get("type") == "select" and "default" in p
                   and p["default"] not in (p.get("cases") or [])]
    check("select 默认值在 cases 内", not bad_default, "; ".join(bad_default[:6]))

    # ---- 已知缺陷：核心是多选，catalog 塌成单选 bool ----
    defect("checkbox（多选）塌成 bool，面板画不出多值",
           [f"{n}.{p['name']}（{len(p.get('cases') or [])} 个候选）" for n, p in multi],
           "; ".join(f"{n}.{p['name']}" for n, p in multi[:5]))
    lost_default = [f"{n}.{p['name']}" for n, p in specs
                    if isinstance(opts.get(p["name"], {}).get("default_case"), list)
                    and "default" not in p]
    defect("数组型 default_case 被 get_str 读成空串，官方默认勾选全丢", lost_default,
           "; ".join(lost_default[:5]))
    defect("apply_option 对无用户值的 checkbox 直接 sel=false，官方默认不进 override",
           [f"{n}.{p['name']}" for n, p in multi],
           "; ".join(f"{n}.{p['name']}" for n, p in multi[:5]))

    # ---- 走真实映射路径：--cli override 全量核对 entry + pipeline_override ----
    proc2 = subprocess.run([EXE, "--cli", "override", exec_id, "*"],
                           capture_output=True, timeout=600)
    plans = {}
    for line in proc2.stdout.decode("utf-8", "replace").splitlines():
        if "\t" not in line:
            continue
        name, _, blob = line.partition("\t")
        try:
            plans[name] = json.loads(blob)
        except ValueError:
            plans[name] = None
    check("override 探测覆盖全部任务", len(plans) == len(catalog),
          f"{len(plans)}/{len(catalog)}")
    bad_plan = [f"{n}({(plans[n] or {}).get('entry') if plans[n] else '解析失败'})"
                for n in plans
                if not isinstance(plans[n], dict) or plans[n].get("entry") not in nodes
                or not isinstance(plans[n].get("override"), dict)]
    check("entry+override 结构可用", not bad_plan, "; ".join(bad_plan[:6]))

    # 官方 default_case 指向的节点必须真的出现在 override 里，否则任务只是走个过场
    lost_nodes = []
    for n, p in multi:
        spec = opts[p["name"]]
        want = set()
        for cs in (spec.get("cases") or []):
            if cs.get("name") in (spec.get("default_case") or []):
                want |= set((cs.get("pipeline_override") or {}).keys())
        got = set(((plans.get(n) or {}).get("override")) or {})
        miss = sorted(want - got)
        if miss:
            lost_nodes.append(f"{n}.{p['name']} 缺 {','.join(miss[:3])}")
    defect("官方默认勾选的节点没进 override，任务等于空跑", lost_nodes,
           "; ".join(lost_nodes[:3]))

    # ---- 预设归属（DailyFull 铺默认勾选依赖它） ----
    with_presets = [t for t in catalog if t.get("presets")]
    names_by_preset = {}
    for t in with_presets:
        for pn in t["presets"]:
            names_by_preset.setdefault(pn, []).append(t["name"])
    check("有任务带预设归属", len(with_presets) > 0,
          f"{len(with_presets)} 个任务 / {len(names_by_preset)} 份预设")
    daily = names_by_preset.get("DailyFull", [])
    check("DailyFull 预设可解析", len(daily) >= 10, f"{len(daily)} 项: " + ",".join(daily[:5]))

    print("\n核心 option 类型：" + str(dict(Counter(v.get("type", "?") for v in opts.values()))))
    print("catalog params 类型：" + str(dict(Counter(p.get("type") for _, p in specs))))

    failed = [n for n, ok in results if not ok]
    print("\n" + "=" * 56)
    print(f"断言：{len(results) - len(failed)}/{len(results)} 通过")
    if failed:
        print("失败：" + ", ".join(failed))
    print("已知缺陷：" + (", ".join(f"{n}({c})" for n, c, _ in known) if known else "无"))
    for n, _, hits in known:
        print(f"  · {n}")
        for h in hits[:12]:
            print(f"      {h}")
        if len(hits) > 12:
            print(f"      …另 {len(hits) - 12} 处")
    if strict and known:
        print("--strict：已知缺陷计入失败")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.stdout.flush()
    sys.exit(main())
