#include "core/build_profile_v3.hpp"

#include "platform/signature/pattern.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enrage::core {
namespace {

enum class JsonKind { object, array, string, number };

struct JsonValue {
    JsonKind kind{JsonKind::object};
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
    std::string text;
};

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    std::optional<JsonValue> parse() {
        auto value = value_impl();
        skip_ws();
        if (!value || pos_ != input_.size()) {
            if (error_.empty()) fail("trailing data");
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t error_offset() const noexcept { return error_offset_; }

private:
    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    void fail(std::string message) {
        if (!error_.empty()) return;
        error_ = std::move(message);
        error_offset_ = pos_;
    }

    bool consume(char ch) {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != ch) return false;
        ++pos_;
        return true;
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (pos_ >= input_.size() || input_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const char ch = input_[pos_++];
            if (ch == '"') return out;
            if (static_cast<unsigned char>(ch) < 0x20) {
                fail("control character in string");
                return std::nullopt;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size()) break;
            switch (input_[pos_++]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    fail("unsupported string escape");
                    return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        skip_ws();
        const auto start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        const auto digits = pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (digits == pos_) {
            pos_ = start;
            return std::nullopt;
        }
        return JsonValue{.kind = JsonKind::number, .text = std::string{input_.substr(start, pos_ - start)}};
    }

    std::optional<JsonValue> parse_array() {
        if (!consume('[')) return std::nullopt;
        JsonValue result{.kind = JsonKind::array};
        if (consume(']')) return result;
        while (true) {
            auto item = value_impl();
            if (!item) {
                if (error_.empty()) fail("invalid array value");
                return std::nullopt;
            }
            result.array.push_back(std::move(*item));
            if (consume(']')) return result;
            if (!consume(',')) {
                fail("expected ',' or ']' in array");
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> parse_object() {
        if (!consume('{')) return std::nullopt;
        JsonValue result{.kind = JsonKind::object};
        if (consume('}')) return result;
        while (true) {
            auto key = parse_string();
            if (!key) {
                if (error_.empty()) fail("expected object key");
                return std::nullopt;
            }
            if (!consume(':')) {
                fail("expected ':' after object key");
                return std::nullopt;
            }
            auto value = value_impl();
            if (!value) {
                if (error_.empty()) fail("invalid object value");
                return std::nullopt;
            }
            result.object.emplace_back(std::move(*key), std::move(*value));
            if (consume('}')) return result;
            if (!consume(',')) {
                fail("expected ',' or '}' in object");
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> value_impl() {
        skip_ws();
        if (pos_ >= input_.size()) return std::nullopt;
        if (input_[pos_] == '{') return parse_object();
        if (input_[pos_] == '[') return parse_array();
        if (input_[pos_] == '"') {
            auto text = parse_string();
            if (!text) return std::nullopt;
            return JsonValue{.kind = JsonKind::string, .text = std::move(*text)};
        }
        return parse_number();
    }

    std::string_view input_;
    std::size_t pos_{};
    std::string error_;
    std::size_t error_offset_{};
};

struct SchemaError {
    std::string message;
};

const JsonValue* field(const JsonValue& object, std::string_view name, SchemaError& error) {
    if (object.kind != JsonKind::object) {
        error.message = "expected object";
        return nullptr;
    }
    const JsonValue* found = nullptr;
    for (const auto& [key, value] : object.object) {
        if (key != name) continue;
        if (found) {
            error.message = "duplicate field: " + std::string{name};
            return nullptr;
        }
        found = &value;
    }
    return found;
}

bool only_fields(const JsonValue& object, std::initializer_list<std::string_view> allowed, SchemaError& error) {
    for (const auto& [key, value] : object.object) {
        static_cast<void>(value);
        const bool known = std::ranges::any_of(allowed, [&](std::string_view candidate) { return candidate == key; });
        if (!known) {
            error.message = "unknown field: " + key;
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> as_u64(const JsonValue* value) {
    if (!value || value->kind != JsonKind::number || value->text.starts_with('-')) return std::nullopt;
    std::uint64_t result{};
    const auto parsed = std::from_chars(value->text.data(), value->text.data() + value->text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value->text.data() + value->text.size()) return std::nullopt;
    return result;
}

std::optional<std::int64_t> as_i64(const JsonValue* value) {
    if (!value || value->kind != JsonKind::number) return std::nullopt;
    std::int64_t result{};
    const auto parsed = std::from_chars(value->text.data(), value->text.data() + value->text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value->text.data() + value->text.size()) return std::nullopt;
    return result;
}

const std::string* as_string(const JsonValue* value) {
    return value && value->kind == JsonKind::string ? &value->text : nullptr;
}

std::optional<std::uint8_t> hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return static_cast<std::uint8_t>(10 + ch - 'A');
    return std::nullopt;
}

bool parse_fingerprint(const JsonValue& value, platform::FileFingerprint& output, SchemaError& error) {
    if (value.kind != JsonKind::object || !only_fields(value, {"sha256", "size"}, error)) return false;
    const auto* sha = as_string(field(value, "sha256", error));
    const auto size = as_u64(field(value, "size", error));
    if (!error.message.empty()) return false;
    if (!sha || sha->size() != 64 || !size) {
        error.message = "fingerprint requires 64-character sha256 and unsigned size";
        return false;
    }
    for (std::size_t index = 0; index < output.sha256.size(); ++index) {
        const auto high = hex_nibble((*sha)[index * 2]);
        const auto low = hex_nibble((*sha)[index * 2 + 1]);
        if (!high || !low) {
            error.message = "fingerprint sha256 contains non-hex characters";
            return false;
        }
        output.sha256[index] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    output.size = *size;
    return true;
}

bool parse_candidates(
    const JsonValue& value,
    std::vector<platform::signature::SignatureCandidate>& output,
    SchemaError& error) {
    if (value.kind != JsonKind::array || value.array.empty()) {
        error.message = "symbol candidates must be a non-empty array";
        return false;
    }
    for (const auto& item : value.array) {
        if (item.kind != JsonKind::object || !only_fields(item, {"pattern", "adjustment"}, error)) return false;
        const auto* pattern_text = as_string(field(item, "pattern", error));
        if (!error.message.empty() || !pattern_text) {
            error.message = error.message.empty() ? "candidate pattern must be a string" : error.message;
            return false;
        }
        auto pattern = platform::signature::Pattern::parse(*pattern_text);
        if (!pattern) {
            error.message = "invalid signature pattern";
            return false;
        }
        std::ptrdiff_t adjustment{};
        if (const auto* adjustment_value = field(item, "adjustment", error)) {
            const auto parsed = as_i64(adjustment_value);
            if (!parsed || *parsed < std::numeric_limits<std::ptrdiff_t>::min() ||
                *parsed > std::numeric_limits<std::ptrdiff_t>::max()) {
                error.message = "candidate adjustment is out of range";
                return false;
            }
            adjustment = static_cast<std::ptrdiff_t>(*parsed);
        }
        if (!error.message.empty()) return false;
        output.push_back({std::move(*pattern), adjustment});
    }
    return true;
}

bool parse_symbols(
    const JsonValue& value,
    std::vector<platform::signature::SymbolSpec>& output,
    SchemaError& error) {
    if (value.kind != JsonKind::array) {
        error.message = "symbols must be an array";
        return false;
    }
    for (const auto& item : value.array) {
        if (item.kind != JsonKind::object || !only_fields(item, {"name", "region", "candidates"}, error)) return false;
        const auto* name = as_string(field(item, "name", error));
        const auto* candidates = field(item, "candidates", error);
        if (!error.message.empty() || !name || name->empty() || !candidates) {
            error.message = error.message.empty() ? "symbol requires non-empty name and candidates" : error.message;
            return false;
        }
        if (std::ranges::any_of(output, [&](const auto& existing) { return existing.name == *name; })) {
            error.message = "duplicate symbol name: " + *name;
            return false;
        }
        platform::signature::SymbolSpec symbol;
        symbol.name = *name;
        if (const auto* region_value = field(item, "region", error)) {
            const auto* region = as_string(region_value);
            if (!region || region->empty()) {
                error.message = "symbol region must be a non-empty string";
                return false;
            }
            symbol.region = *region;
        }
        if (!error.message.empty() || !parse_candidates(*candidates, symbol.candidates, error)) return false;
        output.push_back(std::move(symbol));
    }
    return true;
}

bool parse_anchors(const JsonValue& value, BuildProfile& profile, SchemaError& error) {
    if (value.kind != JsonKind::array) {
        error.message = "anchors must be an array";
        return false;
    }
    for (const auto& item : value.array) {
        if (item.kind != JsonKind::object || !only_fields(item, {"name", "symbol"}, error)) return false;
        const auto* name = as_string(field(item, "name", error));
        const auto* symbol = as_string(field(item, "symbol", error));
        if (!error.message.empty() || !name || name->empty() || !symbol || symbol->empty()) {
            error.message = error.message.empty() ? "anchor requires non-empty name and symbol" : error.message;
            return false;
        }
        if (std::ranges::any_of(profile.anchors, [&](const auto& existing) { return existing.name == *name; })) {
            error.message = "duplicate anchor name: " + *name;
            return false;
        }
        if (!std::ranges::any_of(profile.symbols, [&](const auto& existing) { return existing.name == *symbol; })) {
            error.message = "anchor " + *name + " references unknown symbol " + *symbol;
            return false;
        }
        profile.anchors.push_back({*name, *symbol});
    }
    return true;
}

bool parse_string_array(const JsonValue& value, std::vector<std::string>& output, SchemaError& error) {
    if (value.kind != JsonKind::array || value.array.empty()) {
        error.message = "anchors requirement must be a non-empty array";
        return false;
    }
    for (const auto& item : value.array) {
        const auto* text = as_string(&item);
        if (!text || text->empty()) {
            error.message = "requirement anchor must be a non-empty string";
            return false;
        }
        if (std::ranges::find(output, *text) != output.end()) {
            error.message = "duplicate anchor in capability requirement: " + *text;
            return false;
        }
        output.push_back(*text);
    }
    return true;
}

bool parse_requirements(const JsonValue& value, BuildProfile& profile, SchemaError& error) {
    if (value.kind != JsonKind::array) {
        error.message = "requirements must be an array";
        return false;
    }
    for (const auto& item : value.array) {
        if (item.kind != JsonKind::object || !only_fields(item, {"capability", "anchors"}, error)) return false;
        const auto capability = as_u64(field(item, "capability", error));
        const auto* anchors = field(item, "anchors", error);
        if (!error.message.empty() || !capability || *capability == 0 || !anchors) {
            error.message = error.message.empty() ? "requirement requires non-zero capability and anchors" : error.message;
            return false;
        }
        if ((*capability & (*capability - 1)) != 0) {
            error.message = "requirement capability must contain exactly one bit";
            return false;
        }
        if ((profile.capabilities & *capability) == 0) {
            error.message = "requirement capability is not advertised by the profile";
            return false;
        }
        if (std::ranges::any_of(profile.requirements, [&](const auto& existing) {
                return existing.capability == *capability;
            })) {
            error.message = "duplicate capability requirement";
            return false;
        }
        ProfileCapabilityRequirement requirement;
        requirement.capability = *capability;
        if (!parse_string_array(*anchors, requirement.anchors, error)) return false;
        for (const auto& name : requirement.anchors) {
            if (!std::ranges::any_of(profile.anchors, [&](const auto& anchor) { return anchor.name == name; })) {
                error.message = "requirement references unknown anchor: " + name;
                return false;
            }
        }
        profile.requirements.push_back(std::move(requirement));
    }
    return true;
}

std::string escape_json(std::string_view input) {
    std::string out;
    for (const char ch : input) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream output;
    output << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    return output.str();
}

} // namespace

BuildProfileParseResult parse_build_profile_v3(std::string_view json, std::filesystem::path source) {
    JsonReader reader{json};
    auto root = reader.parse();
    if (!root) {
        return {.error = BuildProfileError{std::move(source), reader.error(), reader.error_offset()}};
    }

    SchemaError error;
    if (root->kind != JsonKind::object ||
        !only_fields(*root, {"version", "id", "fingerprint", "capabilities", "symbols", "anchors", "requirements"}, error)) {
        return {.error = BuildProfileError{std::move(source), error.message.empty() ? "profile must be an object" : error.message, 0}};
    }

    const auto version = as_u64(field(*root, "version", error));
    const auto* id = as_string(field(*root, "id", error));
    const auto* fingerprint = field(*root, "fingerprint", error);
    const auto capabilities = as_u64(field(*root, "capabilities", error));
    const auto* symbols = field(*root, "symbols", error);
    if (!error.message.empty() || !version || *version < 1 || *version > 3 || !id || id->empty() ||
        !fingerprint || !capabilities || !symbols) {
        const auto message = !error.message.empty() ? error.message : "profile requires version 1-3, id, fingerprint, capabilities, and symbols";
        return {.error = BuildProfileError{std::move(source), message, 0}};
    }

    BuildProfile profile;
    profile.id = *id;
    profile.capabilities = *capabilities;
    if (!parse_fingerprint(*fingerprint, profile.fingerprint, error) ||
        !parse_symbols(*symbols, profile.symbols, error)) {
        return {.error = BuildProfileError{std::move(source), error.message, 0}};
    }

    const bool has_regions = std::ranges::any_of(profile.symbols, [](const auto& symbol) { return !symbol.region.empty(); });
    if (*version == 1 && has_regions) {
        return {.error = BuildProfileError{std::move(source), "symbol region requires profile version 2 or newer", 0}};
    }

    const auto* anchors = field(*root, "anchors", error);
    const auto* requirements = field(*root, "requirements", error);
    if (!error.message.empty()) {
        return {.error = BuildProfileError{std::move(source), error.message, 0}};
    }
    if (*version < 3 && (anchors || requirements)) {
        return {.error = BuildProfileError{std::move(source), "anchors and requirements require profile version 3", 0}};
    }
    if (anchors && !parse_anchors(*anchors, profile, error)) {
        return {.error = BuildProfileError{std::move(source), error.message, 0}};
    }
    if (requirements && !parse_requirements(*requirements, profile, error)) {
        return {.error = BuildProfileError{std::move(source), error.message, 0}};
    }

    return {.profile = std::move(profile)};
}

BuildProfileImportResult import_build_profiles_v3(const std::filesystem::path& directory) {
    BuildProfileImportResult result;
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return result;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && !error && it->path().extension() == ".enp") files.push_back(it->path());
    }
    std::ranges::sort(files);
    for (const auto& path : files) {
        const auto text = read_text(path);
        if (!text) {
            result.errors.push_back({path, "profile is unreadable", 0});
            continue;
        }
        auto parsed = parse_build_profile_v3(*text, path);
        if (!parsed.ok()) {
            if (parsed.error) result.errors.push_back(std::move(*parsed.error));
            continue;
        }
        if (!result.catalog.add(std::move(*parsed.profile))) {
            result.errors.push_back({path, "duplicate profile id or fingerprint", 0});
        }
    }
    return result;
}

std::string serialize_build_profile_v3(const BuildProfile& profile) {
    const bool topology = !profile.anchors.empty() || !profile.requirements.empty();
    const bool regions = std::ranges::any_of(profile.symbols, [](const auto& symbol) { return !symbol.region.empty(); });
    const int version = topology ? 3 : (regions ? 2 : 1);

    std::ostringstream out;
    out << "{\n"
        << "  \"version\": " << version << ",\n"
        << "  \"id\": \"" << escape_json(profile.id) << "\",\n"
        << "  \"fingerprint\": {\"sha256\": \"" << profile.fingerprint.hex()
        << "\", \"size\": " << profile.fingerprint.size << "},\n"
        << "  \"capabilities\": " << profile.capabilities << ",\n"
        << "  \"symbols\": [\n";

    for (std::size_t symbol_index = 0; symbol_index < profile.symbols.size(); ++symbol_index) {
        const auto& symbol = profile.symbols[symbol_index];
        out << "    {\"name\": \"" << escape_json(symbol.name) << "\"";
        if (!symbol.region.empty()) out << ", \"region\": \"" << escape_json(symbol.region) << "\"";
        out << ", \"candidates\": [";
        for (std::size_t candidate_index = 0; candidate_index < symbol.candidates.size(); ++candidate_index) {
            const auto& candidate = symbol.candidates[candidate_index];
            out << "{\"pattern\": \"" << candidate.pattern.text() << "\", \"adjustment\": "
                << candidate.adjustment << "}";
            if (candidate_index + 1 != symbol.candidates.size()) out << ", ";
        }
        out << "]}";
        if (symbol_index + 1 != profile.symbols.size()) out << ',';
        out << '\n';
    }
    out << "  ]";

    if (topology) {
        out << ",\n  \"anchors\": [\n";
        for (std::size_t index = 0; index < profile.anchors.size(); ++index) {
            const auto& anchor = profile.anchors[index];
            out << "    {\"name\": \"" << escape_json(anchor.name) << "\", \"symbol\": \""
                << escape_json(anchor.symbol) << "\"}";
            if (index + 1 != profile.anchors.size()) out << ',';
            out << '\n';
        }
        out << "  ],\n  \"requirements\": [\n";
        for (std::size_t index = 0; index < profile.requirements.size(); ++index) {
            const auto& requirement = profile.requirements[index];
            out << "    {\"capability\": " << requirement.capability << ", \"anchors\": [";
            for (std::size_t anchor_index = 0; anchor_index < requirement.anchors.size(); ++anchor_index) {
                out << "\"" << escape_json(requirement.anchors[anchor_index]) << "\"";
                if (anchor_index + 1 != requirement.anchors.size()) out << ", ";
            }
            out << "]}";
            if (index + 1 != profile.requirements.size()) out << ',';
            out << '\n';
        }
        out << "  ]";
    }

    out << "\n}\n";
    return out.str();
}

} // namespace enrage::core
