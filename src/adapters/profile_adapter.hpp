#pragma once

#include "adapters/adapter.hpp"
#include "adapters/image_provider.hpp"
#include "adapters/profile_runtime.hpp"
#include "core/build_profile_io.hpp"

#include <filesystem>
#include <memory>
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

    ProfileAdapter(
        std::unique_ptr<ImageProvider> image_provider,
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
    bool reload_support() override;

    [[nodiscard]] bool profile_matched() const noexcept;
    [[nodiscard]] const std::optional<platform::FileFingerprint>& fingerprint() const noexcept;
    [[nodiscard]] const std::vector<core::BuildProfileError>& profile_errors() const noexcept;
    [[nodiscard]] const ImageProvider& image_provider() const noexcept;

private:
    struct SupportState {
        BuildIdentity build{"unknown", std::nullopt};
        std::optional<platform::FileFingerprint> fingerprint;
        std::optional<ProfileRuntimeTopology> topology;
        AdapterHealthSnapshot health;
        std::vector<core::BuildProfileError> profile_errors;
        bool matched{};
    };

    [[nodiscard]] std::optional<SupportState> evaluate_support() const;
    void apply_support(SupportState state);
    static void set_profile_health(
        SupportState& state,
        AnchorHealthState health,
        std::string reason);

    std::unique_ptr<ImageProvider> image_provider_;
    std::filesystem::path profile_directory_;
    EnvironmentState requested_environment_{EnvironmentState::unknown};
    BuildIdentity build_{"unknown", std::nullopt};
    std::optional<platform::FileFingerprint> fingerprint_;
    std::optional<ProfileRuntimeTopology> topology_;
    AdapterHealthSnapshot health_;
    std::vector<core::BuildProfileError> profile_errors_;
    bool probed_{};
    bool matched_{};
    bool initialized_{};
};

} // namespace enrage::adapters
