#include "core/profile_authoring.hpp"

#include "core/build_profile_v3.hpp"
#include "platform/fingerprint.hpp"
#include "platform/scan_image.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <ranges>
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
    std::ostringstream output;
    output << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    return output.str();
}

std::vector<std::filesystem::path> sorted_files(
    const std::filesystem::path& directory,
    std::string_view extension) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return files;

    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || error) continue;
        if (extension.empty() || it->path().extension() == extension) files.push_back(it->path());
    }
    std::ranges::sort(files);
    return files;
}

struct ImageFixture {
    std::filesystem::path path;
    platform::FileFingerprint fingerprint;
    std::vector<std::byte> bytes;
};

ValidationSymbol resolve_symbol(
    const platform::signature::SymbolSpec& symbol,
    const platform::signature::Resolver& resolver,
    const std::vector<std::byte>& bytes) {
    if (symbol.region.empty()) {
        const auto resolution = resolver.resolve(symbol.name, bytes);
        return {symbol.name, resolution.state, resolution.match_count, resolution.offset};
    }

    const auto image = platform::ScanImage::from_file(bytes);
    if (!image) {
        return {symbol.name, platform::signature::ResolutionState::not_found, 0, std::nullopt};
    }

    const auto resolution = platform::resolve_in_image(resolver, symbol.name, *image);
    return {symbol.name, resolution.state, resolution.match_count, resolution.storage_offset};
}

} // namespace

std::string serialize_profile_draft(const ProfileDraft& profile) {
    BuildProfile build;
    build.id = profile.id;
    build.fingerprint = profile.fingerprint;
    build.capabilities = profile.capabilities;
    build.anchors = profile.anchors;
    build.requirements = profile.requirements;
    build.symbols.reserve(profile.symbols.size());

    for (const auto& draft : profile.symbols) {
        auto pattern = platform::signature::Pattern::parse(draft.pattern);
        if (!pattern) return {};
        platform::signature::SymbolSpec symbol;
        symbol.name = draft.name;
        symbol.region = draft.section.value_or(std::string{});
        symbol.candidates.push_back({std::move(*pattern), draft.adjustment});
        build.symbols.push_back(std::move(symbol));
    }
    return serialize_build_profile_v3(build);
}

ValidationReport validate_profile_set_strict(
    const std::filesystem::path& profile_directory,
    const std::filesystem::path& image_directory) {
    ValidationReport report;

    std::vector<ImageFixture> images;
    for (const auto& file : sorted_files(image_directory, {})) {
        const auto extension = file.extension().string();
        if (extension != ".exe" && extension != ".dll") continue;

        const auto fingerprint = platform::fingerprint_file(file);
        const auto bytes = read_bytes(file);
        if (!fingerprint || !bytes) {
            report.errors.push_back({file, "image is unreadable"});
            continue;
        }
        images.push_back({file, *fingerprint, std::move(*bytes)});
    }
    report.images_scanned = images.size();

    const auto profiles = sorted_files(profile_directory, ".enp");
    report.profiles_scanned = profiles.size();
    for (const auto& profile_file : profiles) {
        const auto text = read_text(profile_file);
        if (!text) {
            report.errors.push_back({profile_file, "profile is unreadable"});
            continue;
        }

        auto parsed = parse_build_profile_v3(*text, profile_file);
        if (!parsed.ok()) {
            report.errors.push_back({
                profile_file,
                parsed.error ? parsed.error->message : "invalid profile",
            });
            continue;
        }

        const auto& profile = *parsed.profile;
        ValidationBuild build{
            .profile_source = profile_file,
            .profile_id = profile.id,
        };

        std::vector<const ImageFixture*> matching;
        for (const auto& image : images) {
            if (image.fingerprint == profile.fingerprint) matching.push_back(&image);
        }
        if (matching.size() != 1) {
            report.errors.push_back({
                profile_file,
                matching.empty() ? "no exact image matches the profile fingerprint"
                                 : "multiple images match the profile fingerprint",
            });
            report.builds.push_back(std::move(build));
            continue;
        }

        const auto& image = *matching.front();
        build.image = image.path;
        build.fingerprint_matched = true;

        platform::signature::Resolver resolver;
        bool resolver_valid = true;
        for (const auto& symbol : profile.symbols) {
            if (!resolver.add(symbol)) {
                resolver_valid = false;
                report.errors.push_back({profile_file, "profile could not be loaded into the symbol resolver"});
                break;
            }
        }
        if (!resolver_valid) {
            report.builds.push_back(std::move(build));
            continue;
        }

        bool all_unique = true;
        for (const auto& symbol : profile.symbols) {
            auto resolution = resolve_symbol(symbol, resolver, image.bytes);
            if (resolution.state != platform::signature::ResolutionState::unique) all_unique = false;
            build.symbols.push_back(std::move(resolution));
        }
        build.valid = all_unique;
        report.builds.push_back(std::move(build));
    }

    return report;
}

} // namespace enrage::core
