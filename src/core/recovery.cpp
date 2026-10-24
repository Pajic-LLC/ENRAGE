#include "core/recovery.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace enrage::core {
namespace {

char hex_digit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

std::string encode(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (const unsigned char ch : text) {
        out.push_back(hex_digit(ch >> 4));
        out.push_back(hex_digit(ch & 0x0f));
    }
    return out;
}

std::optional<unsigned char> nibble(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned char>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned char>(10 + ch - 'A');
    return std::nullopt;
}

std::optional<std::string> decode(std::string_view text) {
    if ((text.size() % 2) != 0) return std::nullopt;
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto high = nibble(text[i]);
        const auto low = nibble(text[i + 1]);
        if (!high || !low) return std::nullopt;
        out.push_back(static_cast<char>((*high << 4) | *low));
    }
    return out;
}

bool atomic_write(const std::filesystem::path& path, std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) return false;
    }

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::optional<std::string> read_active_mod(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    std::string line;
    bool version_ok = false;
    std::optional<std::string> active;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "version=1") {
            version_ok = true;
            continue;
        }
        constexpr std::string_view prefix = "active=";
        if (line.starts_with(prefix)) {
            const auto decoded = decode(std::string_view(line).substr(prefix.size()));
            if (!decoded) return std::nullopt;
            active = *decoded;
        }
    }
    if (!version_ok || !active) return std::nullopt;
    return active;
}

} // namespace

RecoveryManager::RecoveryManager(std::filesystem::path state_root)
    : state_root_(std::move(state_root)),
      session_path_(state_root_ / "session.state"),
      quarantine_path_(state_root_ / "quarantine.list") {}

bool RecoveryManager::begin_session(bool force_safe_mode) {
    if (session_active_) return false;

    safe_mode_ = force_safe_mode;
    previous_session_incomplete_ = false;
    suspected_mod_.reset();
    quarantined_.clear();

    std::error_code error;
    std::filesystem::create_directories(state_root_, error);
    if (error || !load_quarantine()) return false;
    if (!recover_previous_session()) return false;
    if (!write_session({})) return false;

    session_active_ = true;
    return true;
}

void RecoveryManager::end_session() noexcept {
    if (!session_active_) return;
    std::error_code error;
    std::filesystem::remove(session_path_, error);
    session_active_ = false;
}

bool RecoveryManager::enter_mod(std::string_view mod_id) {
    if (!session_active_ || mod_id.empty()) return false;
    return write_session(mod_id);
}

bool RecoveryManager::leave_mod() {
    if (!session_active_) return false;
    return write_session({});
}

bool RecoveryManager::safe_mode() const noexcept {
    return safe_mode_;
}

bool RecoveryManager::previous_session_incomplete() const noexcept {
    return previous_session_incomplete_;
}

const std::optional<std::string>& RecoveryManager::suspected_mod() const noexcept {
    return suspected_mod_;
}

bool RecoveryManager::quarantined(std::string_view mod_id) const noexcept {
    return std::ranges::find(quarantined_, mod_id) != quarantined_.end();
}

const std::vector<std::string>& RecoveryManager::quarantined_mods() const noexcept {
    return quarantined_;
}

bool RecoveryManager::clear_quarantine(std::string_view mod_id) {
    const auto old_size = quarantined_.size();
    std::erase(quarantined_, mod_id);
    if (quarantined_.size() == old_size) return false;
    return save_quarantine();
}

bool RecoveryManager::clear_all_quarantine() {
    if (quarantined_.empty()) return true;
    quarantined_.clear();
    return save_quarantine();
}

bool RecoveryManager::load_quarantine() {
    std::ifstream input(quarantine_path_, std::ios::binary);
    if (!input) {
        std::error_code error;
        return !std::filesystem::exists(quarantine_path_, error) && !error;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto decoded = decode(line);
        if (!decoded || decoded->empty()) return false;
        if (std::ranges::find(quarantined_, *decoded) == quarantined_.end()) {
            quarantined_.push_back(*decoded);
        }
    }
    std::ranges::sort(quarantined_);
    return input.good() || input.eof();
}

bool RecoveryManager::save_quarantine() const {
    std::string contents;
    for (const auto& mod : quarantined_) {
        contents += encode(mod);
        contents.push_back('\n');
    }
    return atomic_write(quarantine_path_, contents);
}

bool RecoveryManager::write_session(std::string_view active_mod) const {
    std::string contents = "version=1\nactive=";
    contents += encode(active_mod);
    contents.push_back('\n');
    return atomic_write(session_path_, contents);
}

bool RecoveryManager::recover_previous_session() {
    std::error_code error;
    if (!std::filesystem::exists(session_path_, error)) return !error;
    if (error) return false;

    previous_session_incomplete_ = true;
    const auto active = read_active_mod(session_path_);
    if (!active) {
        safe_mode_ = true;
        return true;
    }

    if (active->empty()) {
        safe_mode_ = true;
        return true;
    }

    suspected_mod_ = *active;
    if (!quarantined(*active)) {
        quarantined_.push_back(*active);
        std::ranges::sort(quarantined_);
        if (!save_quarantine()) return false;
    }
    return true;
}

} // namespace enrage::core
