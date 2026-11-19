// Regression tests for UI runner parameter normalization and task panel helpers.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <windows.h>
#include <crtdbg.h>
#include "../src/hub.hpp"
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

    printf("UI_RUNNER_TEST PASS\n");
    return 0;
}
