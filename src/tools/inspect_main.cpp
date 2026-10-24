#include "core/inspection.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct Options {
    std::filesystem::path image;
    std::optional<std::filesystem::path> profiles;
    std::optional<std::filesystem::path> output;
};

std::optional<Options> parse_options(int argc, char** argv) {
    if (argc < 2) return std::nullopt;

    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--profiles") {
            if (++i >= argc) return std::nullopt;
            options.profiles = std::filesystem::path{argv[i]};
        } else if (arg == "--output") {
            if (++i >= argc) return std::nullopt;
            options.output = std::filesystem::path{argv[i]};
        } else if (!arg.empty() && arg.front() == '-') {
            return std::nullopt;
        } else if (options.image.empty()) {
            options.image = std::filesystem::path{arg};
        } else {
            return std::nullopt;
        }
    }

    if (options.image.empty()) return std::nullopt;
    return options;
}

void usage() {
    std::cerr << "usage: enrage_inspect <image> [--profiles <directory>] [--output <report.json>]\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        usage();
        return 2;
    }

    const auto report = enrage::core::inspect_executable(options->image, options->profiles);
    if (!report) {
        std::cerr << "ENRAGE could not read the image.\n";
        return 3;
    }

    const auto json = enrage::core::inspection_report_json(*report);
    if (options->output) {
        std::ofstream output(*options->output, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "ENRAGE could not open the report output.\n";
            return 4;
        }
        output << json << '\n';
        if (!output) {
            std::cerr << "ENRAGE could not write the report.\n";
            return 4;
        }
    } else {
        std::cout << json << '\n';
    }

    if (!report->pe_valid) {
        std::cerr << "ENRAGE inspection: input is not a valid PE image.\n";
    } else if (report->matched_profile) {
        std::cerr << "ENRAGE inspection: matched profile " << *report->matched_profile
                  << " with " << report->symbols.size() << " symbol(s).\n";
    } else {
        std::cerr << "ENRAGE inspection: PE image recognized, no profile matched.\n";
    }

    return 0;
}
