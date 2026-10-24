#include "core/inspection.hpp"

#include "core/build_profile_io.hpp"
#include "platform/fingerprint.hpp"
#include "platform/pe_image.hpp"
#include "platform/scan_image.hpp"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <vector>

namespace enrage::core {
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

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string state_name(platform::signature::ResolutionState state) {
    switch (state) {
        case platform::signature::ResolutionState::unknown_symbol: return "unknown_symbol";
        case platform::signature::ResolutionState::not_found: return "not_found";
        case platform::signature::ResolutionState::unique: return "unique";
        case platform::signature::ResolutionState::ambiguous: return "ambiguous";
    }
    return "unknown_symbol";
}

void map_symbol_location(
    InspectionSymbol& symbol,
    const platform::PeImage& pe,
    std::size_t file_offset) {
    for (const auto& section : pe.sections()) {
        const auto start = static_cast<std::uint64_t>(section.raw_offset);
        const auto size = static_cast<std::uint64_t>(section.raw_size);
        const auto offset = static_cast<std::uint64_t>(file_offset);
        if (offset < start || offset - start >= size) continue;

        const auto delta = offset - start;
        const auto rva64 = static_cast<std::uint64_t>(section.virtual_address) + delta;
        if (rva64 > std::numeric_limits<std::uint32_t>::max()) return;

        symbol.section = section.name;
        symbol.rva = static_cast<std::uint32_t>(rva64);
        if (pe.image_base() <= std::numeric_limits<std::uint64_t>::max() - rva64) {
            symbol.virtual_address = pe.image_base() + rva64;
        }
        return;
    }
}

} // namespace

std::optional<InspectionReport> inspect_executable(
    const std::filesystem::path& image,
    const std::optional<std::filesystem::path>& profile_directory) {
    const auto bytes = read_bytes(image);
    if (!bytes) return std::nullopt;

    const auto fingerprint = platform::fingerprint_file(image);
    if (!fingerprint) return std::nullopt;

    InspectionReport report;
    report.image = image;
    report.sha256 = fingerprint->hex();
    report.file_size = fingerprint->size;

    const auto pe = platform::PeImage::parse(*bytes);
    const auto scan_image = platform::ScanImage::from_file(*bytes);
    if (pe) {
        report.pe_valid = true;
        report.pe32_plus = pe->pe32_plus();
        report.x64 = pe->x64();
        report.machine = pe->machine();
        report.timestamp = pe->timestamp();
        report.image_base = pe->image_base();
        report.entry_rva = pe->entry_rva();
        report.image_size = pe->image_size();
        report.sections.reserve(pe->sections().size());
        for (const auto& section : pe->sections()) {
            report.sections.push_back({
                section.name,
                section.virtual_size,
                section.virtual_address,
                section.raw_size,
                section.raw_offset,
                section.characteristics,
            });
        }
    }

    if (!profile_directory) return report;

    auto imported = import_build_profiles(*profile_directory);
    report.imported_profile_count = imported.catalog.size();
    report.profile_errors.reserve(imported.errors.size());
    for (auto& error : imported.errors) {
        report.profile_errors.push_back({
            std::move(error.source),
            std::move(error.message),
            error.offset,
        });
    }

    const auto* profile = imported.catalog.find(*fingerprint);
    if (!profile) return report;

    report.matched_profile = profile->id;
    report.capabilities = profile->capabilities;

    const auto resolver = imported.catalog.resolver_for(*fingerprint);
    if (!resolver) return report;

    report.symbols.reserve(profile->symbols.size());
    const std::span<const std::byte> image_bytes{*bytes};
    for (const auto& spec : profile->symbols) {
        InspectionSymbol symbol;
        symbol.name = spec.name;

        if (!spec.region.empty()) {
            if (!scan_image) {
                symbol.state = platform::signature::ResolutionState::not_found;
                report.symbols.push_back(std::move(symbol));
                continue;
            }
            const auto resolution = platform::resolve_in_image(*resolver, spec.name, *scan_image);
            symbol.state = resolution.state;
            symbol.match_count = resolution.match_count;
            symbol.file_offset = resolution.storage_offset;
            symbol.rva = resolution.rva;
            symbol.virtual_address = resolution.virtual_address;
            symbol.section = resolution.region;
            report.symbols.push_back(std::move(symbol));
            continue;
        }

        const auto resolution = resolver->resolve(spec.name, image_bytes);
        symbol.state = resolution.state;
        symbol.match_count = resolution.match_count;
        symbol.file_offset = resolution.offset;
        if (pe && resolution.offset) {
            map_symbol_location(symbol, *pe, *resolution.offset);
        }
        report.symbols.push_back(std::move(symbol));
    }

    return report;
}

