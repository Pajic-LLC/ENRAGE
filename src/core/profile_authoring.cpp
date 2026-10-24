#include "core/profile_authoring.hpp"

#include "core/build_profile_io.hpp"
#include "platform/pe_image.hpp"
#include "platform/signature/pattern.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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

std::string escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char ch : input) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::ostringstream encoded;
                    encoded << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(static_cast<unsigned char>(ch));
                    out += encoded.str();
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

std::string hex_byte(std::byte value) {
    constexpr char digits[] = "0123456789ABCDEF";
    const auto byte = std::to_integer<unsigned>(value);
    std::string out(2, '0');
    out[0] = digits[(byte >> 4) & 0xfu];
    out[1] = digits[byte & 0xfu];
    return out;
}

bool low_information(std::span<const std::byte> seed) {
    std::array<bool, 256> seen{};
    std::size_t distinct{};
    for (const auto value : seed) {
        const auto index = std::to_integer<unsigned char>(value);
        if (!seen[index]) {
            seen[index] = true;
            ++distinct;
        }
    }
    return distinct < 3;
}

struct SearchRange {
    std::size_t begin{};
    std::size_t end{};
    std::optional<std::string> section;
};

const platform::PeSection* containing_section(const platform::PeImage& image, std::size_t offset) {
    for (const auto& section : image.sections()) {
        const auto begin = static_cast<std::size_t>(section.raw_offset);
        const auto size = static_cast<std::size_t>(section.raw_size);
        if (offset >= begin && offset - begin < size) return &section;
    }
    return nullptr;
}

SearchRange target_search_range(
    std::span<const std::byte> target,
    const std::optional<platform::PeImage>& reference_pe,
    const std::optional<platform::PeImage>& target_pe,
    std::size_t reference_offset) {
    SearchRange range{0, target.size(), std::nullopt};
    if (!reference_pe || !target_pe) return range;

    const auto* reference_section = containing_section(*reference_pe, reference_offset);
    if (!reference_section) return range;
    const auto* target_section = target_pe->find_section(reference_section->name);
    if (!target_section) return range;

    const auto begin = static_cast<std::size_t>(target_section->raw_offset);
    const auto size = static_cast<std::size_t>(target_section->raw_size);
    if (begin > target.size() || size > target.size() - begin) return range;
    return {begin, begin + size, reference_section->name};
}

std::vector<std::size_t> exact_matches(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle,
    std::size_t absolute_base,
    std::size_t limit = 256) {
    std::vector<std::size_t> matches;
    if (needle.empty() || needle.size() > haystack.size()) return matches;

    auto cursor = haystack.begin();
    while (cursor != haystack.end() && matches.size() < limit) {
        const auto found = std::search(cursor, haystack.end(), needle.begin(), needle.end());
        if (found == haystack.end()) break;
        matches.push_back(absolute_base + static_cast<std::size_t>(found - haystack.begin()));
        cursor = found + 1;
    }
    return matches;
}

struct CandidateLocation {
    std::size_t start{};
    std::size_t votes{};
    double similarity{};
};

std::vector<CandidateLocation> locate_candidates(
    std::span<const std::byte> reference,
    std::span<const std::byte> target,
    std::size_t window_start,
    std::size_t window_size,
    SearchRange target_range,
    std::size_t requested_seed_size) {
    std::unordered_map<std::size_t, std::size_t> votes;
    const auto seed_size = std::min(requested_seed_size, window_size);
    if (seed_size < 4 || target_range.begin > target_range.end || target_range.end > target.size()) return {};

    const auto step = std::max<std::size_t>(1, seed_size / 2);
    const auto target_span = target.subspan(target_range.begin, target_range.end - target_range.begin);

    for (std::size_t position = 0; position + seed_size <= window_size; position += step) {
        const auto seed = reference.subspan(window_start + position, seed_size);
        if (low_information(seed)) continue;
        const auto matches = exact_matches(target_span, seed, target_range.begin);
        for (const auto match : matches) {
            if (match < position) continue;
            const auto candidate_start = match - position;
            if (candidate_start < target_range.begin) continue;
            if (window_size > target_range.end - candidate_start) continue;
            ++votes[candidate_start];
        }
    }

    std::vector<CandidateLocation> candidates;
    candidates.reserve(votes.size());
    for (const auto& [start, count] : votes) {
        std::size_t equal{};
        for (std::size_t i = 0; i < window_size; ++i) {
            if (reference[window_start + i] == target[start + i]) ++equal;
        }
        candidates.push_back({start, count, static_cast<double>(equal) / static_cast<double>(window_size)});
    }

    std::ranges::sort(candidates, [](const CandidateLocation& left, const CandidateLocation& right) {
        if (left.votes != right.votes) return left.votes > right.votes;
        if (std::abs(left.similarity - right.similarity) > 0.000001) return left.similarity > right.similarity;
        return left.start < right.start;
    });
    if (candidates.size() > 64) candidates.resize(64);
    return candidates;
}

