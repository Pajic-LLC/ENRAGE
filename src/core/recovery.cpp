#include "core/recovery.hpp"

#include "core/plugin_host.hpp"
#include "platform/atomic_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace enrage::core {
namespace {

std::atomic_uint64_t execution_sequence{1};
std::atomic_uint64_t thread_sequence{1};
thread_local std::uint64_t current_execution_id{};
thread_local std::uint64_t current_thread_token{};
thread_local std::array<char, 17> current_crash_record{};

std::mutex crash_witness_mutex;

#ifdef _WIN32
HANDLE crash_handle = INVALID_HANDLE_VALUE;
LPTOP_LEVEL_EXCEPTION_FILTER previous_exception_filter{};

LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS*) {
    if (crash_handle != INVALID_HANDLE_VALUE && current_execution_id != 0) {
        DWORD written{};
        SetFilePointer(crash_handle, 0, nullptr, FILE_BEGIN);
        WriteFile(
            crash_handle,
            current_crash_record.data(),
            static_cast<DWORD>(current_crash_record.size()),
            &written,
            nullptr);
        FlushFileBuffers(crash_handle);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
int crash_fd = -1;
constexpr std::array<int, 5> fatal_signals{SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};
std::array<struct sigaction, fatal_signals.size()> previous_signal_actions{};

void crash_signal_handler(int signal_number) {
    if (crash_fd >= 0 && current_execution_id != 0) {
        (void)::lseek(crash_fd, 0, SEEK_SET);
        (void)::write(crash_fd, current_crash_record.data(), current_crash_record.size());
    }
    _exit(128 + signal_number);
}
#endif

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

bool parse_u64(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool atomic_write(const std::filesystem::path& path, std::string_view contents) {
    return platform::atomic_replace_file(path, contents);
}

struct SessionRecord {
    unsigned version{};
    std::optional<RecoveryBreadcrumb> legacy_breadcrumb;
};

std::optional<SessionRecord> read_session(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    unsigned version{};
    std::optional<std::string> legacy_active;
    std::optional<std::string> mod;
    std::optional<std::string> plugin;
    std::optional<std::string> callback;

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "version=1") {
            version = 1;
            continue;
        }
        if (line == "version=2") {
            version = 2;
            continue;
        }
        if (line == "version=3") {
            version = 3;
            continue;
        }

        const auto parse_field = [&](std::string_view prefix) -> std::optional<std::string> {
            if (!line.starts_with(prefix)) return std::nullopt;
            return decode(std::string_view(line).substr(prefix.size()));
        };

        if (line.starts_with("active=")) {
            legacy_active = parse_field("active=");
            if (!legacy_active) return std::nullopt;
        } else if (line.starts_with("mod=")) {
            mod = parse_field("mod=");
            if (!mod) return std::nullopt;
        } else if (line.starts_with("plugin=")) {
            plugin = parse_field("plugin=");
            if (!plugin) return std::nullopt;
        } else if (line.starts_with("callback=")) {
            callback = parse_field("callback=");
            if (!callback) return std::nullopt;
        }
    }

    if (!input.good() && !input.eof()) return std::nullopt;

    SessionRecord record{.version = version};
    if (version == 1) {
        if (!legacy_active) return std::nullopt;
        record.legacy_breadcrumb = RecoveryBreadcrumb{
            .mod_id = *legacy_active,
            .plugin_id = {},
            .callback = legacy_active->empty() ? std::string{} : std::string{"activation"},
        };
        return record;
    }

    if (version == 2) {
        if (!mod || !plugin || !callback) return std::nullopt;
        record.legacy_breadcrumb = RecoveryBreadcrumb{
            .mod_id = std::move(*mod),
            .plugin_id = std::move(*plugin),
            .callback = std::move(*callback),
        };
        return record;
    }

    if (version == 3) return record;
    return std::nullopt;
}

std::optional<std::vector<RecoveryBreadcrumb>> read_execution_ledger(
    const std::filesystem::path& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        if (exists_error) return std::nullopt;
        return std::vector<RecoveryBreadcrumb>{};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    bool version_seen = false;
    std::vector<RecoveryBreadcrumb> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line == "version=1") {
            if (version_seen) return std::nullopt;
            version_seen = true;
            continue;
        }
        if (!version_seen || !line.starts_with("entry=")) return std::nullopt;

        std::array<std::string_view, 7> fields{};
        std::string_view rest{line};
        rest.remove_prefix(6);
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto separator = rest.find('|');
            if (index + 1 == fields.size()) {
                if (separator != std::string_view::npos) return std::nullopt;
                fields[index] = rest;
            } else {
                if (separator == std::string_view::npos) return std::nullopt;
                fields[index] = rest.substr(0, separator);
                rest.remove_prefix(separator + 1);
            }
        }

        RecoveryBreadcrumb entry;
        if (!parse_u64(fields[0], entry.execution_id) || entry.execution_id == 0 ||
            !parse_u64(fields[1], entry.thread_id) || entry.thread_id == 0 ||
            !parse_u64(fields[2], entry.generation) ||
            !parse_u64(fields[3], entry.worker_id)) {
            return std::nullopt;
        }
        const auto mod = decode(fields[4]);
        const auto plugin = decode(fields[5]);
        const auto callback = decode(fields[6]);
        if (!mod || !plugin || !callback || mod->empty() || callback->empty()) return std::nullopt;
        entry.mod_id = *mod;
        entry.plugin_id = *plugin;
        entry.callback = *callback;
        entries.push_back(std::move(entry));
    }

    if (!input.good() && !input.eof()) return std::nullopt;
    if (!version_seen) return std::nullopt;
    return entries;
}

