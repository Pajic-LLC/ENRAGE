#include "core/profile_propagation.hpp"

#include "core/build_profile_io.hpp"
#include "platform/fingerprint.hpp"
#include "platform/signature/resolver.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace enrage::core {
namespace {

std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    const auto size = static_cast<std::size_t>(end);
    std::vector<std::byte> bytes(size);
    input.seekg(0, std::ios::beg);
    if (size != 0) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input && size != 0) return std::nullopt;
    return bytes;
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream out;
    out << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    return out.str();
}

} // namespace

AuthoringResult propagate_profile(
    const std::filesystem::path& reference_image,
    const std::filesystem::path& target_image,
    const std::filesystem::path& reference_profile,
    std::string target_profile_id,
    AuthoringOptions options) {
    AuthoringResult result;

    const auto profile_text = read_text(reference_profile);
    if (!profile_text) {
        result.errors.push_back({{}, "reference profile is unreadable"});
        return result;
    }
    auto parsed = parse_build_profile(*profile_text, reference_profile);
    if (!parsed.ok()) {
        result.errors.push_back({{}, parsed.error ? parsed.error->message : "reference profile is invalid"});
        return result;
    }

    const auto reference_fingerprint = platform::fingerprint_file(reference_image);
    const auto reference_bytes = read_bytes(reference_image);
    if (!reference_fingerprint || !reference_bytes) {
        result.errors.push_back({{}, "reference image is unreadable"});
        return result;
    }

    const auto& profile = *parsed.profile;
    if (!(profile.fingerprint == *reference_fingerprint)) {
        result.errors.push_back({{}, "reference profile fingerprint does not match the reference image"});
        return result;
    }

    platform::signature::Resolver resolver;
    for (const auto& symbol : profile.symbols) {
        if (!resolver.add(symbol)) {
            result.errors.push_back({symbol.name, "reference symbol could not be loaded into the resolver"});
        }
    }
    if (!result.errors.empty()) return result;

    std::vector<SymbolSeed> seeds;
    seeds.reserve(profile.symbols.size());
    for (const auto& symbol : profile.symbols) {
        const auto resolution = resolver.resolve(symbol.name, *reference_bytes);
        if (resolution.state != platform::signature::ResolutionState::unique || !resolution.offset) {
            result.errors.push_back({symbol.name, "reference symbol is not uniquely resolvable"});
            continue;
        }
        seeds.push_back({symbol.name, *resolution.offset});
    }
    if (!result.errors.empty()) return result;

    return draft_profile(
        reference_image,
        target_image,
        std::move(target_profile_id),
        seeds,
        profile.capabilities,
        options);
}

} // namespace enrage::core