std::optional<SignatureDraft> make_draft(
    std::string name,
    std::span<const std::byte> reference,
    std::span<const std::byte> target,
    std::size_t reference_offset,
    std::size_t window_start,
    std::size_t window_size,
    const CandidateLocation& candidate,
    std::optional<std::string> section,
    double minimum_similarity) {
    if (candidate.similarity < minimum_similarity) return std::nullopt;

    std::vector<bool> stable(window_size);
    std::size_t exact{};
    for (std::size_t i = 0; i < window_size; ++i) {
        stable[i] = reference[window_start + i] == target[candidate.start + i];
        if (stable[i]) ++exact;
    }
    if (exact < 12) return std::nullopt;

    std::size_t first{};
    while (first < stable.size() && !stable[first]) ++first;
    if (first == stable.size()) return std::nullopt;
    std::size_t last = stable.size() - 1;
    while (last > first && !stable[last]) --last;

    std::ostringstream pattern_text;
    std::size_t trimmed_exact{};
    std::size_t trimmed_wildcards{};
    for (std::size_t i = first; i <= last; ++i) {
        if (i != first) pattern_text << ' ';
        if (stable[i]) {
            pattern_text << hex_byte(reference[window_start + i]);
            ++trimmed_exact;
        } else {
            pattern_text << "??";
            ++trimmed_wildcards;
        }
    }

    if (trimmed_exact < 12) return std::nullopt;
    auto pattern = platform::signature::Pattern::parse(pattern_text.str());
    if (!pattern) return std::nullopt;

    const auto reference_matches = pattern->find_all(reference);
    const auto target_matches = pattern->find_all(target);
    if (reference_matches.size() != 1 || target_matches.size() != 1) return std::nullopt;

    const auto expected_reference_match = window_start + first;
    const auto expected_target_match = candidate.start + first;
    if (reference_matches.front() != expected_reference_match || target_matches.front() != expected_target_match) {
        return std::nullopt;
    }

    const auto symbol_from_window = reference_offset - window_start;
    const auto adjustment_signed = static_cast<std::ptrdiff_t>(symbol_from_window) - static_cast<std::ptrdiff_t>(first);
    const auto target_offset_signed = static_cast<std::ptrdiff_t>(candidate.start) +
                                      static_cast<std::ptrdiff_t>(symbol_from_window);
    if (target_offset_signed < 0 || static_cast<std::size_t>(target_offset_signed) >= target.size()) return std::nullopt;

    return SignatureDraft{
        .name = std::move(name),
        .reference_offset = reference_offset,
        .target_offset = static_cast<std::size_t>(target_offset_signed),
        .pattern = pattern_text.str(),
        .adjustment = adjustment_signed,
        .similarity = candidate.similarity,
        .exact_bytes = trimmed_exact,
        .wildcard_bytes = trimmed_wildcards,
        .reference_match_count = reference_matches.size(),
        .target_match_count = target_matches.size(),
        .section = std::move(section),
    };
}