std::optional<std::uint64_t> read_crash_execution_id(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    std::array<char, 17> record{};
    input.read(record.data(), static_cast<std::streamsize>(record.size()));
    if (input.gcount() != static_cast<std::streamsize>(record.size()) || record[16] != '\n') {
        return std::nullopt;
    }

    std::uint64_t value{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto digit = nibble(record[index]);
        if (!digit) return std::nullopt;
        value = (value << 4) | *digit;
    }
    if (value == 0) return std::nullopt;
    return value;
}

void set_current_crash_record(std::uint64_t execution_id) noexcept {
    current_execution_id = execution_id;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto shift = static_cast<unsigned>((15 - index) * 4);
        current_crash_record[index] = hex_digit(static_cast<unsigned>((execution_id >> shift) & 0x0f));
    }
    current_crash_record[16] = '\n';
}

void clear_current_crash_record() noexcept {
    current_execution_id = 0;
    current_crash_record.fill(0);
}

std::uint64_t next_nonzero(std::atomic_uint64_t& sequence) noexcept {
    auto value = sequence.fetch_add(1, std::memory_order_relaxed);
    if (value != 0) return value;
    value = sequence.fetch_add(1, std::memory_order_relaxed);
    return value;
}

} // namespace

RecoveryManager::RecoveryManager(std::filesystem::path state_root)
    : state_root_(std::move(state_root)),
      session_path_(state_root_ / "session.state"),
      execution_path_(state_root_ / "execution.state"),
      crash_path_(state_root_ / "crash.state"),
      quarantine_path_(state_root_ / "quarantine.list") {}

RecoveryManager::~RecoveryManager() {
    // Destruction without end_session() models an interrupted process in the
    // original recovery contract. Uninstall process-local hooks, but preserve
    // session/ledger/crash files for the next RecoveryManager to inspect.
    if (crash_witness_active_) end_crash_witness();
    session_active_ = false;
    clear_current_crash_record();
}

bool RecoveryManager::begin_session(bool force_safe_mode) {
    if (session_active_) return false;

    safe_mode_ = force_safe_mode;
    previous_session_incomplete_ = false;
    suspected_mod_.reset();
    suspected_execution_.reset();
    active_executions_at_crash_.clear();
    quarantined_.clear();

    std::error_code error;
    std::filesystem::create_directories(state_root_, error);
    if (error || !load_quarantine()) return false;
    if (!recover_previous_session()) return false;

    std::filesystem::remove(execution_path_, error);
    error.clear();
    std::filesystem::remove(crash_path_, error);
    error.clear();

    if (!write_session_marker()) return false;
    if (!begin_crash_witness()) {
        std::filesystem::remove(session_path_, error);
        return false;
    }

    session_active_ = true;
    return true;
}

