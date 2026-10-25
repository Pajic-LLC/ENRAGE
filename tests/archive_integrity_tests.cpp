#include "core/archive.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

void write_u32(std::ostream& stream, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        stream.put(static_cast<char>((value >> (index * 8)) & 0xff));
    }
}

void write_u64(std::ostream& stream, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        stream.put(static_cast<char>((value >> (index * 8)) & 0xff));
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_temporary_artifact(
    const std::filesystem::path& directory,
    std::string_view destination_name) {
    std::error_code error;
    const auto prefix = std::string{destination_name} + ".enrage-";
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end;
         it.increment(error)) {
        if (it->path().filename().string().starts_with(prefix)) return true;
    }
    return static_cast<bool>(error);
}

void rejects_overlapping_payloads() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-archive-overlap-test";
    const auto package = root / "overlap.enr";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    constexpr std::string_view first_path = "a.bin";
    constexpr std::string_view second_path = "b.bin";
    constexpr std::uint64_t index_bytes =
        (20 + first_path.size()) + (20 + second_path.size());
    constexpr std::uint64_t data_start = 20 + index_bytes;

    std::ofstream output(package, std::ios::binary | std::ios::trunc);
    output.write("ENR1", 4);
    write_u32(output, enrage::core::Archive::format_version);
    write_u32(output, 2);
    write_u64(output, index_bytes);

    write_u32(output, static_cast<std::uint32_t>(first_path.size()));
    write_u64(output, data_start);
    write_u64(output, 4);
    output.write(first_path.data(), static_cast<std::streamsize>(first_path.size()));

    write_u32(output, static_cast<std::uint32_t>(second_path.size()));
    write_u64(output, data_start + 2);
    write_u64(output, 4);
    output.write(second_path.data(), static_cast<std::streamsize>(second_path.size()));

    output.write("abcdef", 6);
    output.close();

    assert(!enrage::core::Archive::open(package).has_value());
    std::filesystem::remove_all(root);
}

void replaces_existing_archive_transactionally() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-archive-replace-test";
    const auto source = root / "source";
    const auto package = root / "stable.enr";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(source / "files");

    std::ofstream{source / "mod.json"} << R"({"id":"example.transaction"})";
    std::ofstream{source / "files" / "value.txt"} << "generation-one";
    assert(enrage::core::Archive::create(source, package));

    std::ofstream{source / "files" / "value.txt", std::ios::trunc} << "generation-two";
    assert(enrage::core::Archive::create(source, package));

    const auto archive = enrage::core::Archive::open(package);
    assert(archive.has_value());
    assert(archive->read_text("files/value.txt") == "generation-two");
    assert(!has_temporary_artifact(root, package.filename().string()));

    std::filesystem::remove_all(root);
}

void failed_pack_preserves_previous_destination() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-archive-rollback-test";
    const auto source = root / "source";
    const auto files = source / "files";
    const auto package = root / "stable.enr";
    const auto large = files / "00-large.bin";
    const auto late = files / "zz-late.bin";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(files);

    std::ofstream{source / "mod.json"} << R"({"id":"example.rollback"})";
    {
        std::ofstream large_output(large, std::ios::binary | std::ios::trunc);
        constexpr std::uint64_t large_size = 64ull * 1024ull * 1024ull;
        large_output.seekp(static_cast<std::streamoff>(large_size - 1));
        large_output.put('\0');
    }
    std::ofstream{late, std::ios::binary | std::ios::trunc} << "must-disappear";
    std::ofstream{package, std::ios::binary | std::ios::trunc} << "previous-destination";

    std::atomic_bool pack_finished{false};
    std::atomic_bool temporary_seen{false};
    std::thread mutator([&] {
        const auto prefix = package.filename().string() + ".enrage-pack-";
        while (!pack_finished.load(std::memory_order_acquire)) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(root, error), end;
                 !error && it != end;
                 it.increment(error)) {
                if (!it->path().filename().string().starts_with(prefix)) continue;
                temporary_seen.store(true, std::memory_order_release);
                std::filesystem::remove(late, error);
                return;
            }
            std::this_thread::yield();
        }
    });

    const bool created = enrage::core::Archive::create(source, package);
    pack_finished.store(true, std::memory_order_release);
    mutator.join();

    assert(temporary_seen.load(std::memory_order_acquire));
    assert(!created);
    assert(read_file(package) == "previous-destination");
    assert(!has_temporary_artifact(root, package.filename().string()));

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    rejects_overlapping_payloads();
    replaces_existing_archive_transactionally();
    failed_pack_preserves_previous_destination();
    return 0;
}
