#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace enrage::platform::signature {

struct Byte {
    std::uint8_t value{};
    bool wildcard{};
};

class Pattern {
public:
    static std::optional<Pattern> parse(std::string_view text);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<std::size_t> find(std::span<const std::byte> bytes) const noexcept;

private:
    explicit Pattern(std::vector<Byte> bytes);
    std::vector<Byte> bytes_;
};

} // namespace enrage::platform::signature