void RecoveryManager::end_session() noexcept {
    if (!session_active_ && !crash_witness_active_) return;

    end_crash_witness();
    {
        std::scoped_lock lock(execution_mutex_);
        active_executions_.clear();
    }
    clear_current_crash_record();

    std::error_code error;
    std::filesystem::remove(session_path_, error);
    error.clear();
    std::filesystem::remove(execution_path_, error);
    error.clear();
    std::filesystem::remove(crash_path_, error);
    session_active_ = false;
}

bool RecoveryManager::enter_mod(std::string_view mod_id) {
    return enter_execution(mod_id, {}, "activation");
}

bool RecoveryManager::leave_mod() {
    return leave_execution();
}

bool RecoveryManager::enter_execution(
    std::string_view mod_id,
    std::string_view plugin_id,
    std::string_view callback,
    std::uint64_t generation,
    std::uint64_t worker_id) {
    if (!session_active_ || mod_id.empty() || callback.empty() || current_execution_id != 0) {
        return false;
    }

    if (const auto context = PluginHost::current_execution_context();
        context && context->mod_id == mod_id && context->plugin_id == plugin_id &&
        context->callback == callback) {
        if (generation == 0) generation = context->generation;
        if (worker_id == 0) worker_id = context->worker_id;
    }

    if (current_thread_token == 0) current_thread_token = next_nonzero(thread_sequence);
    const auto execution_id = next_nonzero(execution_sequence);
    if (execution_id == 0 || current_thread_token == 0) return false;

    RecoveryBreadcrumb breadcrumb{
        .mod_id = std::string{mod_id},
        .plugin_id = std::string{plugin_id},
        .callback = std::string{callback},
        .execution_id = execution_id,
        .thread_id = current_thread_token,
        .generation = generation,
        .worker_id = worker_id,
    };

    {
        std::scoped_lock lock(execution_mutex_);
        active_executions_.push_back(breadcrumb);
        if (!write_execution_ledger_locked()) {
            active_executions_.pop_back();
            return false;
        }
    }

    set_current_crash_record(execution_id);
    return true;
}

