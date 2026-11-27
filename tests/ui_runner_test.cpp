// Regression tests for UI runner parameter normalization and task panel helpers.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <windows.h>
#include <crtdbg.h>
#include "../src/hub.hpp"
#include "../src/stage_catalog.hpp"
#include "../src/task_panels.hpp"

// 测试辅助：构造指定时刻的 SYSTEMTIME（2026-11-18 是周三，dow 由参数覆盖以测星期过滤）
static SYSTEMTIME test_at(int h, int m, int dow) {
    SYSTEMTIME st = {};
    st.wYear = 2026; st.wMonth = 11; st.wDay = 18;
    st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wDayOfWeek = (WORD)dow;
    return st;
}

static mj::Value test_sched_entry() {
    mj::Value s = mj::Value::object();
    s.set("id", mj::Value("t"));
    s.set("exec_id", mj::Value("e"));
    s.set("enabled", mj::Value(true));
    return s;
}

int main() {
    // assert 失败必须走 stderr 并 abort，绝不弹对话框（无头环境弹窗 = 永久挂起）
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_error_mode(_OUT_TO_STDERR);
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 1. __once_* promotion & stripping + Fight drops/toggles/-1 unlimited
    {
        mj::Value in = mj::Value::object();
        in.set("use_medicine", mj::Value(false));
        in.set("__once_use_medicine", mj::Value(true));
        in.set("medicine", mj::Value(3));
        in.set("use_stone", mj::Value(false));
        in.set("stone", mj::Value(5));
        in.set("perform_battles", mj::Value(false));
        in.set("times", mj::Value(999));
        in.set("assigned_material", mj::Value(true));
        in.set("drop_item", mj::Value("\xe5\x9b\xba\xe6\xba\x90\xe5\xb2\xa9")); // 固源岩
        in.set("drop_quantity", mj::Value(12));
        mj::Value out = hub::Hub::normalize_task_params("maa", "Fight", in);
        assert(!out.has("__once_use_medicine"));
        assert(out.get_bool("use_medicine", false) == true);
        assert(out.get_int("medicine", -1) == 3);
        assert(out.get_int("stone", -1) == 0);
        assert(out.get_int("times", -1) == 2147483647);
        assert(out.has("drops") && out["drops"].get_int("30012", 0) == 12);

        mj::Value neg = mj::Value::object();
        neg.set("times", mj::Value(-1));
        mj::Value out_neg = hub::Hub::normalize_task_params("maa", "Fight", neg);
        assert(out_neg.get_int("times", -1) == 2147483647);
    }

    // 2. Infrast facility_order custom ordering
    {
        mj::Value in = mj::Value::object();
        mj::Value ord = mj::Value::array();
        ord.push((int64_t)2); // Trade
        ord.push((int64_t)1); // Mfg
        ord.push((int64_t)0); // Control
        in.set("facility_order", ord);
        in.set("facility_0", mj::Value(true));
        in.set("facility_1", mj::Value(true));
        in.set("facility_2", mj::Value(true));
        mj::Value out = hub::Hub::normalize_task_params("maa", "Infrast", in);
        auto& fac = out["facility"].items();
        assert(fac.size() == 3);
        assert(fac[0].as_string() == "Trade");
        assert(fac[1].as_string() == "Mfg");
        assert(fac[2].as_string() == "Control");
    }

    // 3. Roguelike mode mapping (0/1/4/6), theme/squad, and -1 unlimited starts_count
    {
        mj::Value in = mj::Value::object();
        in.set("theme", mj::Value(2)); // Sami
        in.set("squad", mj::Value(0)); // 指挥分队
        in.set("mode", mj::Value(2));  // 刷开局 -> 4
        in.set("start_times_limit", mj::Value(7));
        mj::Value out = hub::Hub::normalize_task_params("maa", "Roguelike", in);
        assert(out.get_str("theme", "") == "Sami");
        assert(out.get_str("squad", "") == "\xe6\x8c\x87\xe6\x8c\xa5\xe5\x88\x86\xe9\x98\x9f");
        assert(out.get_int("mode", -1) == 4);
        assert(out.get_int("starts_count", -1) == 7);

        mj::Value m3 = mj::Value::object();
        m3.set("mode", mj::Value(3)); // 月度小队 -> 6
        m3.set("starts_count", mj::Value(-1));
        mj::Value out3 = hub::Hub::normalize_task_params("maa", "Roguelike", m3);
        assert(out3.get_int("mode", -1) == 6);
        assert(out3.get_int("starts_count", -1) == 2147483647);
    }

    // 4. CloseDown, Custom, Copilot
    {
        mj::Value cd = mj::Value::object();
        cd.set("client_type", mj::Value(1));
        mj::Value out_cd = hub::Hub::normalize_task_params("maa", "CloseDown", cd);
        assert(out_cd.get_str("client_type", "") == "Bilibili");

        mj::Value cu = mj::Value::object();
        cu.set("task_names", mj::Value("TaskA, TaskB;TaskC"));
        cu.set("event_shop_blacklist", mj::Value("X;Y"));
        cu.set("use_normal_token", mj::Value(true));
        mj::Value out_cu = hub::Hub::normalize_task_params("maa", "Custom", cu);
        assert(out_cu["task_names"].items().size() == 3);
        assert(out_cu["task_names"].items()[1].as_string() == "TaskB");
        assert(out_cu.has("event_shop") && out_cu["event_shop"]["blacklist"].items().size() == 2);
        assert(out_cu["event_shop"]["blacklist"].items()[0].as_string() == "X");
        assert(out_cu["event_shop"]["blacklist"].items()[1].as_string() == "Y");
        assert(out_cu.has("auto_raise_potential") && out_cu["auto_raise_potential"].get_bool("use_normal_token", false) == true);

        mj::Value cp = mj::Value::object();
        cp.set("copilot_file", mj::Value("stage.json"));
        cp.set("stage_name", mj::Value(""));
        cp.set("support_unit_name", mj::Value(""));
        mj::Value out_cp = hub::Hub::normalize_task_params("maa", "Copilot", cp);
        assert(out_cp.get_str("filename", "") == "stage.json");
        assert(!out_cp.has("stage_name"));
        assert(!out_cp.has("support_unit_name"));

        mj::Value cp2 = mj::Value::object();
        cp2.set("filename", mj::Value("op.json"));
        cp2.set("stage_name", mj::Value("1-7"));
        cp2.set("support_unit_name", mj::Value("Wisadel"));
        mj::Value out_cp2 = hub::Hub::normalize_task_params("maa", "Copilot", cp2);
        assert(out_cp2.get_str("stage_name", "") == "1-7");
        assert(out_cp2.get_str("support_unit_name", "") == "Wisadel");
    }

    // 5. Infrast custom mode + builtin_plan + fiammetta/dorm toggles
    {
        mj::Value inf = mj::Value::object();
        inf.set("mode", mj::Value(1));
        inf.set("builtin_plan", mj::Value(3));
        inf.set("continue_training", mj::Value(true));
        inf.set("replenish", mj::Value(true));
        inf.set("dorm_trust", mj::Value(true));
        inf.set("dorm_filter_not_stationed", mj::Value(true));
        inf.set("fiammetta_targets", mj::Value("Rosmontis;Shining"));
        mj::Value out_inf = hub::Hub::normalize_task_params("maa", "Infrast", inf);
        assert(out_inf.get_int("mode", 0) == 10000);
        assert(out_inf.get_str("filename", "") == "243_layout_3_times_a_day.json");
        assert(out_inf.get_bool("continue_training", false) == true);
        assert(out_inf.get_bool("replenish", false) == true);
        assert(out_inf.get_bool("dorm_trust_enabled", false) == true);
        assert(out_inf.get_bool("dorm_notstationed_enabled", false) == true);
        assert(out_inf.has("fiammetta_targets") && out_inf["fiammetta_targets"].items().size() == 2);
        assert(out_inf["fiammetta_targets"].items()[0].as_string() == "Rosmontis");

        mj::Value inf_ov = inf;
        inf_ov.set("custom_infrast_file", mj::Value("C:\\plans\\my_252.json"));
        mj::Value out_ov = hub::Hub::normalize_task_params("maa", "Infrast", inf_ov);
        assert(out_ov.get_int("mode", 0) == 10000);
        assert(out_ov.get_str("filename", "") == "C:\\plans\\my_252.json");
    }

    // 6. Roguelike all 6 themes (JieGarden=4, BlackFlow=5)
    {
        mj::Value r4 = mj::Value::object();
        r4.set("theme", mj::Value(4));
        mj::Value o4 = hub::Hub::normalize_task_params("maa", "Roguelike", r4);
        assert(o4.get_str("theme", "") == "JieGarden");

        mj::Value r5 = mj::Value::object();
        r5.set("theme", mj::Value(5));
        mj::Value o5 = hub::Hub::normalize_task_params("maa", "Roguelike", r5);
        assert(o5.get_str("theme", "") == "BlackFlow");
    }

    // 7. Reclamation & OperBox
    {
        mj::Value rec = mj::Value::object();
        rec.set("theme", mj::Value(1));
        rec.set("tools_to_craft", mj::Value("ToolA;ToolB"));
        rec.set("clear_store", mj::Value(true));
        mj::Value orec = hub::Hub::normalize_task_params("maa", "Reclamation", rec);
        assert(orec.get_str("theme", "") == "Fire");
        assert(orec.has("tools_to_craft") && orec["tools_to_craft"].items().size() == 2);
        assert(orec["tools_to_craft"].items()[0].as_string() == "ToolA");
        assert(orec["tools_to_craft"].items()[1].as_string() == "ToolB");
        assert(orec.get_bool("clear_store", false) == true);

        mj::Value ob = mj::Value::object();
        ob.set("update_all", mj::Value(true));
        ob.set("target_oper", mj::Value("Amiya"));
        mj::Value oob = hub::Hub::normalize_task_params("maa", "OperBox", ob);
        assert(!oob.has("update_all"));
        assert(!oob.has("target_oper"));
        assert(oob.is_object() && oob.members().empty());
    }

    // 8. task_has_advanced
    {
        assert(hub::task_has_advanced("Fight"));
        assert(!hub::task_has_advanced("StartUp"));
        assert(!hub::task_has_advanced("Infrast"));
        assert(!hub::task_has_advanced("Roguelike"));
    }

    // 9. Hub unified schedule CRUD
    {
        hub::Hub h(mj::Value::object());
        mj::Value s = mj::Value::object();
        s.set("id", mj::Value("__test_sched_1"));
        s.set("exec_id", mj::Value("__test_exec_1"));
        s.set("kind", mj::Value("maa"));
        s.set("label", mj::Value("TestAccount"));
        s.set("enabled", mj::Value(true));
        mj::Value ts = mj::Value::array();
        ts.push(mj::Value("04:00"));
        ts.push(mj::Value("16:30"));
        s.set("times", std::move(ts));
        mj::Value wd = mj::Value::array();
        wd.push((int64_t)1);
        wd.push((int64_t)3);
        wd.push((int64_t)5);
        s.set("weekdays", std::move(wd));
        s.set("force_start", mj::Value(true));
        s.set("close_game_after", mj::Value(true));

        assert(h.upsert_schedule(s));
        auto snap1 = h.schedules_snapshot();
        bool found = false;
        for (const auto& e : snap1["entries"].items()) {
            if (e.get_str("id", "") == "__test_sched_1") {
                found = true;
                assert(e.get_bool("enabled", false) == true);
                assert(e.get_int("hour", -1) == 4);
                assert(e.get_int("min", -1) == 0);
                assert(e["times"].items().size() == 2);
                assert(e["weekdays"].items().size() == 3);
                assert(e.get_bool("force_start", false) == true);
                assert(e.get_bool("close_game_after", false) == true);
            }
        }
        assert(found);

        assert(h.toggle_schedule("__test_sched_1", false));
        auto snap2 = h.schedules_snapshot();
        for (const auto& e : snap2["entries"].items()) {
            if (e.get_str("id", "") == "__test_sched_1") {
                assert(e.get_bool("enabled", true) == false);
            }
        }

        assert(h.remove_schedule("__test_sched_1"));
        auto snap3 = h.schedules_snapshot();
        for (const auto& e : snap3["entries"].items()) {
            assert(e.get_str("id", "") != "__test_sched_1");
        }
    }

    // 10. schedule_due_slot：Core 定时器语义（精确分钟匹配、无追赶、当日去重、星期过滤）
    {
        const std::string today = "2026-11-18";
        auto mk = &test_sched_entry;
        auto at = &test_at;

        // 精确匹配：到点命中
        {
            mj::Value s = mk();
            mj::Value ts = mj::Value::array();
            ts.push(mj::Value("04:00"));
            ts.push(mj::Value("16:30"));
            s.set("times", std::move(ts));
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 3), today) == "04:00");
            assert(hub::Hub::schedule_due_slot(s, at(16, 30, 3), today) == "16:30");
            // 无追赶窗口：晚 1 分钟不命中（Core 语义）
            assert(hub::Hub::schedule_due_slot(s, at(4, 1, 3), today).empty());
            assert(hub::Hub::schedule_due_slot(s, at(17, 0, 3), today).empty());
            // 早到不命中
            assert(hub::Hub::schedule_due_slot(s, at(3, 59, 3), today).empty());
        }
        // 当日该时段已触发 → 不再触发
        {
            mj::Value s = mk();
            mj::Value ts = mj::Value::array();
            ts.push(mj::Value("04:00"));
            ts.push(mj::Value("16:30"));
            s.set("times", std::move(ts));
            mj::Value lrs = mj::Value::array();
            lrs.push(mj::Value(today + "@04:00"));
            s.set("last_run_slots", std::move(lrs));
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 3), today).empty());
            // 另一个时段照常触发
            assert(hub::Hub::schedule_due_slot(s, at(16, 30, 3), today) == "16:30");
            // 昨天的 slot 记录不影响今天
            mj::Value s2 = s;
            mj::Value lrs2 = mj::Value::array();
            lrs2.push(mj::Value("2026-11-17@04:00"));
            s2.set("last_run_slots", std::move(lrs2));
            assert(hub::Hub::schedule_due_slot(s2, at(4, 0, 3), today) == "04:00");
        }
        // 旧数据兼容：只有 last_run_date/last_run_time
        {
            mj::Value s = mk();
            mj::Value ts = mj::Value::array();
            ts.push(mj::Value("04:00"));
            s.set("times", std::move(ts));
            s.set("last_run_date", mj::Value(today));
            s.set("last_run_time", mj::Value("04:00"));
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 3), today).empty());
        }
        // 星期过滤：weekdays=[1,3,5]，周三命中、周四不命中
        {
            mj::Value s = mk();
            mj::Value ts = mj::Value::array();
            ts.push(mj::Value("04:00"));
            s.set("times", std::move(ts));
            mj::Value wd = mj::Value::array();
            wd.push((int64_t)1); wd.push((int64_t)3); wd.push((int64_t)5);
            s.set("weekdays", std::move(wd));
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 3), today) == "04:00");
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 4), today).empty());
            // weekdays 空/缺失 = 每天
            mj::Value s2 = mk();
            mj::Value ts2 = mj::Value::array();
            ts2.push(mj::Value("04:00"));
            s2.set("times", std::move(ts2));
            assert(hub::Hub::schedule_due_slot(s2, at(4, 0, 6), today) == "04:00");
        }
        // hour/min 单点兜底（无 times）
        {
            mj::Value s = mk();
            s.set("hour", (int64_t)7);
            s.set("min", (int64_t)30);
            assert(hub::Hub::schedule_due_slot(s, at(7, 30, 3), today) == "07:30");
            assert(hub::Hub::schedule_due_slot(s, at(7, 31, 3), today).empty());
        }
        // 禁用条目永不触发
        {
            mj::Value s = mk();
            s.set("enabled", mj::Value(false));
            mj::Value ts = mj::Value::array();
            ts.push(mj::Value("04:00"));
            s.set("times", std::move(ts));
            assert(hub::Hub::schedule_due_slot(s, at(4, 0, 3), today).empty());
        }
    }

    // 11.（r28）设置模型往返：per-kind 自动更新开关 + 旧配置缺键默认 false
    {
        hub::AppSettings s;
        s.update_auto_download = true;
        s.update_auto_install = true;
        s.update_auto_maa = true;
        s.update_auto_maaend = false;
        hub::AppSettings r = hub::AppSettings::from_json(s.to_json());
        assert(r.update_auto_download == true);
        assert(r.update_auto_install == true);
        assert(r.update_auto_maa == true);      // 开关经 to_json/from_json 不丢
        assert(r.update_auto_maaend == false);
        // 旧配置（无 update_auto_* 键）→ 默认 false，绝不能凭空开启自动更新
        mj::Value legacy = mj::Value::object();
        mj::Value su = mj::Value::object();
        su.set("update_check_on_start", mj::Value(true));
        legacy.set("startup_update", std::move(su));
        hub::AppSettings l = hub::AppSettings::from_json(legacy);
        assert(l.update_auto_maa == false);
        assert(l.update_auto_maaend == false);
        assert(l.update_check_on_start == true);
    }

    // 12. 任务副本键：UI/持久化使用实例键，核心排序与执行使用基础类型。
    {
        assert(hub::Hub::task_base_name("Fight") == "Fight");
        assert(hub::Hub::task_base_name("Fight#12") == "Fight");
        auto ordered = hub::Hub::canonical_task_order("maa",
            { "Fight#2", "CloseDown", "StartUp", "Fight" });
        assert(ordered.size() == 4);
        assert(ordered[0] == "StartUp");
        assert(ordered[1] == "Fight#2" && ordered[2] == "Fight");
        assert(ordered[3] == "CloseDown");

        mj::Value themes = mj::Value::object();
        themes.set("themes", mj::Value("默认;暗色\n明亮"));
        mj::Value theme_out = hub::Hub::normalize_task_params("maa", "SwitchTheme", themes);
        assert(theme_out["themes"].is_array() && theme_out["themes"].items().size() == 3);
        assert(theme_out["themes"].items()[1].as_string() == "暗色");

        mj::Value ops = mj::Value::object();
        ops.set("operators", mj::Value("艾雅法拉;令\n维娜"));
        mj::Value ops_out = hub::Hub::normalize_task_params("maa", "OperProgress", ops);
        assert(ops_out["operators"].is_array() && ops_out["operators"].items().size() == 3);
    }

    // 13. 关卡目录：活动时区、过期过滤、今日提示与自定义值保留
    {
        using namespace hub::stage_catalog;
        double utc_start = parse_activity_time("2026/11/22 04:00:00", 8);
        double utc_same = parse_activity_time("2026/11/21 20:00:00", 0);
        assert(utc_start == utc_same);
        assert(parse_activity_time("2026/11/25 04:00:00", -7)
               == parse_activity_time("2026/11/25 11:00:00", 0));
        assert(game_day_of_week(parse_activity_time("2026/11/24 19:59:00", 0), "Official") == 2);
        assert(game_day_of_week(parse_activity_time("2026/11/24 20:00:00", 0), "Official") == 3);
        assert(normalize_server("Bilibili") == "Official");
        assert(valid_stage_code("SSReopen-OS"));
        assert(valid_stage_code("WR-1"));
        assert(!valid_stage_code("---"));

        mj::Value stages = mj::Value::array();
        for (const char* code : { "1-7", "OS-9", "TL-9", "CE-6", "WR-1", "OF-1" }) {
            mj::Value stage = mj::Value::object();
            stage.set("code", mj::Value(code));
            stages.push(std::move(stage));
        }
        const char* activity_json = R"JSON({
          "Official": {
            "sideStoryStage": {
              "OS": {
                "Activity": {
                  "Tip": "雪山降临1101复刻",
                  "StageName": "雪山降临1101",
                  "UtcStartTime": "2026/11/22 04:00:00",
                  "UtcExpireTime": "2026/12/02 03:59:59",
                  "TimeZone": 8
                },
                "Stages": [
                  { "Display": "SSReopen-OS", "Value": "SSReopen-OS", "Drop": "代理1~9关" },
                  { "Display": "OS-9", "Value": "OS-9", "Drop": "31053" },
                  { "Display": "OS-8", "Value": "OS-8", "Drop": "30103" },
                  { "Display": "OS-7", "Value": "OS-7", "Drop": "代理1~9关" }
                ]
              },
              "TL": {
                "Activity": {
                  "Tip": "时间啊，向前！",
                  "UtcStartTime": "2026/11/01 12:00:00",
                  "UtcExpireTime": "2026/11/22 03:59:59",
                  "TimeZone": 8
                },
                "Stages": [
                  { "Display": "TL-9", "Value": "TL-9", "Drop": "31113" }
                ]
              }
            },
            "miniGame": [
              {
                "Activity": "雪山降临1101",
                "Display": "OS-喀兰贸易技术研发部",
                "Value": "MiniGame@OS@Begin",
                "UtcStartTime": "2026/11/22 04:00:00",
                "UtcExpireTime": "2026/12/02 03:59:59",
                "TimeZone": 8
              },
              {
                "Activity": "时间啊，向前！",
                "Display": "TL-过期小游戏",
                "Value": "MiniGame@TL@Begin",
                "UtcStartTime": "2026/11/01 12:00:00",
                "UtcExpireTime": "2026/11/22 03:59:59",
                "TimeZone": 8
              },
              {
                "Display": "常驻小游戏",
                "Value": "MiniGame@Permanent@Begin",
                "UtcStartTime": "2026/01/01 00:00:00",
                "UtcExpireTime": "2099/01/01 00:00:00",
                "TimeZone": 8
              }
            ]
          }
        })JSON";
        mj::Value activities = mj::Parser(activity_json).parse();
        const char* item_json = R"JSON({
          "31053": { "name": "化合切削液" },
          "30103": { "name": "RMA70-12" },
          "31113": { "name": "电极单元" }
        })JSON";
        mj::Value items = mj::Parser(item_json).parse();
        double now = parse_activity_time("2026/11/25 12:00:00", 0);
        StageCatalog catalog = build(stages, activities, items, "Official", now);
        bool found_os = false, found_tl = false, found_wr = false;
        bool found_of = false, found_annihilation = false, found_ssreopen = false;
        for (const auto& stage : catalog.stages) {
            if (stage.value == "OS-9") {
                found_os = true;
                assert(stage.activity && stage.open && !stage.outdated);
                assert(stage.display == "OS-9" && stage.drop == "31053");
            }
            if (stage.value == "TL-9") {
                found_tl = true;
                assert(stage.activity && !stage.open && stage.outdated);
            }
            if (stage.value == "WR-1") found_wr = true;
            if (stage.value == "OF-1") found_of = true;
            if (stage.value == "Annihilation") {
                found_annihilation = true;
                assert(stage.display == "剿灭模式");
            }
            if (stage.value == "SSReopen-OS") found_ssreopen = true;
        }
        assert(found_os && found_tl && found_ssreopen && !found_wr && !found_of && found_annihilation);
        auto tips = today_activity_tips(catalog, now);
        bool tip_os = false, tip_tl = false, tip_os_mini = false;
        bool tip_tl_mini = false, tip_permanent_mini = false;
        bool tip_material_name = false, tip_material_id = false, tip_text_drop = false;
        for (const auto& tip : tips) {
            if (tip.find("雪山降临1101复刻") != std::string::npos) tip_os = true;
            if (tip.find("OS-9") != std::string::npos) tip_os = true;
            if (tip.find("OS-喀兰贸易技术研发部") != std::string::npos) tip_os_mini = true;
            if (tip.find("化合切削液") != std::string::npos) tip_material_name = true;
            if (tip.find("31053") != std::string::npos) tip_material_id = true;
            if (tip.find("代理1~9关") != std::string::npos) tip_text_drop = true;
            if (tip.find("TL-9") != std::string::npos) tip_tl = true;
            if (tip.find("TL-过期小游戏") != std::string::npos) tip_tl_mini = true;
            if (tip.find("常驻小游戏") != std::string::npos) tip_permanent_mini = true;
        }
        assert(tip_os && tip_os_mini && tip_material_name && tip_text_drop
               && !tip_material_id && !tip_tl && !tip_tl_mini && !tip_permanent_mini);
        assert(!activity_open(catalog.activities[0], catalog.activities[0].start));
        assert(!activity_open(catalog.activities[0], catalog.activities[0].expire));

        auto visible = choices_for(catalog, "TL-9", true);
        bool kept_closed_current = false, kept_os = false;
        for (const auto& stage : visible) {
            if (stage.value == "TL-9") kept_closed_current = true;
            if (stage.value == "OS-9") kept_os = true;
        }
        assert(kept_closed_current && kept_os);
        bool ap_closed = false;
        for (const auto& stage : catalog.stages)
            if (stage.value == "AP-5") ap_closed = !stage.open;
        assert(ap_closed);
        auto open_choices = choices_for(catalog, "", false);
        bool listed_tl = false, listed_wr = false, listed_closed_ap = false;
        for (const auto& stage : open_choices) {
            if (stage.value == "TL-9") listed_tl = true;
            if (stage.value == "WR-1") listed_wr = true;
            if (stage.value == "AP-5") listed_closed_ap = true;
        }
        assert(!listed_tl && !listed_wr && listed_closed_ap);
        auto hidden_choices = choices_for(catalog, "", true);
        bool hidden_ap = false;
        for (const auto& stage : hidden_choices)
            if (stage.value == "AP-5") hidden_ap = true;
        assert(!hidden_ap);
        auto hidden_current = choices_for(catalog, "AP-5", true);
        int current_ap_count = 0;
        for (const auto& stage : hidden_current)
            if (stage.value == "AP-5") ++current_ap_count;
        assert(current_ap_count == 1);
        auto custom = choices_for(catalog, "CUSTOM-99", true);
        assert(!custom.empty() && custom.back().value == "CUSTOM-99");
    }

    // 14. 关卡控件交互：光标编辑、选区替换、下拉选择与候选关卡
    {
        using namespace hub::stage_catalog;
        Gdiplus::GdiplusStartupInput gdip_input;
        ULONG_PTR gdip = 0;
        assert(Gdiplus::GdiplusStartup(&gdip, &gdip_input, nullptr) == Gdiplus::Ok);
        {
            gui::Gui g;
            g.init_fonts();
            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 800, 600);
            ReleaseDC(nullptr, screen);
            HBITMAP old_bitmap = (HBITMAP)SelectObject(mem, bitmap);
            RECT surface{ 0, 0, 800, 600 };
            g.set_dc(mem, surface);
            auto frame = [&](const gui::Input& in, const std::function<void()>& draw) {
                PatBlt(mem, 0, 0, 800, 600, WHITENESS);
                g.begin(in);
                draw();
                g.flush_combo_popup();
                g.end();
            };
            gui::Input idle;

            std::string text = "1-7";
            RECT text_rect{ 20, 20, 220, 50 };
            auto draw_text = [&] { g.text_input("test:Fight:stage0", text_rect, text); };
            gui::Input click;
            click.mouse = { text_rect.left + 2, 35 };
            click.down = click.pressed = true;
            frame(click, draw_text);
            assert(g.focused_with_prefix("test:Fight:"));
            gui::Input typed;
            typed.char_inputs = L"X";
            frame(typed, draw_text);
            assert(text == "X1-7");

            gui::Input home;
            home.vk = VK_HOME;
            frame(home, draw_text);
            gui::Input del;
            del.vk = VK_DELETE;
            frame(del, draw_text);
            assert(text == "1-7");
            gui::Input end;
            end.vk = VK_END;
            frame(end, draw_text);
            gui::Input back;
            back.vk = VK_BACK;
            frame(back, draw_text);
            assert(text == "1-");

            click.mouse = { text_rect.left + 2, 35 };
            frame(click, draw_text);
            gui::Input drag = click;
            drag.pressed = false;
            drag.mouse.x = text_rect.right - 2;
            frame(drag, draw_text);
            typed.char_inputs = L"OS-9";
            frame(typed, draw_text);
            assert(text == "OS-9");
            click.mouse = { 500, 500 };
            frame(click, draw_text);
            typed.char_inputs = L"Z";
            frame(typed, draw_text);
            assert(text == "OS-9");

            RECT combo_rect{ 20, 100, 220, 130 };
            std::vector<std::wstring> combo_items{ L"OS-9", L"OS-8" };
            int combo_cur = 0;
            auto draw_combo = [&] {
                int hit = g.combo("test:combo", combo_rect, combo_items, combo_cur);
                if (hit >= 0) combo_cur = hit;
            };
            click.mouse = { 120, 115 };
            frame(click, draw_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_combo);
            click.pressed = true;
            click.released = false;
            click.mouse.y = combo_rect.bottom + 2 + 26 + 13;
            frame(click, draw_combo);
            frame(idle, draw_combo);
            assert(combo_cur == 1);

            RECT key_combo_rect{ 20, 160, 220, 190 };
            std::vector<std::wstring> key_items{ L"Alpha", L"Bravo", L"Charlie" };
            int key_cur = 0;
            auto draw_key_combo = [&] {
                int hit = g.combo("test:key-combo", key_combo_rect, key_items, key_cur);
                if (hit >= 0) key_cur = hit;
            };
            click.mouse = { 120, 175 };
            click.down = click.pressed = true;
            frame(click, draw_key_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_key_combo);
            gui::Input hover;
            hover.mouse = { 120, key_combo_rect.bottom + 2 + 13 };
            frame(hover, draw_key_combo);
            gui::Input down;
            down.vk = VK_DOWN;
            down.mouse = hover.mouse;
            frame(down, draw_key_combo);
            gui::Input enter;
            enter.vk = VK_RETURN;
            frame(enter, draw_key_combo);
            frame(idle, draw_key_combo);
            assert(key_cur == 1);
            click.mouse = { 120, 175 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_key_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_key_combo);
            down.vk = VK_DOWN;
            frame(down, draw_key_combo);
            gui::Input escape;
            escape.vk = VK_ESCAPE;
            frame(escape, draw_key_combo);
            frame(idle, draw_key_combo);
            assert(key_cur == 1);
            click.mouse = { 120, 175 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_key_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_key_combo);
            typed.char_inputs = L"c";
            frame(typed, draw_key_combo);
            frame(enter, draw_key_combo);
            frame(idle, draw_key_combo);
            assert(key_cur == 2);

            RECT search_combo_rect{ 20, 220, 220, 250 };
            std::vector<std::wstring> search_items{ L"OS-9", L"OS-8", L"1-7" };
            int search_cur = 0;
            auto draw_search_combo = [&] {
                int hit = g.combo("test:search-combo", search_combo_rect, search_items,
                                  search_cur, L"", false, true);
                if (hit >= 0) search_cur = hit;
            };
            click.mouse = { 120, 235 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_search_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_search_combo);
            typed.char_inputs = L"OS-8";
            frame(typed, draw_search_combo);
            frame(enter, draw_search_combo);
            frame(idle, draw_search_combo);
            assert(search_cur == 1);

            RECT scroll_combo_rect{ 20, 300, 220, 330 };
            std::vector<std::wstring> scroll_items;
            for (int i = 0; i < 20; ++i) scroll_items.push_back(L"Item-" + std::to_wstring(i));
            int scroll_cur = 19;
            auto draw_scroll_combo = [&] {
                int hit = g.combo("test:scroll-combo", scroll_combo_rect, scroll_items, scroll_cur);
                if (hit >= 0) scroll_cur = hit;
            };
            click.mouse = { 120, 315 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_scroll_combo);
            click.pressed = false;
            click.released = true;
            frame(click, draw_scroll_combo);
            frame(idle, draw_scroll_combo);
            click.mouse = { 217, 513 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_scroll_combo);
            gui::Input scroll_drag = click;
            scroll_drag.pressed = false;
            scroll_drag.mouse.y = 334;
            frame(scroll_drag, draw_scroll_combo);
            scroll_drag.down = false;
            scroll_drag.released = true;
            frame(scroll_drag, draw_scroll_combo);
            frame(idle, draw_scroll_combo);
            click.mouse = { 120, 345 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_scroll_combo);
            frame(idle, draw_scroll_combo);
            assert(scroll_cur == 0);

            StageCatalog catalog;
            catalog.stages = {
                StageChoice{ "OS-9", "OS-9", "31053", true, false, true },
                StageChoice{ "OS-8", "OS-8", "30103", true, false, true },
                StageChoice{ "1-7", "1-7", "", true }
            };
            std::string exec = "exec";
            std::string task = "Fight";
            hub::ParamMap params;
            params["stage"] = mj::Value("OS-9");
            hub::PanelCtx pctx{ g, 20, 0, 400, params, exec, task, false };
            pctx.stage_catalog = [&] { return &catalog; };
            auto draw_fight = [&] {
                pctx.y = 0;
                hub::panel_fight(pctx);
            };
            RECT fight_stage{ 126, 216, 394, 246 };
            frame(idle, draw_fight);
            click.mouse = { 260, 231 };
            click.down = click.pressed = true;
            frame(click, draw_fight);
            click.pressed = false;
            click.released = true;
            frame(click, draw_fight);
            click.pressed = true;
            click.released = false;
            click.mouse.y = fight_stage.bottom + 2 + 26 + 13;
            frame(click, draw_fight);
            frame(idle, draw_fight);
            assert(params["stage"].as_string() == "OS-8");

            params["use_alternate_stage"] = mj::Value(false);
            params["candidate_stages"] = mj::Value("");
            frame(idle, draw_fight);
            click.mouse = { 70, 267 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_fight);
            click.pressed = false;
            click.released = true;
            frame(click, draw_fight);
            frame(idle, draw_fight);
            assert(params["use_alternate_stage"].as_bool());
            assert(params["candidate_stages"].as_string() == "OS-8");

            params["use_custom_stage"] = mj::Value(true);
            frame(idle, draw_fight);
            click.mouse = { fight_stage.left + 24, 231 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_fight);
            typed.char_inputs = L"X";
            frame(typed, draw_fight);
            assert(params["stage"].as_string() == "XOS-8");

            params["use_custom_stage"] = mj::Value(false);
            params["use_alternate_stage"] = mj::Value(true);
            params["stage"] = mj::Value("OS-9");
            params["candidate_stages"] = mj::Value("1-7");
            frame(idle, draw_fight);
            RECT candidate_stage{ 128, 250, 368, 280 };
            click.mouse = { 250, 265 };
            frame(click, draw_fight);
            click.pressed = false;
            click.released = true;
            frame(click, draw_fight);
            click.pressed = true;
            click.released = false;
            click.mouse.y = candidate_stage.bottom + 2 + 26 + 13;
            frame(click, draw_fight);
            frame(idle, draw_fight);
            assert(params["candidate_stages"].as_string() == "OS-8");

            params["candidate_stages"] = mj::Value("1-7,OS-8");
            frame(idle, draw_fight);
            click.mouse = { 137, 297 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_fight);
            gui::Input candidate_drag = click;
            candidate_drag.pressed = false;
            candidate_drag.mouse.y = 265;
            frame(candidate_drag, draw_fight);
            candidate_drag.down = false;
            candidate_drag.released = true;
            frame(candidate_drag, draw_fight);
            frame(idle, draw_fight);
            assert(params["candidate_stages"].as_string() == "OS-8,1-7");

            params["stage"] = mj::Value("OS-9");
            params["candidate_stages"] = mj::Value("1-7,OS-8");
            frame(idle, draw_fight);
            click.mouse = { 137, 233 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_fight);
            gui::Input primary_drag = click;
            primary_drag.pressed = false;
            primary_drag.mouse.y = 265;
            frame(primary_drag, draw_fight);
            primary_drag.down = false;
            primary_drag.released = true;
            frame(primary_drag, draw_fight);
            frame(idle, draw_fight);
            assert(params["stage"].as_string() == "1-7");
            assert(params["candidate_stages"].as_string() == "OS-9,OS-8");

            hub::ParamMap infra_params;
            infra_params["mode"] = mj::Value((int64_t)0);
            std::string infra_exec = "exec";
            std::string infra_task = "Infrast";
            hub::PanelCtx infra_ctx{ g, 20, 0, 400, infra_params,
                                     infra_exec, infra_task, false };
            auto draw_infrast = [&] {
                infra_ctx.y = 0;
                hub::panel_infrast(infra_ctx);
            };
            frame(idle, draw_infrast);
            click.mouse = { 200, 31 };
            click.down = click.pressed = true;
            click.released = false;
            frame(click, draw_infrast);
            click.pressed = false;
            click.released = true;
            frame(click, draw_infrast);
            click.pressed = true;
            click.released = false;
            click.mouse.y = 16 + 30 + 2 + 26 + 13;
            frame(click, draw_infrast);
            frame(idle, draw_infrast);
            assert(infra_params["mode"].as_int() == 1);

            SelectObject(mem, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(mem);
        }
        Gdiplus::GdiplusShutdown(gdip);
    }

    printf("UI_RUNNER_TEST PASS\n");
    return 0;
}
