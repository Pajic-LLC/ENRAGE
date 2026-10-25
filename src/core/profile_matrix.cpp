#include "core/profile_matrix.hpp"

#include "core/build_profile_v3.hpp"
#include "platform/fingerprint.hpp"
#include "platform/scan_image.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
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
    const auto raw_size = static_cast<std::uint64_t>(end);
    if (raw_size > std::numeric_limits<std::size_t>::max() ||
        raw_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(raw_size);
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
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

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

std::vector<std::filesystem::path> files_with_extension(
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

struct ImageData {
    std::filesystem::path path;
    platform::FileFingerprint fingerprint;
    std::vector<std::byte> bytes;
};

platform::ImageResolution resolve_symbol(
    const platform::signature::SymbolSpec& symbol,
    const platform::signature::Resolver& resolver,
    const std::vector<std::byte>& bytes) {
    const auto image = platform::ScanImage::from_file(bytes);
    if (!image) {
        return {
            .state = platform::signature::ResolutionState::not_found,
            .match_count = 0,
        };
    }
    return platform::resolve_in_image(resolver, symbol.name, *image);
}

} // namespace

bool ProfileMatrixReport::ok() const noexcept {
    if (!errors.empty()) return false;
    for (const auto& row : rows) {
        const auto expected = std::ranges::count_if(row.cells, [](const MatrixCell& cell) { return cell.expected; });
        if (expected != 1) return false;
        const auto valid_expected = std::ranges::count_if(row.cells, [](const MatrixCell& cell) {
            return cell.expected && cell.state == platform::signature::ResolutionState::unique;
        });
        if (valid_expected != 1) return false;
    }
    return true;
}

ProfileMatrixReport build_profile_matrix(
    const std::filesystem::path& profile_directory,
    const std::filesystem::path& image_directory) {
    ProfileMatrixReport report;

    std::vector<ImageData> images;
    for (const auto& path : files_with_extension(image_directory, {})) {
        const auto extension = path.extension().string();
        if (extension != ".exe" && extension != ".dll") continue;
        const auto fingerprint = platform::fingerprint_file(path);
        const auto bytes = read_bytes(path);
        if (!fingerprint || !bytes) {
            report.errors.push_back({path, "image is unreadable"});
            continue;
        }
        images.push_back({path, *fingerprint, std::move(*bytes)});
    }
    report.images_scanned = images.size();

    const auto profile_files = files_with_extension(profile_directory, ".enp");
    report.profiles_scanned = profile_files.size();
    for (const auto& profile_file : profile_files) {
        const auto text = read_text(profile_file);
        if (!text) {
            report.errors.push_back({profile_file, "profile is unreadable"});
            continue;
        }
        auto parsed = parse_build_profile_v3(*text, profile_file);
        if (!parsed.ok()) {
            report.errors.push_back({profile_file, parsed.error ? parsed.error->message : "invalid profile"});
            continue;
        }

        const auto& profile = *parsed.profile;
        const auto exact_image_count = std::ranges::count_if(images, [&](const ImageData& image) {
            return image.fingerprint == profile.fingerprint;
        });
        if (exact_image_count != 1) {
            report.errors.push_back({
                profile_file,
                exact_image_count == 0 ? "no exact image matches the profile fingerprint"
                                       : "multiple images match the profile fingerprint",
            });
        }

        for (const auto& symbol : profile.symbols) {
            platform::signature::Resolver resolver;
            if (!resolver.add(symbol)) {
                report.errors.push_back({profile_file, "symbol could not be loaded into resolver: " + symbol.name});
                continue;
            }

            MatrixRow row{
                .profile_source = profile_file,
                .profile_id = profile.id,
                .symbol = symbol.name,
            };
            row.cells.reserve(images.size());
            for (const auto& image : images) {
                const auto resolution = resolve_symbol(symbol, resolver, image.bytes);
                row.cells.push_back({
                    .image = image.path,
                    .expected = image.fingerprint == profile.fingerprint,
                    .state = resolution.state,
                    .match_count = resolution.match_count,
                    .offset = resolution.storage_offset,
                });
            }
            report.rows.push_back(std::move(row));
        }
    }

    return report;
}

std::string profile_matrix_json(const ProfileMatrixReport& report) {
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
    out << "],\n  \"rows\": [\n";

    for (std::size_t i = 0; i < report.rows.size(); ++i) {
        const auto& row = report.rows[i];
        out << "    {\"profile\":\"" << escape_json(row.profile_id) << "\","
            << "\"profileSource\":\"" << escape_json(row.profile_source.generic_string()) << "\","
            << "\"symbol\":\"" << escape_json(row.symbol) << "\",\"cells\":[";
        for (std::size_t c = 0; c < row.cells.size(); ++c) {
            const auto& cell = row.cells[c];
            if (c != 0) out << ',';
            out << "{\"image\":\"" << escape_json(cell.image.generic_string()) << "\","
                << "\"expected\":" << (cell.expected ? "true" : "false") << ','
                << "\"state\":\"" << state_name(cell.state) << "\","
                << "\"matchCount\":" << cell.match_count << ",\"offset\":";
            if (cell.offset) out << *cell.offset;
            else out << "null";
            out << '}';
        }
        out << "]}";
        if (i + 1 != report.rows.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace enrage::core
