#include "adapters/profile_adapter.hpp"

#include "core/build_profile_v3.hpp"

#include <utility>

namespace enrage::adapters {

ProfileAdapter::ProfileAdapter(
    std::filesystem::path image,
    std::filesystem::path profile_directory,
    EnvironmentState environment)
    : ProfileAdapter(
          std::make_unique<FileImageProvider>(std::move(image)),
          std::move(profile_directory),
          environment) {}

ProfileAdapter::ProfileAdapter(
    std::unique_ptr<ImageProvider> image_provider,
    std::filesystem::path profile_directory,
    EnvironmentState environment)
    : image_provider_(std::move(image_provider)),
      profile_directory_(std::move(profile_directory)),
      requested_environment_(environment) {}

std::string_view ProfileAdapter::name() const noexcept {
    return "profile";
}

void ProfileAdapter::set_profile_health(
    SupportState& state,
    AnchorHealthState health,
    std::string reason) {
    state.health.anchors.clear();
    state.health.capabilities.clear();
    state.health.anchors.push_back({"profile", health, std::move(reason)});
}

std::optional<ProfileAdapter::SupportState> ProfileAdapter::evaluate_support() const {
    if (!image_provider_) return std::nullopt;

    const auto fingerprint = image_provider_->identity();
    const auto image = image_provider_->scan_image();
    if (!fingerprint || !image) return std::nullopt;

    SupportState state;
    state.fingerprint = *fingerprint;
    state.build.fingerprint = *fingerprint;

    auto imported = core::import_build_profiles_v3(profile_directory_);
    state.profile_errors = std::move(imported.errors);
    const auto* profile = imported.catalog.find(*fingerprint);
    if (!profile) {
        set_profile_health(
            state,
            AnchorHealthState::missing,
            "no exact build profile matches host image fingerprint " + fingerprint->hex());
        return state;
    }

    state.build.id = profile->id;
    auto resolver = imported.catalog.resolver_for(*fingerprint);
    auto topology = ProfileRuntimeTopology::from_profile(*profile);
    if (!resolver || !topology) {
        set_profile_health(
            state,
            AnchorHealthState::missing,
            "matched build profile has invalid runtime topology");
        return state;
    }

    if (!topology->evaluate(*resolver, *image)) {
        set_profile_health(
            state,
            AnchorHealthState::missing,
            "matched build profile could not evaluate all anchor bindings");
        return state;
    }

    state.topology = std::move(*topology);
    state.health = state.topology->health_snapshot();
    state.health.anchors.push_back({
        "profile",
        AnchorHealthState::resolved,
        "exact fingerprint matched build profile " + state.build.id,
    });
    state.matched = true;
    return state;
}

void ProfileAdapter::apply_support(SupportState state) {
    build_ = std::move(state.build);
    fingerprint_ = std::move(state.fingerprint);
    topology_ = std::move(state.topology);
    health_ = std::move(state.health);
    profile_errors_ = std::move(state.profile_errors);
    matched_ = state.matched;
    probed_ = true;
}

bool ProfileAdapter::probe() {
    build_ = {"unknown", std::nullopt};
    fingerprint_.reset();
    topology_.reset();
    health_ = {};
    profile_errors_.clear();
    probed_ = false;
    matched_ = false;
    initialized_ = false;

    auto state = evaluate_support();
    if (!state) return false;
    apply_support(std::move(*state));
    return true;
}

BuildIdentity ProfileAdapter::build() const {
    return build_;
}

std::uint64_t ProfileAdapter::capabilities() const noexcept {
    return topology_ ? topology_->capabilities() : 0;
}

EnvironmentState ProfileAdapter::environment_state() const noexcept {
    return matched_ ? requested_environment_ : EnvironmentState::unknown;
}

AdapterHealthSnapshot ProfileAdapter::health_snapshot() const {
    return health_;
}

bool ProfileAdapter::initialize() {
    if (!probed_) return false;
    initialized_ = true;
    return true;
}

void ProfileAdapter::shutdown() noexcept {
    initialized_ = false;
}

bool ProfileAdapter::reload_support() {
    if (!probed_) return false;

    auto candidate = evaluate_support();
    if (!candidate) return false;

    const bool same_identity = build_.fingerprint == candidate->build.fingerprint;
    if (matched_ && same_identity && !candidate->matched) {
        // The live topology was already verified against this exact image. A
        // malformed, incomplete, or removed replacement profile must not tear
        // down that known-good in-memory state mid-session.
        profile_errors_ = std::move(candidate->profile_errors);
        return false;
    }

    apply_support(std::move(*candidate));
    return true;
}

bool ProfileAdapter::profile_matched() const noexcept {
    return matched_;
}

const std::optional<platform::FileFingerprint>& ProfileAdapter::fingerprint() const noexcept {
    return fingerprint_;
}

const std::vector<core::BuildProfileError>& ProfileAdapter::profile_errors() const noexcept {
    return profile_errors_;
}

const ImageProvider& ProfileAdapter::image_provider() const noexcept {
    return *image_provider_;
}

} // namespace enrage::adapters
