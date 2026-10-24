#include "core/build_profile_io.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace enrage::core {
namespace {

class Reader {
public:
    Reader(std::string_view input, std::filesystem::path source)
        : input_(input), source_(std::move(source)) {}

    BuildProfileParseResult parse() {
        BuildProfile profile;
        std::uint64_t version{};
        bool have_version = false;
        bool have_id = false;
        bool have_fingerprint = false;
        bool have_capabilities = false;
        bool have_symbols = false;

        if (!object_begin()) return fail("expected profile object");
        if (object_end()) return fail("profile is empty");

        while (true) {
            auto key = string();
            if (!key) return fail("expected string key");
            if (!colon()) return fail("expected ':'");

            if (*key == "version") {
                if (have_version) return fail("duplicate version");
                auto value = unsigned_integer();
                if (!value) return fail("version must be an unsigned integer");
                version = *value;
                have_version = true;
            } else if (*key == "id") {
                if (have_id) return fail("duplicate id");
                auto value = string();
                if (!value || value->empty()) return fail("id must be a non-empty string");
                profile.id = std::move(*value);
                have_id = true;
            } else if (*key == "fingerprint") {
                if (have_fingerprint) return fail("duplicate fingerprint");
                if (!fingerprint(profile.fingerprint)) return fail(last_error_);
                have_fingerprint = true;
            } else if (*key == "capabilities") {
                if (have_capabilities) return fail("duplicate capabilities");
                auto value = unsigned_integer();
                if (!value) return fail("capabilities must be an unsigned integer");
                profile.capabilities = *value;
                have_capabilities = true;
            } else if (*key == "symbols") {
                if (have_symbols) return fail("duplicate symbols");
                if (!symbols(profile.symbols)) return fail(last_error_);
                have_symbols = true;
            } else {
                return fail("unknown profile field: " + *key);
            }

            if (object_end()) break;
            if (!comma()) return fail("expected ',' or '}'");
        }

        skip_ws();
        if (pos_ != input_.size()) return fail("trailing data");
        if (!have_version) return fail("version is required");
        if (version != 1 && version != 2) return fail("unsupported profile version");
        if (!have_id) return fail("id is required");
        if (!have_fingerprint) return fail("fingerprint is required");
        if (!have_capabilities) return fail("capabilities is required");
        if (!have_symbols) return fail("symbols is required");
        if (version == 1 && std::ranges::any_of(profile.symbols, [](const auto& symbol) {
                return !symbol.region.empty();
            })) {
            return fail("symbol region requires profile version 2");
        }

        return {.profile = std::move(profile)};
    }

private:
    BuildProfileParseResult fail(std::string message) const {
        return {.error = BuildProfileError{source_, std::move(message), pos_}};
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool consume(char ch) {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != ch) return false;
        ++pos_;
        return true;
    }

    bool object_begin() { return consume('{'); }
    bool object_end() { return consume('}'); }
    bool array_begin() { return consume('['); }
    bool array_end() { return consume(']'); }
    bool colon() { return consume(':'); }
    bool comma() { return consume(','); }

    std::optional<std::string> string() {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const char ch = input_[pos_++];
            if (ch == '"') return out;
            if (static_cast<unsigned char>(ch) < 0x20) return std::nullopt;
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size()) return std::nullopt;
            switch (input_[pos_++]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> unsigned_integer() {
        skip_ws();
        const auto start = pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (start == pos_) return std::nullopt;
        std::uint64_t value{};
        const auto token = input_.substr(start, pos_ - start);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return std::nullopt;
        return value;
    }

    std::optional<std::int64_t> signed_integer() {
        skip_ws();
        const auto start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        const auto digits = pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (digits == pos_) {
            pos_ = start;
            return std::nullopt;
        }
        std::int64_t value{};
        const auto token = input_.substr(start, pos_ - start);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            pos_ = start;
            return std::nullopt;
        }
        return value;
    }

