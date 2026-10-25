#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace enrage::platform {

class ExecutionJournal {
public:
    static constexpr std::size_t slot_count = 2;
    static constexpr std::size_t slot_size = 64 * 1024;
    static constexpr std::size_t header_size = 32;
    static constexpr std::size_t payload_capacity = slot_size - header_size;
    static constexpr std::size_t file_size = slot_count * slot_size;

    enum class ReadState {
        not_journal,
        valid,
        corrupt,
    };

    struct Snapshot {
        std::uint64_t sequence{};
        std::string payload;
    };

    struct ReadResult {
        ReadState state{ReadState::not_journal};
        std::optional<Snapshot> snapshot;
    };

    ExecutionJournal() = default;
    ~ExecutionJournal();

    ExecutionJournal(const ExecutionJournal&) = delete;
    ExecutionJournal& operator=(const ExecutionJournal&) = delete;

    [[nodiscard]] bool open_new(
        const std::filesystem::path& path,
        std::string_view initial_payload);
    [[nodiscard]] bool write(std::string_view payload);
    void close(bool remove_file = false) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;

    [[nodiscard]] static ReadResult read_latest(const std::filesystem::path& path);

private:
    std::filesystem::path path_;
    std::uint64_t sequence_{};
#ifdef _WIN32
    void* handle_{};
#else
    int descriptor_{-1};
#endif
};

} // namespace enrage::platform
