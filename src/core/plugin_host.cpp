#include "core/plugin_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <utility>

namespace enrage::core {
namespace {

std::atomic_uint64_t shadow_sequence{};

bool valid_descriptor(const enrage_plugin_descriptor* descriptor) {
    return descriptor &&
           descriptor->abi_version == ENRAGE_ABI_VERSION &&
           descriptor->struct_size >= ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE &&
           descriptor->id && descriptor->name && descriptor->version &&
           descriptor->load;
}

} // namespace

PluginHost::PluginHost() {
    api_.abi_version = ENRAGE_ABI_VERSION;
    api_.struct_size = static_cast<uint32_t>(sizeof(enrage_host_api));
    api_.context = this;
    api_.log = &PluginHost::log_thunk;
    api_.capabilities = &PluginHost::capabilities_thunk;
    api_.register_tick = &PluginHost::register_tick_thunk;
    api_.vfs_read = &PluginHost::vfs_read_thunk;
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

bool PluginHost::load(const std::filesystem::path& path, std::string_view owner_mod_id) {
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
    registering_mod_id_ = std::string{owner_mod_id};
    registering_plugin_id_ = prepared->id;
    registration_target_ = nullptr;
    const auto load_result = prepared->descriptor->load(&api_);
    registering_mod_id_.clear();
    registering_plugin_id_.clear();

    const bool recovery_cleared = !execution_end_ || owner_mod_id.empty() || execution_end_();
    if (load_result != ENRAGE_OK || !recovery_cleared) {
        tick_callbacks_.resize(tick_count_before_load);
        if (load_result == ENRAGE_OK && prepared->descriptor->unload) {
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
    return true;
}

bool PluginHost::reload(
    std::string_view plugin_id,
    const std::optional<std::filesystem::path>& replacement_path) {
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

    std::vector<TickRegistration> staged_callbacks;
    if (execution_begin_ && !found->owner_mod_id.empty() &&
        !execution_begin_(found->owner_mod_id, found->id, "reload-load")) {
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    registering_mod_id_ = found->owner_mod_id;
    registering_plugin_id_ = found->id;
    registration_target_ = &staged_callbacks;
    const auto load_result = prepared->descriptor->load(&api_);
    registration_target_ = nullptr;
    registering_mod_id_.clear();
    registering_plugin_id_.clear();

    const bool load_recovery_cleared =
        !execution_end_ || found->owner_mod_id.empty() || execution_end_();
    if (load_result != ENRAGE_OK || !load_recovery_cleared) {
        if (load_result == ENRAGE_OK && prepared->descriptor->unload) {
            prepared->descriptor->unload();
        }
        prepared->library.close();
        cleanup_loaded_path(prepared->loaded_path);
        return false;
    }

    auto& old = plugins_[index];
    bool old_recovery_cleared = true;
    if (old.descriptor && old.descriptor->unload) {
        if (execution_begin_ && !old.owner_mod_id.empty() &&
            !execution_begin_(old.owner_mod_id, old.id, "reload-unload")) {
            if (prepared->descriptor->unload) prepared->descriptor->unload();
            prepared->library.close();
            cleanup_loaded_path(prepared->loaded_path);
            return false;
        }

        old.descriptor->unload();
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
    cleanup_loaded_path(old_loaded_path);

    // If recovery bookkeeping could not be cleared after the old unload, the
    // replacement is still the only valid live generation. The execution
    // observer has already reported the persistence failure, so keep the new
    // generation rather than pretending the rollback was possible.
    (void)old_recovery_cleared;
    return true;
}

bool PluginHost::unload(std::string_view plugin_id) noexcept {
    const auto found = std::ranges::find(plugins_, plugin_id, &LoadedPlugin::id);
    if (found == plugins_.end()) return false;
    return unload_index(static_cast<std::size_t>(std::distance(plugins_.begin(), found)));
}

std::size_t PluginHost::unload_mod(std::string_view owner_mod_id) noexcept {
    std::size_t unloaded{};
    for (std::size_t index = plugins_.size(); index > 0; --index) {
        if (plugins_[index - 1].owner_mod_id != owner_mod_id) continue;
        (void)unload_index(index - 1);
        ++unloaded;
    }
    return unloaded;
}

void PluginHost::unload_all() noexcept {
    while (!plugins_.empty()) {
        (void)unload_index(plugins_.size() - 1);
    }
    tick_callbacks_.clear();
}

bool PluginHost::unload_index(std::size_t index) noexcept {
    if (index >= plugins_.size()) return false;

    auto& plugin = plugins_[index];
    bool callback_ok = true;

    if (plugin.descriptor && plugin.descriptor->unload) {
        bool execute = true;
        if (execution_begin_ && !plugin.owner_mod_id.empty()) {
            execute = execution_begin_(plugin.owner_mod_id, plugin.id, "unload");
        }

        if (execute) {
            plugin.descriptor->unload();
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
    if ((capabilities_ & ENRAGE_CAPABILITY_TICK) == 0) return true;

    const auto callbacks = tick_callbacks_;
    for (const auto& registration : callbacks) {
        if (!registration.callback) continue;

        if (execution_begin_ && !registration.owner_mod_id.empty() &&
            !execution_begin_(registration.owner_mod_id, registration.plugin_id, "tick")) {
            return false;
        }

        registration.callback(delta_seconds, registration.user_data);

        if (execution_end_ && !registration.owner_mod_id.empty() && !execution_end_()) return false;
    }
    return true;
}

void PluginHost::set_capabilities(enrage_capability_bits capabilities) noexcept {
    capabilities_ = capabilities;
}

void PluginHost::set_execution_observer(ExecutionBegin begin, ExecutionEnd end) {
    execution_begin_ = std::move(begin);
    execution_end_ = std::move(end);
}

void PluginHost::set_virtual_file_reader(VirtualFileReader reader) {
    virtual_file_reader_ = std::move(reader);
}

const std::vector<LoadedPlugin>& PluginHost::plugins() const noexcept {
    return plugins_;
}

std::size_t PluginHost::tick_callback_count() const noexcept {
    return tick_callbacks_.size();
}

void ENRAGE_CALL PluginHost::log_thunk(void*, uint32_t level, const char* message) {
    std::clog << "[ENRAGE:" << level << "] " << (message ? message : "") << '\n';
}

enrage_capability_bits ENRAGE_CALL PluginHost::capabilities_thunk(void* context) {
    if (!context) return ENRAGE_CAPABILITY_NONE;
    return static_cast<PluginHost*>(context)->capabilities_;
}

enrage_result ENRAGE_CALL PluginHost::register_tick_thunk(
    void* context,
    enrage_tick_callback callback,
    void* user_data) {
    if (!context || !callback) return ENRAGE_INVALID_ARGUMENT;

    auto* host = static_cast<PluginHost*>(context);
    if ((host->capabilities_ & ENRAGE_CAPABILITY_TICK) == 0) return ENRAGE_UNSUPPORTED;
    if (host->registering_plugin_id_.empty()) {
        // ABI v1 registrations are owned by the plugin being loaded. Reject
        // late registrations because they cannot be attributed safely.
        return ENRAGE_UNSUPPORTED;
    }

    TickRegistration registration{
        callback,
        user_data,
        host->registering_mod_id_,
        host->registering_plugin_id_,
    };
    if (host->registration_target_) {
        host->registration_target_->push_back(std::move(registration));
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
    if ((host->capabilities_ & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0 ||
        !host->virtual_file_reader_) {
        return ENRAGE_UNSUPPORTED;
    }

    const auto bytes = host->virtual_file_reader_(logical_path);
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

} // namespace enrage::core