    static std::optional<std::uint8_t> hex_nibble(char ch) {
        if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(10 + ch - 'a');
        if (ch >= 'A' && ch <= 'F') return static_cast<std::uint8_t>(10 + ch - 'A');
        return std::nullopt;
    }

    bool fingerprint(platform::FileFingerprint& output) {
        if (!object_begin()) {
            last_error_ = "fingerprint must be an object";
            return false;
        }

        bool have_sha = false;
        bool have_size = false;
        if (object_end()) {
            last_error_ = "fingerprint is empty";
            return false;
        }

        while (true) {
            auto key = string();
            if (!key || !colon()) {
                last_error_ = "invalid fingerprint object";
                return false;
            }

            if (*key == "sha256") {
                if (have_sha) {
                    last_error_ = "duplicate fingerprint sha256";
                    return false;
                }
                auto value = string();
                if (!value || value->size() != 64) {
                    last_error_ = "sha256 must contain exactly 64 hex characters";
                    return false;
                }
                for (std::size_t i = 0; i < output.sha256.size(); ++i) {
                    const auto high = hex_nibble((*value)[i * 2]);
                    const auto low = hex_nibble((*value)[i * 2 + 1]);
                    if (!high || !low) {
                        last_error_ = "sha256 contains non-hex characters";
                        return false;
                    }
                    output.sha256[i] = static_cast<std::uint8_t>((*high << 4) | *low);
                }
                have_sha = true;
            } else if (*key == "size") {
                if (have_size) {
                    last_error_ = "duplicate fingerprint size";
                    return false;
                }
                auto value = unsigned_integer();
                if (!value) {
                    last_error_ = "fingerprint size must be an unsigned integer";
                    return false;
                }
                output.size = *value;
                have_size = true;
            } else {
                last_error_ = "unknown fingerprint field: " + *key;
                return false;
            }

            if (object_end()) break;
            if (!comma()) {
                last_error_ = "invalid fingerprint object";
                return false;
            }
        }

        if (!have_sha || !have_size) {
            last_error_ = "fingerprint requires sha256 and size";
            return false;
        }
        return true;
    }

    bool candidates(std::vector<platform::signature::SignatureCandidate>& output) {
        if (!array_begin()) {
            last_error_ = "candidates must be an array";
            return false;
        }
        if (array_end()) {
            last_error_ = "candidate array cannot be empty";
            return false;
        }

        while (true) {
            if (!object_begin()) {
                last_error_ = "candidate must be an object";
                return false;
            }

            std::optional<platform::signature::Pattern> pattern;
            std::ptrdiff_t adjustment{};
            bool have_pattern = false;
            bool have_adjustment = false;

            if (object_end()) {
                last_error_ = "candidate is empty";
                return false;
            }

            while (true) {
                auto key = string();
                if (!key || !colon()) {
                    last_error_ = "invalid candidate object";
                    return false;
                }

                if (*key == "pattern") {
                    if (have_pattern) {
                        last_error_ = "duplicate candidate pattern";
                        return false;
                    }
                    auto text = string();
                    if (!text) {
                        last_error_ = "candidate pattern must be a string";
                        return false;
                    }
                    pattern = platform::signature::Pattern::parse(*text);
                    if (!pattern) {
                        last_error_ = "invalid signature pattern";
                        return false;
                    }
                    have_pattern = true;
                } else if (*key == "adjustment") {
                    if (have_adjustment) {
                        last_error_ = "duplicate candidate adjustment";
                        return false;
                    }
                    auto value = signed_integer();
                    if (!value || *value < std::numeric_limits<std::ptrdiff_t>::min() ||
                        *value > std::numeric_limits<std::ptrdiff_t>::max()) {
                        last_error_ = "candidate adjustment is out of range";
                        return false;
                    }
                    adjustment = static_cast<std::ptrdiff_t>(*value);
                    have_adjustment = true;
                } else {
                    last_error_ = "unknown candidate field: " + *key;
                    return false;
                }

                if (object_end()) break;
                if (!comma()) {
                    last_error_ = "invalid candidate object";
                    return false;
                }
            }

            if (!have_pattern) {
                last_error_ = "candidate pattern is required";
                return false;
            }
            output.push_back({std::move(*pattern), adjustment});

            if (array_end()) break;
            if (!comma()) {
                last_error_ = "invalid candidates array";
                return false;
            }
        }
        return true;
    }

