#include "core/plugin_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace enrage::core {
namespace {

std::atomic_uint64_t shadow_sequence{};
std::atomic_uint64_t worker_sequence{1};
constexpr std::size_t max_reload_state_bytes = 16u * 1024u * 1024u;
constexpr auto worker_stop_timeout = std::chrono::seconds{1};

bool valid_descriptor(const enrage_plugin_descriptor* descriptor) {
    return descriptor &&
           descriptor->abi_version == ENRAGE_ABI_VERSION &&
           descriptor->struct_size >= ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE &&
           descriptor->id && descriptor->name && descriptor->version &&
           descriptor->load;
}

template <typename Callback>
Callback descriptor_callback(
    const enrage_plugin_descriptor* descriptor,
    std::size_t offset) {
    if (!descriptor || descriptor->struct_size < offset + sizeof(Callback)) return nullptr;
    Callback callback{};
    const auto* bytes = reinterpret_cast<const std::byte*>(descriptor);
    std::memcpy(&callback, bytes + offset, sizeof(callback));
    return callback;
}

enrage_plugin_save_state_callback save_state_callback(
    const enrage_plugin_descriptor* descriptor) {
    return descriptor_callback<enrage_plugin_save_state_callback>(
        descriptor,
        offsetof(enrage_plugin_descriptor, save_state));
}

enrage_plugin_restore_state_callback restore_state_callback(
    const enrage_plugin_descriptor* descriptor) {
    return descriptor_callback<enrage_plugin_restore_state_callback>(
        descriptor,
        offsetof(enrage_plugin_descriptor, restore_state));
}

} // namespace

thread_local const PluginHost* PluginHost::callback_host_ = nullptr;
thread_local PluginHost* PluginHost::registration_host_ = nullptr;
thread_local std::string PluginHost::registration_mod_id_;
thread_local std::string PluginHost::registration_plugin_id_;
thread_local std::vector<PluginHost::TickRegistration>* PluginHost::registration_target_ = nullptr;
thread_local std::vector<PluginHost::PendingWorker>* PluginHost::registration_worker_target_ = nullptr;

PluginHost::CallbackGuard::CallbackGuard(const PluginHost* host) noexcept
    : previous_(callback_host_) {
    callback_host_ = host;
}

PluginHost::CallbackGuard::~CallbackGuard() {
    callback_host_ = previous_;
}

bool PluginHost::callback_active_on_this_thread() const noexcept {
    return callback_host_ == this;
}

PluginHost::PluginHost() {
    api_.abi_version = ENRAGE_ABI_VERSION;
    api_.struct_size = static_cast<uint32_t>(sizeof(enrage_host_api));
    api_.context = this;
    api_.log = &PluginHost::log_thunk;
    api_.capabilities = &PluginHost::capabilities_thunk;
    api_.register_tick = &PluginHost::register_tick_thunk;
    api_.vfs_read = &PluginHost::vfs_read_thunk;
    api_.start_worker = &PluginHost::start_worker_thunk;
    api_.worker_stop_requested = &PluginHost::worker_stop_requested_thunk;
}

PluginHost::~PluginHost() {
    unload_all();
}

std::optional<std::filesystem::path> PluginHost::create_shadow_copy(
    const std::filesystem::path& source) const {
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) return std::nullopt;

    const auto parent = source.parent_path();
    const auto stem = source.stem().string();
    const auto extension = source.extension().string();
    const auto time_seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    for (std::uint64_t attempt = 0; attempt < 16; ++attempt) {
        const auto sequence = shadow_sequence.fetch_add(1, std::memory_order_relaxed);
        const auto nonce = time_seed ^ (sequence + attempt * 0x9e3779b97f4a7c15ull);
        const auto shadow = parent /
            (stem + ".enrage-shadow-" + std::to_string(nonce) + extension);
        error.clear();
        if (std::filesystem::copy_file(
                source,
                shadow,
                std::filesystem::copy_options::none,
                error)) {
            return shadow;
        }
        if (error != std::errc::file_exists) return std::nullopt;
    }
    return std::nullopt;
}

