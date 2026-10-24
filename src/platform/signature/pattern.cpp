#include "platform/signature/pattern.hpp"

#include <charconv>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace enrage::platform::signature {

Pattern::Pattern(std::vector<Byte> bytes) : bytes_(std::move(bytes)) {}

std::optional<Pattern> Pattern::parse(std::string_view text) {
    std::istringstream stream{std::string{text}};
    std::string token;
    std::vector<Byte> bytes;

    while (stream >> token) {
        if (token == "?" || token == "??") {
            bytes.push_back({0, true});
            continue;
        }

        if (token.size() != 2) {
            return std::nullopt;
        }

        unsigned value{};
        const auto* begin = token.data();
        const auto* end = begin + token.size();
        const auto result = std::from_chars(begin, end, value, 16);
        if (result.ec != std::errc{} || result.ptr != end || value > 0xffu) {
            return std::nullopt;
        }

        bytes.push_back({static_cast<std::uint8_t>(value), false});
    }

    if (bytes.empty()) {
        return std::nullopt;
    }

    return Pattern{std::move(bytes)};
}

std::size_t Pattern::size() const noexcept {
    return bytes_.size();
}

std::optional<std::size_t> Pattern::find(std::span<const std::byte> bytes) const noexcept {
    if (bytes_.size() > bytes.size()) {
        return std::nullopt;
    }

    for (std::size_t offset = 0; offset <= bytes.size() - bytes_.size(); ++offset) {
        bool matches = true;

        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            if (bytes_[index].wildcard) {
                continue;
            }

            if (std::to_integer<std::uint8_t>(bytes[offset + index]) != bytes_[index].value) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return offset;
        }
    }

    return std::nullopt;
}

std::vector<std::size_t> Pattern::find_all(std::span<const std::byte> bytes) const {
    std::vector<std::size_t> matches;
    if (bytes_.size() > bytes.size()) {
        return matches;
    }

    for (std::size_t offset = 0; offset <= bytes.size() - bytes_.size(); ++offset) {
        bool matched = true;
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            if (bytes_[index].wildcard) {
                continue;
            }
            if (std::to_integer<std::uint8_t>(bytes[offset + index]) != bytes_[index].value) {
                matched = false;
                break;
            }
        }
        if (matched) {
            matches.push_back(offset);
        }
    }

    return matches;
}

} // namespace enrage::platform::signature
