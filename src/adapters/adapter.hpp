#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace enrage::adapters {

struct BuildIdentity {
    std::string id;
    std::uint64_t fingerprint{};
};

enum class EnvironmentState {
    unknown,
    local,
    restricted,
};

class Adapter {
public:
    virtual ~Adapter() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool probe() = 0;
    [[nodiscard]] virtual BuildIdentity build() const = 0;
    [[nodiscard]] virtual std::uint64_t capabilities() const noexcept = 0;
    [[nodiscard]] virtual EnvironmentState environment_state() const noexcept = 0;

    virtual bool initialize() = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace enrage::adapters
