#include "adapters/profile_adapter.hpp"

#include "core/build_profile_v3.hpp"
#include "platform/scan_image.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace enrage::adapters {
namespace {

std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) return std::nullopt;
    }
    return bytes;
}

std::uint64_t fingerprint_summary(const platform::FileFingerprint& fingerprint) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(fingerprint.sha256[index]) << (index * 8u);
    }
    return value;
}

} // namespace

ProfileAdapter::ProfileAdapter(
    std::filesystem::path image,
    std::filesystem::path profile_directory,
    EnvironmentState environment)
    : image_(std::move(image)),
      profile_directory_(std::move(profile_directory)),
      requested_environment_(environment) {}

std::string_view ProfileAdapter::name() const noexcept {
    return "profile";
}

void ProfileAdapter::reset_probe_state() {
    build_ = {"unknown", 0};
    fingerprint_.reset();
    topology_.reset();
    health_ = {};
    profile_errors_.clear();
    probed_ = false;
    matched_ = false;
    initialized_ = false;
}

void ProfileAdapter::set_profile_health(AnchorHealthState state, std::string reason) {
    health_.anchors.clear();
    health_.capabilities.clear();
    health_.anchors.push_back({"profile", state, std::move(reason)});
}

bool ProfileAdapter::probe() {
    reset_probe_state();

    const auto fingerprint = platform::fingerprint_file(image_);
    const auto bytes = read_bytes(image_);
    if (!fingerprint || !bytes) {
        set_profile_health(AnchorHealthState::missing, "host image is unreadable");
        return false;
    }

    fingerprint_ = *fingerprint;
    build_.fingerprint = fingerprint_summary(*fingerprint);

    const auto image = platform::ScanImage::from_file(*bytes);
    if (!image) {
        set_profile_health(AnchorHealthState::missing, "host image is not a supported PE image");
        return false;
    }

    auto imported = core::import_build_profiles_v3(profile_directory_);
    profile_errors_ = std::move(imported.errors);
    const auto* profile = imported.catalog.find(*fingerprint);
    probed_ = true;

    if (!profile) {
        set_profile_health(
            AnchorHealthState::missing,
            "no exact build profile matches host image fingerprint " + fingerprint->hex());
        return true;
    }

    build_.id = profile->id;
    auto resolver = imported.catalog.resolver_for(*fingerprint);
    auto topology = ProfileRuntimeTopology::from_profile(*profile);
    if (!resolver || !topology) {
        set_profile_health(AnchorHealthState::missing, "matched build profile has invalid runtime topology");
        return true;
    }

    if (!topology->evaluate(*resolver, *image)) {
        set_profile_health(AnchorHealthState::missing, "matched build profile could not evaluate all anchor bindings");
        return true;
    }

    topology_ = std::move(*topology);
    health_ = topology_->health_snapshot();
    health_.anchors.push_back({
        "profile",
        AnchorHealthState::resolved,
        "exact fingerprint matched build profile " + build_.id,
    });
    matched_ = true;
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

bool ProfileAdapter::profile_matched() const noexcept {
    return matched_;
}

const std::optional<platform::FileFingerprint>& ProfileAdapter::fingerprint() const noexcept {
    return fingerprint_;
}

const std::vector<core::BuildProfileError>& ProfileAdapter::profile_errors() const noexcept {
    return profile_errors_;
}

} // namespace enrage::adapters