std::optional<SignatureDraft> draft_symbol(
    const SymbolSeed& symbol,
    std::span<const std::byte> reference,
    std::span<const std::byte> target,
    const std::optional<platform::PeImage>& reference_pe,
    const std::optional<platform::PeImage>& target_pe,
    AuthoringOptions options) {
    if (symbol.name.empty() || symbol.reference_offset >= reference.size()) return std::nullopt;
    if (options.minimum_radius == 0 || options.maximum_radius < options.minimum_radius || options.seed_size < 4) {
        return std::nullopt;
    }

    const auto search_range = target_search_range(
        target,
        reference_pe,
        target_pe,
        symbol.reference_offset);

    std::optional<SignatureDraft> best;
    for (std::size_t radius = options.minimum_radius; radius <= options.maximum_radius; radius += 8) {
        const auto window_start = symbol.reference_offset > radius ? symbol.reference_offset - radius : 0;
        const auto window_end = std::min(reference.size(), symbol.reference_offset + radius + 1);
        if (window_end <= window_start) continue;
        const auto window_size = window_end - window_start;
        if (window_size < options.seed_size) continue;

        auto candidates = locate_candidates(
            reference,
            target,
            window_start,
            window_size,
            search_range,
            options.seed_size);
        for (const auto& candidate : candidates) {
            auto draft = make_draft(
                symbol.name,
                reference,
                target,
                symbol.reference_offset,
                window_start,
                window_size,
                candidate,
                search_range.section,
                options.minimum_similarity);
            if (!draft) continue;
            if (!best || draft->exact_bytes > best->exact_bytes ||
                (draft->exact_bytes == best->exact_bytes && draft->similarity > best->similarity)) {
                best = std::move(draft);
            }
        }

        if (best && best->exact_bytes >= 24 && best->similarity >= 0.75) break;
        if (radius > options.maximum_radius - std::min<std::size_t>(8, options.maximum_radius)) break;
    }
    return best;
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

std::string state_name(platform::signature::ResolutionState state) {
    using platform::signature::ResolutionState;
    switch (state) {
        case ResolutionState::unknown_symbol: return "unknown_symbol";
        case ResolutionState::not_found: return "not_found";
        case ResolutionState::unique: return "unique";
        case ResolutionState::ambiguous: return "ambiguous";
    }
    return "unknown_symbol";
}

} // namespace

AuthoringResult draft_profile(
    const std::filesystem::path& reference_image,
    const std::filesystem::path& target_image,
    std::string profile_id,
    const std::vector<SymbolSeed>& symbols,
    std::uint64_t capabilities,
    AuthoringOptions options) {
    AuthoringResult result;
    if (profile_id.empty()) {
        result.errors.push_back({{}, "profile id must not be empty"});
        return result;
    }
    if (symbols.empty()) {
        result.errors.push_back({{}, "at least one symbol is required"});
        return result;
    }

    const auto reference_bytes = read_bytes(reference_image);
    const auto target_bytes = read_bytes(target_image);
    const auto target_fingerprint = platform::fingerprint_file(target_image);
    if (!reference_bytes) {
        result.errors.push_back({{}, "reference image is unreadable"});
        return result;
    }
    if (!target_bytes || !target_fingerprint) {
        result.errors.push_back({{}, "target image is unreadable"});
        return result;
    }

    const auto reference_pe = platform::PeImage::parse(*reference_bytes);
    const auto target_pe = platform::PeImage::parse(*target_bytes);

    ProfileDraft profile{
        .id = std::move(profile_id),
        .fingerprint = *target_fingerprint,
        .capabilities = capabilities,
    };
    profile.symbols.reserve(symbols.size());

    std::set<std::string> names;
    for (const auto& symbol : symbols) {
        if (symbol.name.empty() || !names.insert(symbol.name).second) {
            result.errors.push_back({symbol.name, "symbol names must be non-empty and unique"});
            continue;
        }
        if (symbol.reference_offset >= reference_bytes->size()) {
            result.errors.push_back({symbol.name, "reference offset is outside the image"});
            continue;
        }
        auto draft = draft_symbol(
            symbol,
            *reference_bytes,
            *target_bytes,
            reference_pe,
            target_pe,
            options);
        if (!draft) {
            result.errors.push_back({symbol.name, "could not derive a unique cross-build signature"});
            continue;
        }
        profile.symbols.push_back(std::move(*draft));
    }

    if (!result.errors.empty() || profile.symbols.size() != symbols.size()) return result;

    const auto json = profile_draft_json(profile);
    const auto parsed = parse_build_profile(json);
    if (!parsed.ok()) {
        result.errors.push_back({{}, "generated profile failed self-validation"});
        return result;
    }
    result.profile = std::move(profile);
    return result;
}

