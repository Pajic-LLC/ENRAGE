#include "core/profile_authoring.hpp"
#include "core/profile_matrix.hpp"
#include "core/profile_propagation.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::optional<std::uint64_t> number(std::string_view text) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return std::nullopt;

    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<enrage::core::SymbolSeed> symbol_seed(std::string_view value) {
    const auto split = value.rfind('@');
    if (split == std::string_view::npos || split == 0 || split + 1 >= value.size()) return std::nullopt;

    const auto offset = number(value.substr(split + 1));
    if (!offset || *offset > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return enrage::core::SymbolSeed{
        std::string{value.substr(0, split)},
        static_cast<std::size_t>(*offset),
    };
}

bool write_output(const std::optional<std::filesystem::path>& path, std::string_view text) {
    if (!path) {
        std::cout << text;
        if (text.empty() || text.back() != '\n') std::cout << '\n';
        return static_cast<bool>(std::cout);
    }

    std::ofstream output(*path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << text;
    if (text.empty() || text.back() != '\n') output << '\n';
    return static_cast<bool>(output);
}

struct DirectoryOptions {
    std::optional<std::filesystem::path> profiles;
    std::optional<std::filesystem::path> images;
    std::optional<std::filesystem::path> output;
};

std::optional<DirectoryOptions> directory_options(int argc, char** argv) {
    DirectoryOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--profiles") {
            if (++i >= argc) return std::nullopt;
            options.profiles = std::filesystem::path{argv[i]};
        } else if (arg == "--images") {
            if (++i >= argc) return std::nullopt;
            options.images = std::filesystem::path{argv[i]};
        } else if (arg == "--output") {
            if (++i >= argc) return std::nullopt;
            options.output = std::filesystem::path{argv[i]};
        } else {
            return std::nullopt;
        }
    }
    if (!options.profiles || !options.images) return std::nullopt;
    return options;
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  enrage_profile draft <reference> <target> --id <profile-id>\n"
        << "      --symbol <name>@<file-offset> [--symbol ...]\n"
        << "      [--capabilities <bits>] [--output <profile.enp>]\n"
        << "  enrage_profile propagate <reference> <target> --profile <known.enp>\n"
        << "      --id <profile-id> [--output <profile.enp>]\n"
        << "  enrage_profile validate --profiles <directory> --images <directory>\n"
        << "      [--output <report.json>]\n"
        << "  enrage_profile matrix --profiles <directory> --images <directory>\n"
        << "      [--output <matrix.json>]\n";
}

int draft_command(int argc, char** argv) {
    if (argc < 5) {
        usage();
        return 2;
    }

    const std::filesystem::path reference = argv[2];
    const std::filesystem::path target = argv[3];
    std::string id;
    std::vector<enrage::core::SymbolSeed> symbols;
    std::uint64_t capabilities{};
    std::optional<std::filesystem::path> output;

    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--id") {
            if (++i >= argc) return 2;
            id = argv[i];
        } else if (arg == "--symbol") {
            if (++i >= argc) return 2;
            auto parsed = symbol_seed(argv[i]);
            if (!parsed) return 2;
            symbols.push_back(std::move(*parsed));
        } else if (arg == "--capabilities") {
            if (++i >= argc) return 2;
            const auto parsed = number(argv[i]);
            if (!parsed) return 2;
            capabilities = *parsed;
        } else if (arg == "--output") {
            if (++i >= argc) return 2;
            output = std::filesystem::path{argv[i]};
        } else {
            usage();
            return 2;
        }
    }

    const auto result = enrage::core::draft_profile(reference, target, id, symbols, capabilities);
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            if (!error.symbol.empty()) std::cerr << error.symbol << ": ";
            std::cerr << error.message << '\n';
        }
        return 3;
    }

    for (const auto& symbol : result.profile->symbols) {
        std::cerr << symbol.name << ": 0x" << std::hex << symbol.reference_offset
                  << " -> 0x" << symbol.target_offset << std::dec
                  << " similarity=" << symbol.similarity;
        if (symbol.section) std::cerr << " region=" << *symbol.section;
        std::cerr << '\n';
    }

    if (!write_output(output, enrage::core::serialize_profile_draft(*result.profile))) return 4;
    return 0;
}

int propagate_command(int argc, char** argv) {
    if (argc < 7) {
        usage();
        return 2;
    }

    const std::filesystem::path reference = argv[2];
    const std::filesystem::path target = argv[3];
    std::optional<std::filesystem::path> profile;
    std::optional<std::filesystem::path> output;
    std::string id;

    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--profile") {
            if (++i >= argc) return 2;
            profile = std::filesystem::path{argv[i]};
        } else if (arg == "--id") {
            if (++i >= argc) return 2;
            id = argv[i];
        } else if (arg == "--output") {
            if (++i >= argc) return 2;
            output = std::filesystem::path{argv[i]};
        } else {
            usage();
            return 2;
        }
    }

    if (!profile || id.empty()) return 2;
    const auto result = enrage::core::propagate_profile(reference, target, *profile, id);
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            if (!error.symbol.empty()) std::cerr << error.symbol << ": ";
            std::cerr << error.message << '\n';
        }
        return 3;
    }

    if (!write_output(output, enrage::core::serialize_profile_draft(*result.profile))) return 4;
    return 0;
}

int validate_command(int argc, char** argv) {
    const auto options = directory_options(argc, argv);
    if (!options) return 2;

    const auto report = enrage::core::validate_profile_set_strict(*options->profiles, *options->images);
    if (!write_output(options->output, enrage::core::validation_report_json(report))) return 4;
    std::cerr << "profiles=" << report.profiles_scanned
              << " images=" << report.images_scanned
              << " valid=" << (report.ok() ? "yes" : "no") << '\n';
    return report.ok() ? 0 : 5;
}

int matrix_command(int argc, char** argv) {
    const auto options = directory_options(argc, argv);
    if (!options) return 2;

    const auto report = enrage::core::build_profile_matrix(*options->profiles, *options->images);
    if (!write_output(options->output, enrage::core::profile_matrix_json(report))) return 4;
    std::cerr << "profiles=" << report.profiles_scanned
              << " images=" << report.images_scanned
              << " rows=" << report.rows.size()
              << " expected-valid=" << (report.ok() ? "yes" : "no") << '\n';
    return report.ok() ? 0 : 5;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "draft") return draft_command(argc, argv);
    if (command == "propagate") return propagate_command(argc, argv);
    if (command == "validate") return validate_command(argc, argv);
    if (command == "matrix") return matrix_command(argc, argv);
    usage();
    return 2;
}
