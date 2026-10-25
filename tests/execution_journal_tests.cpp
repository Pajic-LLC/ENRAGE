#include "platform/execution_journal.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void double_buffer_recovers_previous_valid_slot() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-execution-journal-test";
    const auto path = root / "execution.state";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    enrage::platform::ExecutionJournal journal;
    assert(journal.open_new(path, "version=1\n"));
    assert(journal.sequence() == 1);
    assert(std::filesystem::file_size(path) == enrage::platform::ExecutionJournal::file_size);

    auto latest = enrage::platform::ExecutionJournal::read_latest(path);
    assert(latest.state == enrage::platform::ExecutionJournal::ReadState::valid);
    assert(latest.snapshot.has_value());
    assert(latest.snapshot->sequence == 1);
    assert(latest.snapshot->payload == "version=1\n");

    assert(journal.write("version=1\nentry=first\n"));
    assert(journal.sequence() == 2);
    assert(journal.write("version=1\nentry=second\n"));
    assert(journal.sequence() == 3);
    assert(!std::filesystem::exists(path.string() + ".tmp"));
    journal.close(false);

    latest = enrage::platform::ExecutionJournal::read_latest(path);
    assert(latest.state == enrage::platform::ExecutionJournal::ReadState::valid);
    assert(latest.snapshot.has_value());
    assert(latest.snapshot->sequence == 3);
    assert(latest.snapshot->payload == "version=1\nentry=second\n");

    // Sequence 3 lives in slot zero. Damage its payload after the writer has
    // closed; the reader must reject that slot and fall back to sequence 2.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekg(static_cast<std::streamoff>(enrage::platform::ExecutionJournal::header_size));
        char byte{};
        file.read(&byte, 1);
        assert(file);
        file.seekp(static_cast<std::streamoff>(enrage::platform::ExecutionJournal::header_size));
        byte ^= 0x5a;
        file.write(&byte, 1);
        file.flush();
        assert(file);
    }

    latest = enrage::platform::ExecutionJournal::read_latest(path);
    assert(latest.state == enrage::platform::ExecutionJournal::ReadState::valid);
    assert(latest.snapshot.has_value());
    assert(latest.snapshot->sequence == 2);
    assert(latest.snapshot->payload == "version=1\nentry=first\n");

    std::filesystem::remove_all(root);
}

void rejects_oversize_payload_without_advancing() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-execution-journal-capacity-test";
    const auto path = root / "execution.state";
    std::filesystem::remove_all(root);

    enrage::platform::ExecutionJournal journal;
    assert(journal.open_new(path, "version=1\n"));
    const auto before = journal.sequence();
    const std::string oversized(enrage::platform::ExecutionJournal::payload_capacity + 1, 'x');
    assert(!journal.write(oversized));
    assert(journal.sequence() == before);
    journal.close(true);
    assert(!std::filesystem::exists(path));

    std::filesystem::remove_all(root);
}

void distinguishes_legacy_text_from_corrupt_journal() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-execution-journal-probe-test";
    const auto path = root / "execution.state";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::ofstream{path, std::ios::binary | std::ios::trunc} << "version=1\n";
    auto result = enrage::platform::ExecutionJournal::read_latest(path);
    assert(result.state == enrage::platform::ExecutionJournal::ReadState::not_journal);

    std::ofstream{path, std::ios::binary | std::ios::trunc} << "ENJ1broken";
    result = enrage::platform::ExecutionJournal::read_latest(path);
    assert(result.state == enrage::platform::ExecutionJournal::ReadState::corrupt);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    double_buffer_recovers_previous_valid_slot();
    rejects_oversize_payload_without_advancing();
    distinguishes_legacy_text_from_corrupt_journal();
    return 0;
}
