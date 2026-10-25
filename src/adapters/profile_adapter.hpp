#pragma once

#include "adapters/adapter.hpp"
#include "adapters/profile_runtime.hpp"
#include "core/build_profile_io.hpp"
#include "platform/fingerprint.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::adapters {

class ProfileAdapter final : public Adapter {
public:
    ProfileAdapter(
        std::filesystem::path image,
        std::filesystem::path profile_directory,
        EnvironmentState environment = EnvironmentState::unknown);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool probe() override;
    [[nodiscard]] BuildIdentity build() const override;
    [[nodiscard]] std::uint64_t capabilities() const noexcept override;
    [[nodiscard]] EnvironmentState environment_state() const noexcept override;
    [[nodiscard]] AdapterHealthSnapshot health_snapshot() const override;
    bool initialize() override;
    void shutdown() noexcept override;

    [[nodiscard]] bool profile_matched() const noexcept;
    [[nodiscard]] const std::optional<platform::FileFingerprint>& fingerprint() const noexcept;
    [[nodiscard]] const std::vector<core::BuildProfileError>& profile_errors() const noexcept;

private:
    void reset_probe_state();
    void set_profile_health(AnchorHealthState state, std::string reason);

    std::filesystem::path image_;
    std::filesystem::path profile_directory_;
    EnvironmentState requested_environment_{EnvironmentState::unknown};
    BuildIdentity build_{"unknown", 0};
    std::optional<platform::FileFingerprint> fingerprint_;
    std::optional<ProfileRuntimeTopology> topology_;
    AdapterHealthSnapshot health_;
    std::vector<core::BuildProfileError> profile_errors_;
    bool probed_{};
    bool matched_{};
    bool initialized_{};
};

} // namespace enrage::adapters
