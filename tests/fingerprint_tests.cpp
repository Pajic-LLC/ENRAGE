#include "core/build_catalog.hpp"
#include "platform/fingerprint.hpp"
#include "platform/signature/pattern.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-fingerprint-test";
    const auto fixture = root / "fixture.bin";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream{fixture, std::ios::binary} << "abc";

    const auto fingerprint = enrage::platform::fingerprint_file(fixture);
    assert(fingerprint.has_value());
    assert(fingerprint->size == 3);
    assert(fingerprint->hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    auto pattern = enrage::platform::signature::Pattern::parse("61 62 63");
    assert(pattern.has_value());

    enrage::core::BuildCatalog catalog;
    assert(catalog.add({
        .id = "fixture-build",
        .fingerprint = *fingerprint,
        .capabilities = 7,
        .symbols = {{"fixture.symbol", {{std::move(*pattern), 0}}}},
    }));
    assert(catalog.size() == 1);
    assert(!catalog.add({.id = "duplicate", .fingerprint = *fingerprint}));

    const auto* profile = catalog.find(*fingerprint);
    assert(profile != nullptr);
    assert(profile->id == "fixture-build");
    assert(profile->capabilities == 7);

    auto resolver = catalog.resolver_for(*fingerprint);
    assert(resolver.has_value());
    const std::array bytes{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
    const auto resolved = resolver->resolve("fixture.symbol", bytes);
    assert(resolved.state == enrage::platform::signature::ResolutionState::unique);
    assert(resolved.offset == 0);

    auto unknown = *fingerprint;
    unknown.sha256[0] ^= 0xffu;
    assert(catalog.find(unknown) == nullptr);
    assert(!catalog.resolver_for(unknown).has_value());

    std::filesystem::remove_all(root);
    return 0;
}