std::string profile_draft_json(const ProfileDraft& profile) {
    std::ostringstream out;
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"id\": \"" << escape_json(profile.id) << "\",\n"
        << "  \"fingerprint\": {\"sha256\": \"" << profile.fingerprint.hex()
        << "\", \"size\": " << profile.fingerprint.size << "},\n"
        << "  \"capabilities\": " << profile.capabilities << ",\n"
        << "  \"symbols\": [\n";

    for (std::size_t i = 0; i < profile.symbols.size(); ++i) {
        const auto& symbol = profile.symbols[i];
        out << "    {\"name\": \"" << escape_json(symbol.name) << "\", \"candidates\": ["
            << "{\"pattern\": \"" << symbol.pattern << "\", \"adjustment\": "
            << symbol.adjustment << "}]}";
        if (i + 1 != profile.symbols.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

bool ValidationReport::ok() const noexcept {
    return errors.empty() && std::ranges::all_of(builds, [](const ValidationBuild& build) { return build.valid; });
}

ValidationReport validate_profile_set(
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
        auto parsed = parse_build_profile(*text, profile_file);
        if (!parsed.ok()) {
            report.errors.push_back({profile_file, parsed.error ? parsed.error->message : "invalid profile"});
            continue;
        }

        auto& profile = *parsed.profile;
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
            const auto resolution = resolver.resolve(symbol.name, image.bytes);
            build.symbols.push_back({symbol.name, resolution.state, resolution.match_count, resolution.offset});
            if (resolution.state != platform::signature::ResolutionState::unique) all_unique = false;
        }
        build.valid = all_unique;
        report.builds.push_back(std::move(build));
    }

    return report;
}

std::string validation_report_json(const ValidationReport& report) {
    std::ostringstream out;
    out << "{\n"
        << "  \"ok\": " << (report.ok() ? "true" : "false") << ",\n"
        << "  \"profilesScanned\": " << report.profiles_scanned << ",\n"
        << "  \"imagesScanned\": " << report.images_scanned << ",\n"
        << "  \"errors\": [";
    for (std::size_t i = 0; i < report.errors.size(); ++i) {
        if (i != 0) out << ',';
        out << "{\"source\":\"" << escape_json(report.errors[i].source.generic_string())
            << "\",\"message\":\"" << escape_json(report.errors[i].message) << "\"}";
    }
    out << "],\n  \"builds\": [\n";

    for (std::size_t i = 0; i < report.builds.size(); ++i) {
        const auto& build = report.builds[i];
        out << "    {\"profile\":\"" << escape_json(build.profile_id) << "\","
            << "\"profileSource\":\"" << escape_json(build.profile_source.generic_string()) << "\","
            << "\"image\":";
        if (build.image) out << "\"" << escape_json(build.image->generic_string()) << "\"";
        else out << "null";
        out << ",\"fingerprintMatched\":" << (build.fingerprint_matched ? "true" : "false")
            << ",\"valid\":" << (build.valid ? "true" : "false")
            << ",\"symbols\":[";
        for (std::size_t s = 0; s < build.symbols.size(); ++s) {
            const auto& symbol = build.symbols[s];
            if (s != 0) out << ',';
            out << "{\"name\":\"" << escape_json(symbol.name) << "\","
                << "\"state\":\"" << state_name(symbol.state) << "\","
                << "\"matchCount\":" << symbol.match_count << ",\"offset\":";
            if (symbol.offset) out << *symbol.offset;
            else out << "null";
            out << '}';
        }
        out << "]}";
        if (i + 1 != report.builds.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace enrage::core
