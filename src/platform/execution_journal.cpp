#include "platform/execution_journal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace enrage::platform {
namespace {

constexpr std::array<std::byte, 4> journal_magic{
    std::byte{0x45}, std::byte{0x4e}, std::byte{0x4a}, std::byte{0x31}};
constexpr std::uint32_t journal_version = 1;
constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

void put_u32(std::byte* destination, std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4; ++index) {
        destination[index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

void put_u64(std::byte* destination, std::uint64_t value) noexcept {
    for (unsigned index = 0; index < 8; ++index) {
        destination[index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

std::uint32_t get_u32(const std::byte* source) noexcept {
    std::uint32_t value{};
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(source[index]))
                 << (index * 8);
    }
    return value;
}

std::uint64_t get_u64(const std::byte* source) noexcept {
    std::uint64_t value{};
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(source[index]))
                 << (index * 8);
    }
    return value;
}

std::uint64_t checksum(
    std::uint64_t sequence,
    std::uint32_t payload_size,
    std::string_view payload) noexcept {
    std::uint64_t value = fnv_offset;
    const auto mix = [&](unsigned char byte) {
        value ^= byte;
        value *= fnv_prime;
    };
    for (unsigned index = 0; index < 8; ++index) {
        mix(static_cast<unsigned char>((sequence >> (index * 8)) & 0xffu));
    }
    for (unsigned index = 0; index < 4; ++index) {
        mix(static_cast<unsigned char>((payload_size >> (index * 8)) & 0xffu));
    }
    for (const unsigned char byte : payload) mix(byte);
    return value;
}

std::array<std::byte, ExecutionJournal::header_size> make_header(
    std::uint64_t sequence,
    std::string_view payload) {
    std::array<std::byte, ExecutionJournal::header_size> header{};
    std::ranges::copy(journal_magic, header.begin());
    put_u32(header.data() + 4, journal_version);
    put_u64(header.data() + 8, sequence);
    const auto payload_size = static_cast<std::uint32_t>(payload.size());
    put_u32(header.data() + 16, payload_size);
    put_u32(header.data() + 20, 0);
    put_u64(header.data() + 24, checksum(sequence, payload_size, payload));
    return header;
}

struct ParsedSlot {
    std::uint64_t sequence{};
    std::string payload;
};

std::optional<ParsedSlot> parse_slot(
    const std::byte* slot,
    std::size_t available) {
    if (available < ExecutionJournal::slot_size) return std::nullopt;
    if (!std::ranges::equal(journal_magic, std::span{slot, journal_magic.size()})) {
        return std::nullopt;
    }
    if (get_u32(slot + 4) != journal_version) return std::nullopt;
    const auto sequence = get_u64(slot + 8);
    const auto payload_size = get_u32(slot + 16);
    if (sequence == 0 || payload_size > ExecutionJournal::payload_capacity) return std::nullopt;
    const auto expected = get_u64(slot + 24);
    const auto payload_begin = reinterpret_cast<const char*>(slot + ExecutionJournal::header_size);
    const std::string_view payload{payload_begin, payload_size};
    if (checksum(sequence, payload_size, payload) != expected) return std::nullopt;
    return ParsedSlot{sequence, std::string{payload}};
}

#ifdef _WIN32
bool write_at(HANDLE handle, std::uint64_t offset, const void* data, std::size_t size) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return false;
    const auto* cursor = static_cast<const std::byte*>(data);
    std::size_t remaining = size;
    while (remaining != 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 0xffffffffu));
        DWORD written{};
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}
#else
bool write_at(int descriptor, std::uint64_t offset, const void* data, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::byte*>(data);
    std::size_t remaining = size;
    auto position = static_cast<off_t>(offset);
    while (remaining != 0) {
        const auto written = ::pwrite(descriptor, cursor, remaining, position);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
        position += written;
    }
    return true;
}
#endif

} // namespace

ExecutionJournal::~ExecutionJournal() {
    close(false);
}

bool ExecutionJournal::open_new(
    const std::filesystem::path& path,
    std::string_view initial_payload) {
    if (is_open() || initial_payload.size() > payload_capacity) return false;

    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }

#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(file_size);
    if (!SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        CloseHandle(handle);
        return false;
    }
    handle_ = handle;
#else
    int flags = O_RDWR | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags, 0600);
    if (descriptor < 0) return false;
    if (::ftruncate(descriptor, static_cast<off_t>(file_size)) != 0) {
        ::close(descriptor);
        return false;
    }
    descriptor_ = descriptor;
#endif

    path_ = path;
    sequence_ = 0;
    if (!write(initial_payload)) {
        close(true);
        return false;
    }

#ifdef _WIN32
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        close(true);
        return false;
    }
#else
    if (::fsync(descriptor_) != 0) {
        close(true);
        return false;
    }
#endif
    return true;
}

bool ExecutionJournal::write(std::string_view payload) {
    if (!is_open() || payload.size() > payload_capacity ||
        sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    const auto next_sequence = sequence_ + 1;
    const auto slot_index = static_cast<std::size_t>((next_sequence - 1) % slot_count);
    const auto slot_offset = static_cast<std::uint64_t>(slot_index * slot_size);
    const auto header = make_header(next_sequence, payload);

#ifdef _WIN32
    const auto handle = static_cast<HANDLE>(handle_);
    if (!write_at(
            handle,
            slot_offset + header_size,
            payload.data(),
            payload.size()) ||
        !write_at(handle, slot_offset, header.data(), header.size())) {
        return false;
    }
#else
    if (!write_at(
            descriptor_,
            slot_offset + header_size,
            payload.data(),
            payload.size()) ||
        !write_at(descriptor_, slot_offset, header.data(), header.size())) {
        return false;
    }
#endif

    sequence_ = next_sequence;
    return true;
}

void ExecutionJournal::close(bool remove_file) noexcept {
#ifdef _WIN32
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
        descriptor_ = -1;
    }
#endif
    sequence_ = 0;
    if (remove_file && !path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    path_.clear();
}

bool ExecutionJournal::is_open() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return descriptor_ >= 0;
#endif
}

std::uint64_t ExecutionJournal::sequence() const noexcept {
    return sequence_;
}

ExecutionJournal::ReadResult ExecutionJournal::read_latest(
    const std::filesystem::path& path) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) return {};

    std::ifstream input(path, std::ios::binary);
    if (!input) return {};

    std::array<char, 4> prefix{};
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    const auto prefix_count = input.gcount();
    input.clear();
    input.seekg(0, std::ios::beg);

    const bool starts_with_magic =
        prefix_count == static_cast<std::streamsize>(prefix.size()) &&
        prefix[0] == 'E' && prefix[1] == 'N' && prefix[2] == 'J' && prefix[3] == '1';

    if (size != file_size) {
        return ReadResult{
            starts_with_magic ? ReadState::corrupt : ReadState::not_journal,
            std::nullopt};
    }

    std::vector<std::byte> bytes(file_size);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return ReadResult{ReadState::corrupt, std::nullopt};
    }

    const auto first = parse_slot(bytes.data(), slot_size);
    const auto second = parse_slot(bytes.data() + slot_size, slot_size);
    if (!first && !second) {
        return ReadResult{
            starts_with_magic ? ReadState::corrupt : ReadState::not_journal,
            std::nullopt};
    }

    const ParsedSlot* latest{};
    if (first && second) {
        latest = first->sequence >= second->sequence ? &*first : &*second;
    } else {
        latest = first ? &*first : &*second;
    }

    return ReadResult{
        ReadState::valid,
        Snapshot{latest->sequence, latest->payload}};
}

} // namespace enrage::platform
