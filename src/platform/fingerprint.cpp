#include "platform/fingerprint.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>

namespace enrage::platform {
namespace {

constexpr std::array<std::uint32_t, 64> k{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

class Sha256 {
public:
    void update(std::span<const std::uint8_t> bytes) {
        total_bytes_ += bytes.size();
        for (const auto byte : bytes) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bit_length = total_bytes_ * 8u;

        buffer_[buffer_size_++] = 0x80u;
        if (buffer_size_ > 56) {
            while (buffer_size_ < 64) buffer_[buffer_size_++] = 0;
            transform(buffer_);
            buffer_size_ = 0;
        }

        while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>((bit_length >> shift) & 0xffu);
        }
        transform(buffer_);

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
            digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
            digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
        }
        return digest;
    }

private:
    void transform(const std::array<std::uint8_t, 64>& block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto base = i * 4;
            words[i] = (static_cast<std::uint32_t>(block[base]) << 24) |
                       (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                       static_cast<std::uint32_t>(block[base + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];

        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + choice + k[i] + words[i];
            const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t total_bytes_{};
};

} // namespace

std::string FileFingerprint::hex() const {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : sha256) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

std::optional<FileFingerprint> fingerprint_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total{};

    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(count)});
            total += static_cast<std::uint64_t>(count);
        }
    }
    if (!input.eof()) return std::nullopt;

    return FileFingerprint{hash.finish(), total};
}

} // namespace enrage::platform
