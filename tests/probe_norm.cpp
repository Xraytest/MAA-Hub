// 探针：验证 normalize_task_params / canonical_task_order 的最终下发形态。
// 不参与产品构建；单独 cl 编译（见 probe_norm.bat）。
#include "../src/hub.hpp"
#include <cstdio>
#include <string>

using namespace hub;

static void show(const char* tag, const std::string& kind, const std::string& task,
                 const mj::Value& in) {
    mj::Value out = Hub::normalize_task_params(kind, task, in);
    printf("%s %s => %s\n", tag, task.c_str(), mj::Value::dump(out).c_str());
}

int main() {
    // --- Infrast: 面板键 -> 核心参数（maa1 drones_idx=5 必须出 PureGold）
    {
        mj::Value in = mj::Value::object();
        in.set("mode", mj::Value(0));
        in.set("drones_idx", mj::Value(5));
        in.set("dorm_threshold", mj::Value(20));
        for (int i = 0; i < 8; ++i)
            in.set("facility_" + std::to_string(i), mj::Value(true));
        show("M1", "maa", "Infrast", in);
    }
    // --- Infrast: drones_idx 缺失 + 已有 drones 字符串 -> 保留（编辑④ gate）
    {
        mj::Value in = mj::Value::object();
        in.set("drones", mj::Value("Money"));
        in.set("mode", mj::Value(10000));
        show("GATE", "maa", "Infrast", in);
    }
    // --- Infrast: drones_idx=1 (maa3/5 Money)
    {
        mj::Value in = mj::Value::object();
        in.set("drones_idx", mj::Value(1));
        in.set("mode", mj::Value(0));
        show("M35", "maa", "Infrast", in);
    }
    // --- Mall: 分号字符串 -> 数组（中文 UTF-8）
    {
        mj::Value in = mj::Value::object();
        in.set("blacklist_str", mj::Value("\xe5\x8a\xa0\xe6\x80\xa5\xe8\xae\xb8\xe5\x8f\xaf"));  // 加急许可
        in.set("buy_first_str", mj::Value("\xe6\x8b\x9b\xe8\x81\x98\xe8\xae\xb8\xe5\x8f\xaf"));  // 招聘许可
        in.set("shopping", mj::Value(true));
        in.set("visit_friends", mj::Value(true));
        show("MALL", "maa", "Mall", in);
    }
    // --- Mall: 空串不写（保留已有值）
    {
        mj::Value in = mj::Value::object();
        in.set("blacklist_str", mj::Value(""));
        show("MALL-EMPTY", "maa", "Mall", in);
    }
    // --- StartUp: int 下标 -> 字符串
    {
        mj::Value in = mj::Value::object();
        in.set("client_type", mj::Value(0));
        in.set("account_name_set", mj::Value(true));
        show("SU0", "maa", "StartUp", in);
        in.set("client_type", mj::Value(2));
        show("SU2", "maa", "StartUp", in);
        in.set("client_type", mj::Value(9));
        show("SU9", "maa", "StartUp", in);
    }
    // --- StartUp: 字符串直通 / 非法字符串回退 Official / 缺失补 Official
    {
        mj::Value in = mj::Value::object();
        in.set("client_type", mj::Value("Bilibili"));
        show("SUS", "maa", "StartUp", in);
        in.set("client_type", mj::Value("Nonsense"));
        show("SUBAD", "maa", "StartUp", in);
        mj::Value empty = mj::Value::object();
        show("SUEMPTY", "maa", "StartUp", empty);
    }
    // --- Recruit: confirm 数组
    {
        mj::Value in = mj::Value::object();
        in.set("confirm_3", mj::Value(true));
        in.set("confirm_4", mj::Value(true));
        in.set("confirm_5", mj::Value(true));
        in.set("confirm_6", mj::Value(false));
        in.set("refresh", mj::Value(true));
        in.set("times", mj::Value(300));
        show("REC", "maa", "Recruit", in);
    }
    // --- canonical order：task-lists 字典序 -> 官方日常序
    {
        std::vector<std::string> t = { "Award", "Fight", "Infrast", "Mall", "Recruit", "StartUp" };
        auto r = Hub::canonical_task_order("maa", t);
        printf("ORDER-MAA =>");
        for (auto& s : r) printf(" %s", s.c_str());
        printf("\n");
        std::vector<std::string> t2 = { "Award", "StartUp", "Copilot", "CloseDown", "Fight" };
        auto r2 = Hub::canonical_task_order("maa", t2);
        printf("ORDER-MIX =>");
        for (auto& s : r2) printf(" %s", s.c_str());
        printf("\n");
        std::vector<std::string> t3 = { "DailyRewards", "CloseGamePC", "AutoSell" };
        auto r3 = Hub::canonical_task_order("maaend", t3);
        printf("ORDER-END =>");
        for (auto& s : r3) printf(" %s", s.c_str());
        printf("\n");
    }
    printf("PROBE-DONE\n");
    return 0;
}
