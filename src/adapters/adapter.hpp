#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

enum class AnchorHealthState {
    unresolved,
    resolved,
    degraded,
    missing,
};

struct AnchorHealth {
    std::string name;
    AnchorHealthState state{AnchorHealthState::unresolved};
    std::string reason;
};

struct CapabilityHealth {
    std::uint64_t capability{};
    bool advertised{};
    bool available{};
    std::vector<std::string> required_anchors;
    std::vector<std::string> blocking_anchors;
    std::vector<std::string> degraded_anchors;
};

struct AdapterHealthSnapshot {
    std::vector<AnchorHealth> anchors;
    std::vector<CapabilityHealth> capabilities;
};

class Adapter {
public:
    virtual ~Adapter() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool probe() = 0;
    [[nodiscard]] virtual BuildIdentity build() const = 0;
    [[nodiscard]] virtual std::uint64_t capabilities() const noexcept = 0;
    [[nodiscard]] virtual EnvironmentState environment_state() const noexcept = 0;
    [[nodiscard]] virtual AdapterHealthSnapshot health_snapshot() const { return {}; }

    virtual bool initialize() = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace enrage::adapters
