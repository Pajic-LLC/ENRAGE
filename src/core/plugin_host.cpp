#include "core/plugin_host.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <utility>

namespace enrage::core {

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

bool PluginHost::load(const std::filesystem::path& path, std::string_view owner_mod_id) {
    platform::DynamicLibrary library;
    if (!library.open(path)) return false;

    auto* raw_entry = library.symbol(ENRAGE_PLUGIN_ENTRY_NAME);
    if (!raw_entry) return false;

    const auto entry = reinterpret_cast<enrage_plugin_entry_fn>(raw_entry);
    const auto* descriptor = entry();
    if (!descriptor ||
        descriptor->abi_version != ENRAGE_ABI_VERSION ||
        descriptor->struct_size < ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE ||
        !descriptor->id || !descriptor->name || !descriptor->version ||
        !descriptor->load) {
        return false;
    }

    const auto duplicate = std::ranges::find_if(plugins_, [descriptor](const LoadedPlugin& plugin) {
        return plugin.id == descriptor->id;
    });
    if (duplicate != plugins_.end()) return false;

    if (execution_begin_ && !owner_mod_id.empty() &&
        !execution_begin_(owner_mod_id, descriptor->id, "load")) {
        return false;
    }

    const auto tick_count_before_load = tick_callbacks_.size();
    registering_mod_id_ = std::string{owner_mod_id};
    registering_plugin_id_ = descriptor->id;
    const auto load_result = descriptor->load(&api_);
    registering_mod_id_.clear();
    registering_plugin_id_.clear();

    const bool recovery_cleared = !execution_end_ || owner_mod_id.empty() || execution_end_();
    if (load_result != ENRAGE_OK || !recovery_cleared) {
        tick_callbacks_.resize(tick_count_before_load);
        if (load_result == ENRAGE_OK && descriptor->unload) descriptor->unload();
        return false;
    }

    LoadedPlugin loaded;
    loaded.path = path;
    loaded.owner_mod_id = std::string{owner_mod_id};
    loaded.id = descriptor->id;
    loaded.name = descriptor->name;
    loaded.version = descriptor->version;
    loaded.descriptor = descriptor;
    loaded.library = std::move(library);
    plugins_.push_back(std::move(loaded));
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
    remove_callbacks(owner, id);
    plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
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

    host->tick_callbacks_.push_back({
        callback,
        user_data,
        host->registering_mod_id_,
        host->registering_plugin_id_,
    });
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
