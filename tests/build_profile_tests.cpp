#include "core/build_profile_io.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr auto kSha = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

std::string profile_json(std::string id, std::string sha = kSha, std::uint64_t size = 4096) {
    return "{"
           "\"version\":1,"
           "\"id\":\"" + id + "\","
           "\"fingerprint\":{\"sha256\":\"" + sha + "\",\"size\":" + std::to_string(size) + "},"
           "\"capabilities\":5,"
           "\"symbols\":[{"
             "\"name\":\"ExampleSymbol\","
             "\"candidates\":["
               "{\"pattern\":\"48 8B ?? 89\",\"adjustment\":0},"
               "{\"pattern\":\"AA BB CC\",\"adjustment\":-5}"
             "]"
           "}]"
           "}";
}

void parses_strict_profile_and_builds_resolver() {
    const auto parsed = enrage::core::parse_build_profile(profile_json("example.build"));
    assert(parsed.ok());
    assert(parsed.profile->id == "example.build");
    assert(parsed.profile->fingerprint.size == 4096);
    assert(parsed.profile->fingerprint.hex() == kSha);
    assert(parsed.profile->capabilities == 5);
    assert(parsed.profile->symbols.size() == 1);
    assert(parsed.profile->symbols[0].region.empty());

    enrage::core::BuildCatalog catalog;
    assert(catalog.add(*parsed.profile));
    const auto resolver = catalog.resolver_for(parsed.profile->fingerprint);
    assert(resolver.has_value());

    const std::array bytes{
        std::byte{0x90}, std::byte{0x48}, std::byte{0x8B},
        std::byte{0x12}, std::byte{0x89}, std::byte{0x90},
    };
    const auto resolved = resolver->resolve("ExampleSymbol", bytes);
    assert(resolved.state == enrage::platform::signature::ResolutionState::unique);
    assert(resolved.offset == 1);
}

void parses_v2_symbol_region() {
    const auto parsed = enrage::core::parse_build_profile(
        "{\"version\":2,\"id\":\"example.v2\","
        "\"fingerprint\":{\"sha256\":\"" + std::string(kSha) + "\",\"size\":4096},"
        "\"capabilities\":5,\"symbols\":[{"
        "\"name\":\"ExampleSymbol\",\"region\":\".text\","
        "\"candidates\":[{\"pattern\":\"48 8B ?? 89\",\"adjustment\":0}]}]}"
    );
    assert(parsed.ok());
    assert(parsed.profile->symbols.size() == 1);
    assert(parsed.profile->symbols[0].region == ".text");

    const auto legacy_with_region = enrage::core::parse_build_profile(
        "{\"version\":1,\"id\":\"legacy.region\","
        "\"fingerprint\":{\"sha256\":\"" + std::string(kSha) + "\",\"size\":4096},"
        "\"capabilities\":0,\"symbols\":[{"
        "\"name\":\"ExampleSymbol\",\"region\":\".text\","
        "\"candidates\":[{\"pattern\":\"48 8B ?? 89\",\"adjustment\":0}]}]}"
    );
    assert(!legacy_with_region.ok());
}

void rejects_malformed_or_forward_unknown_profiles() {
    auto bad_sha = profile_json("bad.sha", "xyz");
    assert(!enrage::core::parse_build_profile(bad_sha).ok());

    const auto unknown = enrage::core::parse_build_profile(
        "{\"version\":1,\"id\":\"x\",\"fingerprint\":{\"sha256\":\"" +
        std::string(kSha) + "\",\"size\":1},\"capabilities\":0,\"symbols\":[],\"future\":true}");
    assert(!unknown.ok());

    const auto future = enrage::core::parse_build_profile(
        "{\"version\":3,\"id\":\"x\",\"fingerprint\":{\"sha256\":\"" +
        std::string(kSha) + "\",\"size\":1},\"capabilities\":0,\"symbols\":[]}");
    assert(!future.ok());
}

void imports_enp_directory_deterministically() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-import-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::ofstream{root / "a.enp"} << profile_json("build.a", kSha, 100);
    std::ofstream{root / "b.enp"} << profile_json(
        "build.b",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        200);
    std::ofstream{root / "ignored.txt"} << "not a profile";

    auto imported = enrage::core::import_build_profiles(root);
    assert(imported.errors.empty());
    assert(imported.catalog.size() == 2);

    std::ofstream{root / "duplicate.enp"} << profile_json("build.a", kSha, 100);
    imported = enrage::core::import_build_profiles(root);
    assert(imported.catalog.size() == 2);
    assert(imported.errors.size() == 1);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    parses_strict_profile_and_builds_resolver();
    parses_v2_symbol_region();
    rejects_malformed_or_forward_unknown_profiles();
    imports_enp_directory_deterministically();
    return 0;
}
