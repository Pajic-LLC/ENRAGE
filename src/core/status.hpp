#pragma once

#include <string>

namespace enrage::core {

class Runtime;

[[nodiscard]] std::string runtime_status_json(const Runtime& runtime);

} // namespace enrage::core