void PluginHost::cleanup_loaded_path(const std::filesystem::path& path) const noexcept {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::optional<PluginHost::PreparedPlugin> PluginHost::prepare_plugin(
    const std::filesystem::path& source,
    std::string_view owner_mod_id) const {
    const auto shadow = create_shadow_copy(source);
    if (!shadow) return std::nullopt;

    platform::DynamicLibrary library;
    if (!library.open(*shadow)) {
        cleanup_loaded_path(*shadow);
        return std::nullopt;
    }

    auto* raw_entry = library.symbol(ENRAGE_PLUGIN_ENTRY_NAME);
    if (!raw_entry) {
        library.close();
        cleanup_loaded_path(*shadow);
        return std::nullopt;
    }

    const auto entry = reinterpret_cast<enrage_plugin_entry_fn>(raw_entry);
    const auto* descriptor = entry();
    if (!valid_descriptor(descriptor)) {
        library.close();
        cleanup_loaded_path(*shadow);
        return std::nullopt;
    }

    PreparedPlugin prepared;
    prepared.source_path = source;
    prepared.loaded_path = *shadow;
    prepared.owner_mod_id = std::string{owner_mod_id};
    prepared.id = descriptor->id;
    prepared.name = descriptor->name;
    prepared.version = descriptor->version;
    prepared.descriptor = descriptor;
    prepared.library = std::move(library);
    return prepared;
}

bool PluginHost::stage_workers(
    const std::vector<PendingWorker>& pending,
    std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept {
    try {
        staged.reserve(pending.size());
        for (const auto& request : pending) {
            auto worker = std::make_unique<WorkerRegistration>();
            worker->id = request.id;
            worker->main_callback = request.main_callback;
            worker->request_stop_callback = request.request_stop_callback;
            worker->user_data = request.user_data;
            worker->owner_mod_id = request.owner_mod_id;
            worker->plugin_id = request.plugin_id;

            auto* registration = worker.get();
            registration->thread = std::thread([this, registration] {
                {
                    std::unique_lock state_lock(registration->state_mutex);
                    registration->state_cv.wait(state_lock, [registration] {
                        return registration->start_allowed || registration->cancel_before_start;
                    });
                    if (registration->cancel_before_start) {
                        registration->finished = true;
                        state_lock.unlock();
                        registration->state_cv.notify_all();
                        return;
                    }
                }

                try {
                    CallbackGuard callback_guard{this};
                    registration->main_callback(registration->id, registration->user_data);
                } catch (...) {
                    // A plugin exception must not escape the host-owned thread.
                }

                {
                    std::scoped_lock state_lock(registration->state_mutex);
                    registration->finished = true;
                }
                registration->state_cv.notify_all();
            });
            staged.push_back(std::move(worker));
        }
        return true;
    } catch (...) {
        cancel_staged_workers(staged);
        return false;
    }
}

bool PluginHost::reserve_worker_capacity(std::size_t additional) noexcept {
    try {
        std::scoped_lock lock(worker_mutex_);
        if (additional > workers_.max_size() - workers_.size()) return false;
        workers_.reserve(workers_.size() + additional);
        return true;
    } catch (...) {
        return false;
    }
}

void PluginHost::cancel_staged_workers(
    std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept {
    for (auto& worker : staged) {
        if (!worker) continue;
        {
            std::scoped_lock state_lock(worker->state_mutex);
            worker->cancel_before_start = true;
        }
        worker->state_cv.notify_all();
    }
    for (auto& worker : staged) {
        if (worker && worker->thread.joinable()) worker->thread.join();
    }
    staged.clear();
}

void PluginHost::publish_workers(
    std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept {
    {
        std::scoped_lock lock(worker_mutex_);
        for (auto& worker : staged) {
            auto* registration = worker.get();
            workers_.push_back(std::move(worker));
            {
                std::scoped_lock state_lock(registration->state_mutex);
                registration->start_allowed = true;
            }
            registration->state_cv.notify_all();
        }
    }
    staged.clear();
}

bool PluginHost::stop_workers(
    std::string_view owner_mod_id,
    std::string_view plugin_id,
    bool wait_forever) noexcept {
    for (const auto& worker : workers_) {
        if (worker->owner_mod_id == owner_mod_id && worker->plugin_id == plugin_id) {
            worker->stop_requested.store(true, std::memory_order_release);
        }
    }

    for (const auto& worker : workers_) {
        if (worker->owner_mod_id != owner_mod_id || worker->plugin_id != plugin_id ||
            !worker->request_stop_callback) {
            continue;
        }
        try {
            CallbackGuard callback_guard{this};
            worker->request_stop_callback(worker->id, worker->user_data);
        } catch (...) {
            // The stop flag is authoritative; a throwing wake callback does not
            // permit the generation to unload while its worker is still alive.
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + worker_stop_timeout;
    for (const auto& worker : workers_) {
        if (worker->owner_mod_id != owner_mod_id || worker->plugin_id != plugin_id) continue;

        std::unique_lock state_lock(worker->state_mutex);
        if (wait_forever) {
            worker->state_cv.wait(state_lock, [&worker] { return worker->finished; });
        } else if (!worker->state_cv.wait_until(
                       state_lock,
                       deadline,
                       [&worker] { return worker->finished; })) {
            return false;
        }
    }

    for (const auto& worker : workers_) {
        if (worker->owner_mod_id == owner_mod_id && worker->plugin_id == plugin_id &&
            worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    {
        std::scoped_lock lock(worker_mutex_);
        std::erase_if(workers_, [&](const std::unique_ptr<WorkerRegistration>& worker) {
            return worker->owner_mod_id == owner_mod_id && worker->plugin_id == plugin_id;
        });
    }
    return true;
}

bool PluginHost::load(const std::filesystem::path& path, std::string_view owner_mod_id) {
    if (callback_active_on_this_thread()) return false;
    std::scoped_lock lock(lifecycle_mutex_);

    auto prepared = prepare_plugin(path, owner_mod_id);
    if (!prepared) return false;

    const auto duplicate = std::ranges::find(plugins_, prepared->id, &LoadedPlugin::id);
    if (duplicate != plugins_.end()) {
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    if (execution_begin_ && !owner_mod_id.empty() &&
        !execution_begin_(owner_mod_id, prepared->id, "load")) {
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    const auto tick_count_before_load = tick_callbacks_.size();
    std::vector<PendingWorker> pending_workers;
    registration_host_ = this;
    registration_mod_id_ = std::string{owner_mod_id};
    registration_plugin_id_ = prepared->id;
    registration_target_ = nullptr;
    registration_worker_target_ = &pending_workers;

    enrage_result load_result{};
    {
        CallbackGuard callback_guard{this};
        load_result = prepared->descriptor->load(&api_);
    }

    registration_host_ = nullptr;
    registration_mod_id_.clear();
    registration_plugin_id_.clear();
    registration_target_ = nullptr;
    registration_worker_target_ = nullptr;

    const bool recovery_cleared = !execution_end_ || owner_mod_id.empty() || execution_end_();
    if (load_result != ENRAGE_OK || !recovery_cleared) {
        tick_callbacks_.resize(tick_count_before_load);
        if (load_result == ENRAGE_OK && prepared->descriptor->unload) {
            CallbackGuard callback_guard{this};
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    std::vector<std::unique_ptr<WorkerRegistration>> staged_workers;
    if (!stage_workers(pending_workers, staged_workers) ||
        !reserve_worker_capacity(staged_workers.size())) {
        cancel_staged_workers(staged_workers);
        tick_callbacks_.resize(tick_count_before_load);
        if (prepared->descriptor->unload) {
            CallbackGuard callback_guard{this};
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    LoadedPlugin loaded;
    loaded.path = std::move(prepared->source_path);
    loaded.loaded_path = std::move(prepared->loaded_path);
    loaded.owner_mod_id = std::move(prepared->owner_mod_id);
    loaded.id = std::move(prepared->id);
    loaded.name = std::move(prepared->name);
    loaded.version = std::move(prepared->version);
    loaded.descriptor = prepared->descriptor;
    loaded.library = std::move(prepared->library);
    plugins_.push_back(std::move(loaded));
    publish_workers(staged_workers);
    return true;
}

bool PluginHost::reload(
    std::string_view plugin_id,
    const std::optional<std::filesystem::path>& replacement_path) {
    if (callback_active_on_this_thread()) return false;
    std::scoped_lock lock(lifecycle_mutex_);

    const auto found = std::ranges::find(plugins_, plugin_id, &LoadedPlugin::id);
    if (found == plugins_.end()) return false;
    const auto index = static_cast<std::size_t>(std::distance(plugins_.begin(), found));

    const auto source = replacement_path ? *replacement_path : found->path;
    auto prepared = prepare_plugin(source, found->owner_mod_id);
    if (!prepared || prepared->id != found->id) {
        if (prepared) {
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
        }
        return false;
    }

    const auto save_state = save_state_callback(found->descriptor);
    const auto restore_state = restore_state_callback(prepared->descriptor);
    const bool handoff_state = save_state && restore_state;
    std::vector<std::byte> reload_state;

    if (handoff_state) {
        if (execution_begin_ && !found->owner_mod_id.empty() &&
            !execution_begin_(found->owner_mod_id, found->id, "reload-save-state")) {
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }

        std::size_t required_size{};
        enrage_result state_result{};
        {
            CallbackGuard callback_guard{this};
            state_result = save_state(nullptr, 0, &required_size);
        }
        if ((state_result == ENRAGE_OK || state_result == ENRAGE_BUFFER_TOO_SMALL) &&
            required_size <= max_reload_state_bytes) {
            reload_state.resize(required_size);
            if (required_size != 0) {
                std::size_t written_size = required_size;
                {
                    CallbackGuard callback_guard{this};
                    state_result = save_state(reload_state.data(), reload_state.size(), &written_size);
                }
                if (state_result == ENRAGE_OK && written_size <= reload_state.size()) {
                    reload_state.resize(written_size);
                } else {
                    state_result = ENRAGE_INTERNAL_ERROR;
                }
            } else if (state_result == ENRAGE_BUFFER_TOO_SMALL) {
                state_result = ENRAGE_INTERNAL_ERROR;
            }
        } else {
            state_result = ENRAGE_INTERNAL_ERROR;
        }

        const bool state_recovery_cleared =
            !execution_end_ || found->owner_mod_id.empty() || execution_end_();
        if (state_result != ENRAGE_OK || !state_recovery_cleared) {
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }
    }

    std::vector<TickRegistration> staged_callbacks;
    std::vector<PendingWorker> pending_workers;
    if (execution_begin_ && !found->owner_mod_id.empty() &&
        !execution_begin_(found->owner_mod_id, found->id, "reload-load")) {
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    registration_host_ = this;
    registration_mod_id_ = found->owner_mod_id;
    registration_plugin_id_ = found->id;
    registration_target_ = &staged_callbacks;
    registration_worker_target_ = &pending_workers;

    enrage_result load_result{};
    {
        CallbackGuard callback_guard{this};
        load_result = prepared->descriptor->load(&api_);
    }

    registration_host_ = nullptr;
    registration_mod_id_.clear();
    registration_plugin_id_.clear();
    registration_target_ = nullptr;
    registration_worker_target_ = nullptr;

    const bool load_recovery_cleared =
        !execution_end_ || found->owner_mod_id.empty() || execution_end_();
    if (load_result != ENRAGE_OK || !load_recovery_cleared) {
        if (load_result == ENRAGE_OK && prepared->descriptor->unload) {
            CallbackGuard callback_guard{this};
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    if (handoff_state) {
        if (execution_begin_ && !found->owner_mod_id.empty() &&
            !execution_begin_(found->owner_mod_id, found->id, "reload-restore-state")) {
            if (prepared->descriptor->unload) {
                CallbackGuard callback_guard{this};
                prepared->descriptor->unload();
            }
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }

        enrage_result restore_result{};
        {
            CallbackGuard callback_guard{this};
            restore_result = restore_state(
                reload_state.empty() ? nullptr : reload_state.data(),
                reload_state.size());
        }
        const bool restore_recovery_cleared =
            !execution_end_ || found->owner_mod_id.empty() || execution_end_();
        if (restore_result != ENRAGE_OK || !restore_recovery_cleared) {
            if (prepared->descriptor->unload) {
                CallbackGuard callback_guard{this};
                prepared->descriptor->unload();
            }
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }
    }

    std::vector<std::unique_ptr<WorkerRegistration>> staged_workers;
    if (!stage_workers(pending_workers, staged_workers) ||
        !reserve_worker_capacity(staged_workers.size())) {
        cancel_staged_workers(staged_workers);
        if (prepared->descriptor->unload) {
            CallbackGuard callback_guard{this};
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    auto& old = plugins_[index];
    if (!stop_workers(old.owner_mod_id, old.id, false)) {
        cancel_staged_workers(staged_workers);
        if (prepared->descriptor->unload) {
            CallbackGuard callback_guard{this};
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    bool old_recovery_cleared = true;
    if (old.descriptor && old.descriptor->unload) {
        if (execution_begin_ && !old.owner_mod_id.empty() &&
            !execution_begin_(old.owner_mod_id, old.id, "reload-unload")) {
            cancel_staged_workers(staged_workers);
            if (prepared->descriptor->unload) {
                CallbackGuard callback_guard{this};
                prepared->descriptor->unload();
            }
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }

        {
            CallbackGuard callback_guard{this};
            old.descriptor->unload();
        }
        old_recovery_cleared =
            !execution_end_ || old.owner_mod_id.empty() || execution_end_();
    }

    const auto old_owner = old.owner_mod_id;
    const auto old_id = old.id;
    const auto old_loaded_path = old.loaded_path;
    remove_callbacks(old_owner, old_id);

    LoadedPlugin replacement;
    replacement.path = std::move(prepared->source_path);
    replacement.loaded_path = std::move(prepared->loaded_path);
    replacement.owner_mod_id = std::move(prepared->owner_mod_id);
    replacement.id = std::move(prepared->id);
    replacement.name = std::move(prepared->name);
    replacement.version = std::move(prepared->version);
    replacement.descriptor = prepared->descriptor;
    replacement.library = std::move(prepared->library);
    plugins_[index] = std::move(replacement);

    tick_callbacks_.insert(
        tick_callbacks_.end(),
        std::make_move_iterator(staged_callbacks.begin()),
        std::make_move_iterator(staged_callbacks.end()));
    publish_workers(staged_workers);
    cleanup_loaded_path(old_loaded_path);

    (void)old_recovery_cleared;
    return true;
}

bool PluginHost::unload(std::string_view plugin_id) noexcept {
    if (callback_active_on_this_thread()) return false;
    std::scoped_lock lock(lifecycle_mutex_);

    const auto found = std::ranges::find(plugins_, plugin_id, &LoadedPlugin::id);
    if (found == plugins_.end()) return false;
    return unload_index(static_cast<std::size_t>(std::distance(plugins_.begin(), found)));
}

std::size_t PluginHost::unload_mod(std::string_view owner_mod_id) noexcept {
    if (callback_active_on_this_thread()) return 0;
    std::scoped_lock lock(lifecycle_mutex_);

    std::size_t unloaded{};
    for (std::size_t index = plugins_.size(); index > 0; --index) {
        if (plugins_[index - 1].owner_mod_id != owner_mod_id) continue;
        if (unload_index(index - 1)) ++unloaded;
    }
    return unloaded;
}

void PluginHost::unload_all() noexcept {
    if (callback_active_on_this_thread()) return;
    std::scoped_lock lock(lifecycle_mutex_);

    while (!plugins_.empty()) {
        if (!unload_index(plugins_.size() - 1, true)) return;
    }
    tick_callbacks_.clear();
}

bool PluginHost::unload_index(std::size_t index, bool wait_forever) noexcept {
    if (index >= plugins_.size()) return false;

    auto& plugin = plugins_[index];
    if (!stop_workers(plugin.owner_mod_id, plugin.id, wait_forever)) return false;

    bool callback_ok = true;
    if (plugin.descriptor && plugin.descriptor->unload) {
        bool execute = true;
        if (execution_begin_ && !plugin.owner_mod_id.empty()) {
            execute = execution_begin_(plugin.owner_mod_id, plugin.id, "unload");
        }

        if (execute) {
            {
                CallbackGuard callback_guard{this};
                plugin.descriptor->unload();
            }
            if (execution_end_ && !plugin.owner_mod_id.empty()) callback_ok = execution_end_();
        } else {
            callback_ok = false;
        }
    }

    const auto owner = plugin.owner_mod_id;
    const auto id = plugin.id;
    const auto loaded_path = plugin.loaded_path;
    remove_callbacks(owner, id);
    plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
    cleanup_loaded_path(loaded_path);
    return callback_ok;
}

void PluginHost::remove_callbacks(std::string_view owner_mod_id, std::string_view plugin_id) noexcept {
    std::erase_if(tick_callbacks_, [&](const TickRegistration& registration) {
        return registration.owner_mod_id == owner_mod_id && registration.plugin_id == plugin_id;
    });
}

bool PluginHost::tick(double delta_seconds) {
    if (callback_active_on_this_thread()) return false;
    std::scoped_lock lock(lifecycle_mutex_);

    if ((capabilities_.load(std::memory_order_acquire) & ENRAGE_CAPABILITY_TICK) == 0) return true;

    const auto callbacks = tick_callbacks_;
    for (const auto& registration : callbacks) {
        if (!registration.callback) continue;

        if (execution_begin_ && !registration.owner_mod_id.empty() &&
            !execution_begin_(registration.owner_mod_id, registration.plugin_id, "tick")) {
            return false;
        }

        {
            CallbackGuard callback_guard{this};
            registration.callback(delta_seconds, registration.user_data);
        }

        if (execution_end_ && !registration.owner_mod_id.empty() && !execution_end_()) return false;
    }
    return true;
}

void PluginHost::set_capabilities(enrage_capability_bits capabilities) noexcept {
    capabilities_.store(capabilities, std::memory_order_release);
}

void PluginHost::set_execution_observer(ExecutionBegin begin, ExecutionEnd end) {
    if (callback_active_on_this_thread()) return;
    std::scoped_lock lock(lifecycle_mutex_);
    execution_begin_ = std::move(begin);
    execution_end_ = std::move(end);
}

void PluginHost::set_virtual_file_reader(VirtualFileReader reader) {
    std::scoped_lock lock(api_mutex_);
    virtual_file_reader_ = std::move(reader);
}

const std::vector<LoadedPlugin>& PluginHost::plugins() const noexcept {
    return plugins_;
}

std::size_t PluginHost::tick_callback_count() const noexcept {
    std::scoped_lock lock(lifecycle_mutex_);
    return tick_callbacks_.size();
}

std::size_t PluginHost::worker_count() const noexcept {
    std::scoped_lock lock(worker_mutex_);
    return workers_.size();
}

void ENRAGE_CALL PluginHost::log_thunk(void*, uint32_t level, const char* message) {
    std::clog << "[ENRAGE:" << level << "] " << (message ? message : "") << '\n';
}

enrage_capability_bits ENRAGE_CALL PluginHost::capabilities_thunk(void* context) {
    if (!context) return ENRAGE_CAPABILITY_NONE;
    return static_cast<PluginHost*>(context)->capabilities_.load(std::memory_order_acquire);
}

enrage_result ENRAGE_CALL PluginHost::register_tick_thunk(
    void* context,
    enrage_tick_callback callback,
    void* user_data) {
    if (!context || !callback) return ENRAGE_INVALID_ARGUMENT;

    auto* host = static_cast<PluginHost*>(context);
    if ((host->capabilities_.load(std::memory_order_acquire) & ENRAGE_CAPABILITY_TICK) == 0) {
        return ENRAGE_UNSUPPORTED;
    }
    if (registration_host_ != host || registration_plugin_id_.empty()) {
        return ENRAGE_UNSUPPORTED;
    }

    TickRegistration registration{
        callback,
        user_data,
        registration_mod_id_,
        registration_plugin_id_,
    };
    if (registration_target_) {
        registration_target_->push_back(std::move(registration));
    } else {
        host->tick_callbacks_.push_back(std::move(registration));
    }
    return ENRAGE_OK;
}

enrage_result ENRAGE_CALL PluginHost::vfs_read_thunk(
    void* context,
    const char* logical_path,
    void* buffer,
    std::size_t buffer_size,
    std::size_t* required_size) {
    if (!context || !logical_path || !required_size) return ENRAGE_INVALID_ARGUMENT;

    auto* host = static_cast<PluginHost*>(context);
    if ((host->capabilities_.load(std::memory_order_acquire) & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0) {
        return ENRAGE_UNSUPPORTED;
    }

    VirtualFileReader reader;
    {
        std::scoped_lock lock(host->api_mutex_);
        reader = host->virtual_file_reader_;
    }
    if (!reader) return ENRAGE_UNSUPPORTED;

    const auto bytes = reader(logical_path);
    if (!bytes) {
        *required_size = 0;
        return ENRAGE_NOT_FOUND;
    }

    *required_size = bytes->size();
    if (bytes->empty()) return ENRAGE_OK;
    if (!buffer || buffer_size < bytes->size()) return ENRAGE_BUFFER_TOO_SMALL;

    std::memcpy(buffer, bytes->data(), bytes->size());
    return ENRAGE_OK;
}

enrage_result ENRAGE_CALL PluginHost::start_worker_thunk(
    void* context,
    enrage_worker_main_callback main_callback,
    enrage_worker_request_stop_callback request_stop_callback,
    void* user_data,
    enrage_worker_id* worker_id) {
    if (!context || !main_callback || !worker_id) return ENRAGE_INVALID_ARGUMENT;

    auto* host = static_cast<PluginHost*>(context);
    if (registration_host_ != host || registration_plugin_id_.empty() ||
        !registration_worker_target_) {
        return ENRAGE_UNSUPPORTED;
    }

    auto id = worker_sequence.fetch_add(1, std::memory_order_relaxed);
    if (id == ENRAGE_WORKER_ID_INVALID) {
        id = worker_sequence.fetch_add(1, std::memory_order_relaxed);
        if (id == ENRAGE_WORKER_ID_INVALID) return ENRAGE_INTERNAL_ERROR;
    }

    try {
        registration_worker_target_->push_back({
            id,
            main_callback,
            request_stop_callback,
            user_data,
            registration_mod_id_,
            registration_plugin_id_,
        });
    } catch (...) {
        return ENRAGE_INTERNAL_ERROR;
    }

    *worker_id = id;
    return ENRAGE_OK;
}

uint32_t ENRAGE_CALL PluginHost::worker_stop_requested_thunk(
    void* context,
    enrage_worker_id worker_id) {
    if (!context || worker_id == ENRAGE_WORKER_ID_INVALID) return 1u;

    auto* host = static_cast<PluginHost*>(context);
    std::scoped_lock lock(host->worker_mutex_);
    const auto found = std::ranges::find_if(host->workers_, [worker_id](const auto& worker) {
        return worker && worker->id == worker_id;
    });
    if (found == host->workers_.end()) return 1u;
    return (*found)->stop_requested.load(std::memory_order_acquire) ? 1u : 0u;
}

} // namespace enrage::core