bool RecoveryManager::leave_execution() {
    if (!session_active_ || current_execution_id == 0) return false;
    const auto leaving_id = current_execution_id;

    bool written = false;
    {
        std::scoped_lock lock(execution_mutex_);
        const auto found = std::ranges::find(
            active_executions_,
            leaving_id,
            &RecoveryBreadcrumb::execution_id);
        if (found == active_executions_.end()) return false;
        active_executions_.erase(found);
        written = write_execution_ledger_locked();
    }

    clear_current_crash_record();
    return written;
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

const std::optional<RecoveryBreadcrumb>& RecoveryManager::suspected_execution() const noexcept {
    return suspected_execution_;
}

const std::vector<RecoveryBreadcrumb>& RecoveryManager::active_executions_at_crash() const noexcept {
    return active_executions_at_crash_;
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

bool RecoveryManager::write_session_marker() const {
    return atomic_write(session_path_, "version=3\n");
}

bool RecoveryManager::write_execution_ledger_locked() const {
    if (active_executions_.empty()) {
        std::error_code error;
        std::filesystem::remove(execution_path_, error);
        return !error;
    }

    std::string contents = "version=1\n";
    for (const auto& entry : active_executions_) {
        contents += "entry=";
        contents += std::to_string(entry.execution_id);
        contents.push_back('|');
        contents += std::to_string(entry.thread_id);
        contents.push_back('|');
        contents += std::to_string(entry.generation);
        contents.push_back('|');
        contents += std::to_string(entry.worker_id);
        contents.push_back('|');
        contents += encode(entry.mod_id);
        contents.push_back('|');
        contents += encode(entry.plugin_id);
        contents.push_back('|');
        contents += encode(entry.callback);
        contents.push_back('\n');
    }
    return atomic_write(execution_path_, contents);
}

bool RecoveryManager::recover_previous_session() {
    std::error_code error;
    if (!std::filesystem::exists(session_path_, error)) return !error;
    if (error) return false;

    previous_session_incomplete_ = true;
    const auto session = read_session(session_path_);
    if (!session) {
        safe_mode_ = true;
        return true;
    }

    if (session->version <= 2) {
        if (!session->legacy_breadcrumb || session->legacy_breadcrumb->mod_id.empty()) {
            safe_mode_ = true;
            return true;
        }
        suspected_execution_ = *session->legacy_breadcrumb;
    } else {
        const auto ledger = read_execution_ledger(execution_path_);
        if (!ledger) {
            safe_mode_ = true;
            return true;
        }
        active_executions_at_crash_ = *ledger;

        const auto crash_execution = read_crash_execution_id(crash_path_);
        if (crash_execution) {
            const auto found = std::ranges::find(
                active_executions_at_crash_,
                *crash_execution,
                &RecoveryBreadcrumb::execution_id);
            if (found != active_executions_at_crash_.end()) suspected_execution_ = *found;
        }

        // Hard exits such as _Exit bypass fatal signal/exception hooks. Preserve
        // the original exact-attribution behavior when only one execution was live.
        if (!suspected_execution_ && active_executions_at_crash_.size() == 1) {
            suspected_execution_ = active_executions_at_crash_.front();
        }

        // Multiple live executions without a crash witness are genuinely
        // ambiguous. Do not falsely quarantine an arbitrary mod.
        if (!suspected_execution_) {
            safe_mode_ = true;
            return true;
        }
    }

    suspected_mod_ = suspected_execution_->mod_id;
    if (!quarantined(*suspected_mod_)) {
        quarantined_.push_back(*suspected_mod_);
        std::ranges::sort(quarantined_);
        if (!save_quarantine()) return false;
    }
    return true;
}

bool RecoveryManager::begin_crash_witness() {
    std::scoped_lock lock(crash_witness_mutex);
    if (crash_witness_active_) return true;

#ifdef _WIN32
    if (crash_handle != INVALID_HANDLE_VALUE) return false;
    crash_handle = CreateFileW(
        crash_path_.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (crash_handle == INVALID_HANDLE_VALUE) return false;
    previous_exception_filter = SetUnhandledExceptionFilter(&crash_exception_filter);
#else
    if (crash_fd >= 0) return false;
    crash_fd = ::open(crash_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (crash_fd < 0) return false;

    struct sigaction action{};
    action.sa_handler = &crash_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    for (std::size_t index = 0; index < fatal_signals.size(); ++index) {
        if (sigaction(fatal_signals[index], &action, &previous_signal_actions[index]) != 0) {
            for (std::size_t rollback = 0; rollback < index; ++rollback) {
                (void)sigaction(
                    fatal_signals[rollback],
                    &previous_signal_actions[rollback],
                    nullptr);
            }
            ::close(crash_fd);
            crash_fd = -1;
            return false;
        }
    }
#endif

    crash_witness_active_ = true;
    return true;
}

void RecoveryManager::end_crash_witness() noexcept {
    std::scoped_lock lock(crash_witness_mutex);
    if (!crash_witness_active_) return;

#ifdef _WIN32
    SetUnhandledExceptionFilter(previous_exception_filter);
    previous_exception_filter = nullptr;
    if (crash_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(crash_handle);
        crash_handle = INVALID_HANDLE_VALUE;
    }
#else
    for (std::size_t index = 0; index < fatal_signals.size(); ++index) {
        (void)sigaction(fatal_signals[index], &previous_signal_actions[index], nullptr);
    }
    if (crash_fd >= 0) {
        ::close(crash_fd);
        crash_fd = -1;
    }
#endif

    crash_witness_active_ = false;
}

} // namespace enrage::core
