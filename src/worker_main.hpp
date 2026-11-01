// worker 入口声明
#pragma once
#include "json.hpp"

namespace hub {
int worker_main(const std::string& kind, const mj::Value& cfg);
}
