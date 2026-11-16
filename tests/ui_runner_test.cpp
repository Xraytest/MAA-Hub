// Regression tests for UI runner parameter normalization and task panel helpers.
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/hub.hpp"
#include "../src/task_panels.hpp"

int main() {
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
        mj::Value out_cu = hub::Hub::normalize_task_params("maa", "Custom", cu);
        assert(out_cu["task_names"].items().size() == 3);
        assert(out_cu["task_names"].items()[1].as_string() == "TaskB");

        mj::Value cp = mj::Value::object();
        cp.set("copilot_file", mj::Value("stage.json"));
        mj::Value out_cp = hub::Hub::normalize_task_params("maa", "Copilot", cp);
        assert(out_cp.get_str("filename", "") == "stage.json");
    }

    // 5. task_has_advanced
    {
        assert(hub::task_has_advanced("Fight"));
        assert(!hub::task_has_advanced("StartUp"));
        assert(!hub::task_has_advanced("Infrast"));
        assert(!hub::task_has_advanced("Roguelike"));
    }

    printf("UI_RUNNER_TEST PASS\n");
    return 0;
}