std::string inspection_report_json(const InspectionReport& report) {
    std::ostringstream out;
    out << '{';
    out << "\"schema\":1";
    out << ",\"image\":\"" << json_escape(report.image.generic_string()) << '\"';
    out << ",\"sha256\":\"" << report.sha256 << '\"';
    out << ",\"fileSize\":" << report.file_size;

    out << ",\"pe\":{";
    out << "\"valid\":" << (report.pe_valid ? "true" : "false");
    if (report.pe_valid) {
        out << ",\"pe32Plus\":" << (report.pe32_plus ? "true" : "false");
        out << ",\"x64\":" << (report.x64 ? "true" : "false");
        out << ",\"machine\":" << report.machine;
        out << ",\"timestamp\":" << report.timestamp;
        out << ",\"imageBase\":" << report.image_base;
        out << ",\"entryRva\":" << report.entry_rva;
        out << ",\"imageSize\":" << report.image_size;
        out << ",\"sections\":[";
        for (std::size_t i = 0; i < report.sections.size(); ++i) {
            if (i != 0) out << ',';
            const auto& section = report.sections[i];
            out << '{'
                << "\"name\":\"" << json_escape(section.name) << '\"'
                << ",\"virtualSize\":" << section.virtual_size
                << ",\"virtualAddress\":" << section.virtual_address
                << ",\"rawSize\":" << section.raw_size
                << ",\"rawOffset\":" << section.raw_offset
                << ",\"characteristics\":" << section.characteristics
                << '}';
        }
        out << ']';
    }
    out << '}';

    out << ",\"profiles\":{";
    out << "\"imported\":" << report.imported_profile_count;
    out << ",\"errors\":[";
    for (std::size_t i = 0; i < report.profile_errors.size(); ++i) {
        if (i != 0) out << ',';
        const auto& error = report.profile_errors[i];
        out << '{'
            << "\"source\":\"" << json_escape(error.source.generic_string()) << '\"'
            << ",\"message\":\"" << json_escape(error.message) << '\"'
            << ",\"offset\":" << error.offset
            << '}';
    }
    out << ']';
    if (report.matched_profile) {
        out << ",\"matched\":\"" << json_escape(*report.matched_profile) << '\"';
        out << ",\"capabilities\":" << report.capabilities;
    } else {
        out << ",\"matched\":null";
        out << ",\"capabilities\":0";
    }
    out << '}';

    out << ",\"symbols\":[";
    for (std::size_t i = 0; i < report.symbols.size(); ++i) {
        if (i != 0) out << ',';
        const auto& symbol = report.symbols[i];
        out << '{'
            << "\"name\":\"" << json_escape(symbol.name) << '\"'
            << ",\"state\":\"" << state_name(symbol.state) << '\"'
            << ",\"matchCount\":" << symbol.match_count;
        if (symbol.file_offset) out << ",\"fileOffset\":" << *symbol.file_offset;
        else out << ",\"fileOffset\":null";
        if (symbol.section) out << ",\"section\":\"" << json_escape(*symbol.section) << '\"';
        else out << ",\"section\":null";
        if (symbol.rva) out << ",\"rva\":" << *symbol.rva;
        else out << ",\"rva\":null";
        if (symbol.virtual_address) out << ",\"virtualAddress\":" << *symbol.virtual_address;
        else out << ",\"virtualAddress\":null";
        out << '}';
    }
    out << ']';
    out << '}';
    return out.str();
}

} // namespace enrage::core
