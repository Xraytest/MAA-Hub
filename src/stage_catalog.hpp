#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace hub::stage_catalog {

struct StageChoice {
    std::string value;
    std::string display;
    std::string drop;
    bool open = true;
    bool outdated = false;
    bool activity = false;
    bool resource_collection = false;
    double start = 0.0;
    double expire = 0.0;
};

struct StageActivity {
    struct RelatedEntry {
        std::string value;
        std::string display;
        std::string tip;
    };

    std::string key;
    std::string tip;
    std::string stage_name;
    double start = 0.0;
    double expire = 0.0;
    bool resource_collection = false;
    std::vector<StageChoice> stages;
    std::vector<RelatedEntry> related_entries;
};

struct StageCatalog {
    std::string server = "Official";
    std::vector<StageChoice> stages;
    std::vector<StageActivity> activities;
};

inline std::string trim_bom(std::string text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF
        && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

inline std::string read_text_file(const std::string& path) {
    HANDLE h = CreateFileW(util::utf8_to_wide(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return {};
    }
    std::string text((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    bool ok = ReadFile(h, text.data(), (DWORD)text.size(), &read, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return {};
    text.resize(read);
    return trim_bom(std::move(text));
}

inline bool write_text_file(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(util::utf8_to_wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) != 0
              && written == text.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(util::utf8_to_wide(tmp).c_str());
        return false;
    }
    return MoveFileExW(util::utf8_to_wide(tmp).c_str(), util::utf8_to_wide(path).c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
}

inline double parse_activity_time(const std::string& text, int timezone) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf_s(text.c_str(), "%d/%d/%d %d:%d:%d",
                 &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0.0;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    time_t utc = _mkgmtime(&tm);
    if (utc == (time_t)-1) return 0.0;
    return (double)utc - (double)timezone * 3600.0;
}

inline std::string normalize_server(std::string server) {
    if (server == "Bilibili") return "Official";
    static const char* const known[] = { "Official", "txwy", "YoStarEN", "YoStarJP", "YoStarKR" };
    for (const char* k : known) {
        if (server == k) return server;
    }
    return "Official";
}

inline int activity_timezone(const std::string& server, int fallback = 0) {
    if (fallback != 0) return fallback;
    if (server == "YoStarEN") return -7;
    if (server == "YoStarJP" || server == "YoStarKR") return 9;
    return 8;
}

inline int game_day_offset(const std::string& server) {
    if (server == "YoStarEN") return -7;
    if (server == "YoStarJP" || server == "YoStarKR") return 9;
    if (server == "txwy") return 8;
    return 4;
}

inline int game_day_of_week(double now, const std::string& server) {
    time_t shifted = (time_t)std::floor(now + game_day_offset(server) * 3600.0);
    std::tm tm{};
    _gmtime64_s(&tm, &shifted);
    return tm.tm_wday;
}

inline bool weekly_stage_open(const std::string& code, int day_of_week) {
    static const std::map<std::string, std::vector<int>> days = {
        { "CE-6", { 2, 4, 6, 0 } }, { "AP-5", { 1, 4, 6, 0 } },
        { "CA-5", { 2, 3, 5, 0 } }, { "SK-5", { 1, 3, 5, 6 } },
        { "PR-A-1", { 1, 4, 5, 0 } }, { "PR-A-2", { 1, 4, 5, 0 } },
        { "PR-B-1", { 1, 2, 5, 6 } }, { "PR-B-2", { 1, 2, 5, 6 } },
        { "PR-C-1", { 3, 4, 6, 0 } }, { "PR-C-2", { 3, 4, 6, 0 } },
        { "PR-D-1", { 2, 3, 6, 0 } }, { "PR-D-2", { 2, 3, 6, 0 } },
    };
    auto it = days.find(code);
    if (it == days.end()) return true;
    return std::find(it->second.begin(), it->second.end(), day_of_week) != it->second.end();
}

inline bool valid_stage_code(const std::string& code) {
    if (code.empty() || code.size() > 64) return false;
    bool has_digit = false;
    for (unsigned char ch : code) {
        if (ch >= 0x80 || std::isspace(ch)) return false;
        if (!(std::isalnum(ch) || ch == '-' || ch == '_')) return false;
        if (std::isdigit(ch)) has_digit = true;
    }
    return has_digit;
}

inline std::vector<StageChoice> default_stages() {
    static const char* const codes[] = {
        "1-7", "LS-6", "CE-6", "AP-5", "CA-5", "SK-5",
        "PR-A-1", "PR-A-2", "PR-B-1", "PR-B-2", "PR-C-1", "PR-C-2", "PR-D-1", "PR-D-2"
    };
    std::vector<StageChoice> out;
    for (const char* code : codes) out.push_back(StageChoice{ code, code, "" });
    return out;
}

inline std::vector<StageChoice> parse_stage_resource(const mj::Value& root, double now,
                                                      const std::string& server) {
    std::vector<StageChoice> out;
    if (root.is_array()) {
        int day = game_day_of_week(now, server);
        for (const auto& item : root.items()) {
            if (!item.is_object()) continue;
            std::string code = item.get_str("code", "");
            if (!valid_stage_code(code)) continue;
            StageChoice choice;
            choice.value = code;
            choice.display = code;
            choice.open = weekly_stage_open(code, day);
            out.push_back(std::move(choice));
        }
    }
    if (out.empty()) {
        out = default_stages();
        int day = game_day_of_week(now, server);
        for (auto& choice : out) choice.open = weekly_stage_open(choice.value, day);
    }
    return out;
}

inline void parse_activity_entry(StageCatalog& catalog, const std::string& key,
                                 const mj::Value& value, double now) {
    if (!value.is_object()) return;
    const mj::Value* info = value.find("Activity");
    if (!info || !info->is_object()) info = &value;
    StageActivity activity;
    activity.key = key;
    activity.tip = info->get_str("Tip", "");
    activity.stage_name = info->get_str("StageName", "");
    activity.resource_collection = info->get_bool("IsResourceCollection", false);
    int tz = activity_timezone(catalog.server, (int)info->get_int("TimeZone", 0));
    activity.start = parse_activity_time(info->get_str("UtcStartTime", ""), tz);
    activity.expire = parse_activity_time(info->get_str("UtcExpireTime", ""), tz);
    if (activity.start <= 0.0 || activity.expire <= 0.0) return;
    if (const auto* stages = info->find("Stages"); stages && stages->is_array()) {
        for (const auto& stage : stages->items()) {
            if (!stage.is_object()) continue;
            std::string code = stage.get_str("Value", stage.get_str("Display", ""));
            if (!valid_stage_code(code)) continue;
            StageChoice choice;
            choice.value = code;
            choice.display = stage.get_str("Display", code);
            choice.drop = stage.get_str("Drop", "");
            choice.activity = true;
            choice.resource_collection = activity.resource_collection;
            choice.start = activity.start;
            choice.expire = activity.expire;
            choice.outdated = now >= activity.expire;
            choice.open = now > activity.start && now < activity.expire;
            if (activity.resource_collection && now >= activity.expire) {
                choice.outdated = false;
                choice.open = weekly_stage_open(code, game_day_of_week(now, catalog.server));
            }
            activity.stages.push_back(choice);
        }
    }
    if (const auto* stages = value.find("Stages"); stages && stages->is_array()
        && activity.stages.empty()) {
        for (const auto& stage : stages->items()) {
            if (!stage.is_object()) continue;
            std::string code = stage.get_str("Value", stage.get_str("Display", ""));
            if (!valid_stage_code(code)) continue;
            StageChoice choice;
            choice.value = code;
            choice.display = stage.get_str("Display", code);
            choice.drop = stage.get_str("Drop", "");
            choice.activity = true;
            choice.resource_collection = activity.resource_collection;
            choice.start = activity.start;
            choice.expire = activity.expire;
            choice.outdated = now >= activity.expire;
            choice.open = now > activity.start && now < activity.expire;
            activity.stages.push_back(choice);
        }
    }
    catalog.activities.push_back(std::move(activity));
}

inline void attach_related_entries(StageCatalog& catalog, const mj::Value& server_root,
                                   double now) {
    const mj::Value* mini_games = server_root.find("miniGame");
    if (!mini_games || !mini_games->is_array()) return;
    for (const auto& item : mini_games->items()) {
        if (!item.is_object()) continue;
        std::string affiliation = item.get_str("Activity", "");
        if (affiliation.empty()) continue;
        int tz = activity_timezone(catalog.server, (int)item.get_int("TimeZone", 0));
        double start = parse_activity_time(item.get_str("UtcStartTime", ""), tz);
        double expire = parse_activity_time(item.get_str("UtcExpireTime", ""), tz);
        if (start <= 0.0 || expire <= 0.0 || now <= start || now >= expire) continue;
        for (auto& activity : catalog.activities) {
            if (activity.start <= 0.0 || now <= activity.start || now >= activity.expire)
                continue;
            if (affiliation != activity.stage_name && affiliation != activity.key
                && affiliation != activity.tip)
                continue;
            StageActivity::RelatedEntry entry;
            entry.value = item.get_str("Value", "");
            entry.display = item.get_str("Display", entry.value);
            entry.tip = item.get_str("Tip", "");
            if (entry.display.empty()) continue;
            bool duplicate = false;
            for (const auto& existing : activity.related_entries)
                if (existing.value == entry.value) duplicate = true;
            if (!duplicate) activity.related_entries.push_back(std::move(entry));
            break;
        }
    }
}

inline StageCatalog build(const mj::Value& stage_resource, const mj::Value& activity_root,
                          std::string server, double now) {
    StageCatalog catalog;
    catalog.server = normalize_server(server);
    catalog.stages = parse_stage_resource(stage_resource, now, catalog.server);
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < catalog.stages.size(); ++i) index[catalog.stages[i].value] = i;

    const mj::Value* server_root = activity_root.find(catalog.server);
    if (!server_root || !server_root->is_object()) server_root = activity_root.find("Official");
    if (server_root && server_root->is_object()) {
        if (const auto* side = server_root->find("sideStoryStage"); side && side->is_object()) {
            for (const auto& kv : side->members()) {
                parse_activity_entry(catalog, kv.first, kv.second, now);
            }
        }
        if (const auto* resource = server_root->find("resourceCollection");
            resource && resource->is_object()) {
            parse_activity_entry(catalog, "resourceCollection", *resource, now);
        }
        attach_related_entries(catalog, *server_root, now);
    }

    for (const auto& activity : catalog.activities) {
        for (const auto& choice : activity.stages) {
            auto it = index.find(choice.value);
            if (it == index.end()) {
                index[choice.value] = catalog.stages.size();
                catalog.stages.push_back(choice);
            } else {
                catalog.stages[it->second] = choice;
            }
        }
    }

    auto rank = [](const StageChoice& choice) {
        if (choice.activity && choice.open) return 0;
        if (choice.value == "1-7" || choice.value == "LS-6") return 1;
        if (!choice.activity && choice.open) return 2;
        if (choice.outdated) return 4;
        return 3;
    };
    std::stable_sort(catalog.stages.begin(), catalog.stages.end(), [&](const StageChoice& a,
                                                                       const StageChoice& b) {
        int ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : a.value < b.value;
    });
    return catalog;
}

inline std::vector<StageChoice> choices_for(const StageCatalog& catalog,
                                             const std::string& current,
                                             bool hide_unavailable) {
    std::vector<StageChoice> out;
    bool found = false;
    for (const auto& choice : catalog.stages) {
        if (hide_unavailable && !choice.open) continue;
        out.push_back(choice);
        if (choice.value == current) found = true;
    }
    if (!current.empty() && !found) {
        StageChoice custom;
        custom.value = current;
        custom.display = current;
        custom.open = false;
        out.push_back(std::move(custom));
    }
    return out;
}

inline bool activity_open(const StageActivity& activity, double now) {
    return activity.start > 0.0 && now > activity.start && now < activity.expire;
}

inline std::vector<std::string> today_activity_tips(const StageCatalog& catalog, double now) {
    std::vector<std::string> out;
    for (const auto& activity : catalog.activities) {
        if (!activity_open(activity, now)) continue;
        std::string title = activity.tip.empty() ? activity.stage_name : activity.tip;
        if (title.empty()) title = activity.key;
        if (!title.empty()) out.push_back("\xe9\x99\x90\xe6\x97\xb6 \xc2\xb7 " + title);
        for (const auto& stage : activity.stages) {
            std::string line = "  " + stage.display;
            if (!stage.drop.empty()) line += " \xc2\xb7 " + stage.drop;
            out.push_back(std::move(line));
        }
        for (const auto& entry : activity.related_entries) {
            std::string line = "  " + entry.display;
            if (!entry.tip.empty() && entry.tip.find('\n') == std::string::npos
                && entry.tip.size() <= 80)
                line += " \xc2\xb7 " + entry.tip;
            out.push_back(std::move(line));
        }
    }
    return out;
}

} // namespace hub::stage_catalog
