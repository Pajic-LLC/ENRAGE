#include "core/profile_matrix.hpp"
#include "platform/fingerprint.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

void place(std::vector<std::byte>& bytes, std::size_t offset) {
    const std::byte pattern[]{
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
        std::byte{0xEE}, std::byte{0xFF}, std::byte{0x11}, std::byte{0x22},
    };
    std::copy(std::begin(pattern), std::end(pattern), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

const enrage::core::MatrixCell& cell_named(
    const enrage::core::MatrixRow& row,
    std::string_view filename) {
    const auto found = std::ranges::find_if(row.cells, [&](const enrage::core::MatrixCell& cell) {
        return cell.image.filename() == filename;
    });
    assert(found != row.cells.end());
    return *found;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-matrix-test";
    const auto profiles = root / "profiles";
    const auto images = root / "images";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);
    std::filesystem::create_directories(images);

    std::vector<std::byte> target(256, std::byte{0x31});
    std::vector<std::byte> compatible(256, std::byte{0x42});
    std::vector<std::byte> ambiguous(256, std::byte{0x53});
    std::vector<std::byte> unrelated(256, std::byte{0x64});
    place(target, 80);
    place(compatible, 120);
    place(ambiguous, 40);
    place(ambiguous, 180);

    const auto target_path = images / "target.exe";
    write_bytes(target_path, target);
    write_bytes(images / "compatible.exe", compatible);
    write_bytes(images / "ambiguous.exe", ambiguous);
    write_bytes(images / "unrelated.exe", unrelated);

    const auto fingerprint = enrage::platform::fingerprint_file(target_path);
    assert(fingerprint.has_value());
    std::ofstream{profiles / "fixture.enp"}
        << "{\"version\":1,\"id\":\"matrix.fixture\","
        << "\"fingerprint\":{\"sha256\":\"" << fingerprint->hex()
        << "\",\"size\":" << fingerprint->size << "},"
        << "\"capabilities\":0,\"symbols\":[{\"name\":\"MatrixSymbol\","
        << "\"candidates\":[{\"pattern\":\"AA BB CC DD EE FF 11 22\",\"adjustment\":0}]}]}";

    const auto report = enrage::core::build_profile_matrix(profiles, images);
    assert(report.ok());
    assert(report.profiles_scanned == 1);
    assert(report.images_scanned == 4);
    assert(report.rows.size() == 1);

    const auto& row = report.rows.front();
    const auto& target_cell = cell_named(row, "target.exe");
    assert(target_cell.expected);
    assert(target_cell.state == enrage::platform::signature::ResolutionState::unique);
    assert(target_cell.offset == 80);

    const auto& compatible_cell = cell_named(row, "compatible.exe");
    assert(!compatible_cell.expected);
    assert(compatible_cell.state == enrage::platform::signature::ResolutionState::unique);
    assert(compatible_cell.offset == 120);

    const auto& ambiguous_cell = cell_named(row, "ambiguous.exe");
    assert(!ambiguous_cell.expected);
    assert(ambiguous_cell.state == enrage::platform::signature::ResolutionState::ambiguous);
    assert(ambiguous_cell.match_count == 2);

    const auto& unrelated_cell = cell_named(row, "unrelated.exe");
    assert(!unrelated_cell.expected);
    assert(unrelated_cell.state == enrage::platform::signature::ResolutionState::not_found);

    const auto json = enrage::core::profile_matrix_json(report);
    assert(json.find("\"expected\":true") != std::string::npos);
    assert(json.find("\"state\":\"ambiguous\"") != std::string::npos);

    std::filesystem::remove_all(root);
    return 0;
}