    bool symbols(std::vector<platform::signature::SymbolSpec>& output) {
        if (!array_begin()) {
            last_error_ = "symbols must be an array";
            return false;
        }
        if (array_end()) return true;

        while (true) {
            if (!object_begin()) {
                last_error_ = "symbol must be an object";
                return false;
            }

            platform::signature::SymbolSpec symbol;
            bool have_name = false;
            bool have_candidates = false;
            bool have_region = false;

            if (object_end()) {
                last_error_ = "symbol is empty";
                return false;
            }

            while (true) {
                auto key = string();
                if (!key || !colon()) {
                    last_error_ = "invalid symbol object";
                    return false;
                }

                if (*key == "name") {
                    if (have_name) {
                        last_error_ = "duplicate symbol name";
                        return false;
                    }
                    auto value = string();
                    if (!value || value->empty()) {
                        last_error_ = "symbol name must be non-empty";
                        return false;
                    }
                    symbol.name = std::move(*value);
                    have_name = true;
                } else if (*key == "candidates") {
                    if (have_candidates) {
                        last_error_ = "duplicate symbol candidates";
                        return false;
                    }
                    if (!candidates(symbol.candidates)) return false;
                    have_candidates = true;
                } else if (*key == "region") {
                    if (have_region) {
                        last_error_ = "duplicate symbol region";
                        return false;
                    }
                    auto value = string();
                    if (!value || value->empty()) {
                        last_error_ = "symbol region must be non-empty";
                        return false;
                    }
                    symbol.region = std::move(*value);
                    have_region = true;
                } else {
                    last_error_ = "unknown symbol field: " + *key;
                    return false;
                }

                if (object_end()) break;
                if (!comma()) {
                    last_error_ = "invalid symbol object";
                    return false;
                }
            }

            if (!have_name || !have_candidates) {
                last_error_ = "symbol requires name and candidates";
                return false;
            }
            if (std::ranges::any_of(output, [&](const auto& existing) { return existing.name == symbol.name; })) {
                last_error_ = "duplicate symbol name: " + symbol.name;
                return false;
            }
            output.push_back(std::move(symbol));

            if (array_end()) break;
            if (!comma()) {
                last_error_ = "invalid symbols array";
                return false;
            }
        }
        return true;
    }

    std::string_view input_;
    std::filesystem::path source_;
    std::size_t pos_{};
    std::string last_error_;
};

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    return buffer.str();
}

} // namespace

BuildProfileParseResult parse_build_profile(std::string_view json, std::filesystem::path source) {
    return Reader{json, std::move(source)}.parse();
}

BuildProfileImportResult import_build_profiles(const std::filesystem::path& directory) {
    BuildProfileImportResult result;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return result;
    if (error || !std::filesystem::is_directory(directory, error) || error) {
        result.errors.push_back({directory, "profile path is not a readable directory", 0});
        return result;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && !error && it->path().extension() == ".enp") {
            files.push_back(it->path());
        }
    }
    if (error) {
        result.errors.push_back({directory, "failed while enumerating profile directory", 0});
        return result;
    }
    std::ranges::sort(files);

    for (const auto& file : files) {
        auto text = read_text(file);
        if (!text) {
            result.errors.push_back({file, "profile is unreadable", 0});
            continue;
        }
        auto parsed = parse_build_profile(*text, file);
        if (!parsed.ok()) {
            result.errors.push_back(std::move(*parsed.error));
            continue;
        }
        if (!result.catalog.add(std::move(*parsed.profile))) {
            result.errors.push_back({file, "duplicate build id or fingerprint", 0});
        }
    }
    return result;
}

} // namespace enrage::core
