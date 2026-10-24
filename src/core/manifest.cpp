#include "core/manifest.hpp"

#include <charconv>
#include <cctype>
#include <limits>
#include <utility>

namespace enrage::core {
namespace {

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    ManifestParseResult parse() {
        ModManifest manifest;
        skip_ws();
        if (!consume('{')) return fail("expected object");

        skip_ws();
        if (consume('}')) return fail("manifest is empty");

        while (true) {
            auto key = string();
            if (!key) return fail("expected string key");
            skip_ws();
            if (!consume(':')) return fail("expected ':'");
            skip_ws();

            if (*key == "manifestVersion") {
                auto value = integer();
                if (!value || *value <= 0) return fail("manifestVersion must be a positive integer");
                manifest.manifest_version = static_cast<std::uint32_t>(*value);
            } else if (*key == "runtimeAbi") {
                auto value = integer();
                if (!value || *value <= 0) return fail("runtimeAbi must be a positive integer");
                manifest.runtime_abi = static_cast<std::uint32_t>(*value);
            } else if (*key == "id") {
                auto value = string();
                if (!value) return fail("id must be a string");
                manifest.id = std::move(*value);
            } else if (*key == "name") {
                auto value = string();
                if (!value) return fail("name must be a string");
                manifest.name = std::move(*value);
            } else if (*key == "version") {
                auto value = string();
                if (!value) return fail("version must be a string");
                manifest.version = std::move(*value);
            } else if (*key == "priority") {
                auto value = integer();
                if (!value) return fail("priority must be an integer");
                manifest.priority = *value;
            } else if (*key == "dependencies") {
                auto value = string_array();
                if (!value) return fail("dependencies must be a string array");
                manifest.dependencies = std::move(*value);
            } else if (*key == "loadAfter") {
                auto value = string_array();
                if (!value) return fail("loadAfter must be a string array");
                manifest.load_after = std::move(*value);
            } else if (*key == "conflicts") {
                auto value = string_array();
                if (!value) return fail("conflicts must be a string array");
                manifest.conflicts = std::move(*value);
            } else if (*key == "contentRoot") {
                auto value = string();
                if (!value) return fail("contentRoot must be a string");
                manifest.content_root = std::move(*value);
            } else if (*key == "nativePlugins") {
                auto value = string_array();
                if (!value) return fail("nativePlugins must be a string array");
                manifest.native_plugins = std::move(*value);
            } else if (!skip_value()) {
                return fail("invalid value");
            }

            skip_ws();
            if (consume('}')) break;
            if (!consume(',')) return fail("expected ',' or '}'");
            skip_ws();
        }

        skip_ws();
        if (pos_ != input_.size()) return fail("trailing data");
        if (manifest.id.empty()) return fail("id is required");
        if (manifest.name.empty()) manifest.name = manifest.id;
        if (manifest.version.empty()) manifest.version = "0.0.0";
        if (manifest.content_root.empty()) return fail("contentRoot cannot be empty");

        return {.manifest = std::move(manifest)};
    }

private:
    ManifestParseResult fail(std::string message) const {
        return {.error = ManifestError{std::move(message), pos_}};
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

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
            const char esc = input_[pos_++];
            switch (esc) {
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

    std::optional<int> integer() {
        skip_ws();
        const auto start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        const auto digits = pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (digits == pos_) {
            pos_ = start;
            return std::nullopt;
        }
        long long value{};
        const auto token = input_.substr(start, pos_ - start);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            pos_ = start;
            return std::nullopt;
        }
        return static_cast<int>(value);
    }

    std::optional<std::vector<std::string>> string_array() {
        if (!consume('[')) return std::nullopt;
        std::vector<std::string> values;
        skip_ws();
        if (consume(']')) return values;
        while (true) {
            auto value = string();
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
            skip_ws();
            if (consume(']')) return values;
            if (!consume(',')) return std::nullopt;
        }
    }

    bool skip_value() {
        skip_ws();
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') return string().has_value();
        if (input_[pos_] == '{') {
            ++pos_;
            skip_ws();
            if (consume('}')) return true;
            while (true) {
                if (!string()) return false;
                if (!consume(':') || !skip_value()) return false;
                if (consume('}')) return true;
                if (!consume(',')) return false;
            }
        }
        if (input_[pos_] == '[') {
            ++pos_;
            skip_ws();
            if (consume(']')) return true;
            while (true) {
                if (!skip_value()) return false;
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }
        const auto start = pos_;
        while (pos_ < input_.size() && input_[pos_] != ',' && input_[pos_] != '}' && input_[pos_] != ']' && !std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        return pos_ > start;
    }

    std::string_view input_;
    std::size_t pos_{};
};

} // namespace

ModSpec ModManifest::to_spec() const {
    return {
        .id = id,
        .priority = priority,
        .dependencies = dependencies,
        .load_after = load_after,
        .conflicts = conflicts,
    };
}

ManifestParseResult parse_manifest(std::string_view json) {
    return Reader{json}.parse();
}

} // namespace enrage::core
