#include "core/plugin_host.hpp"

#include <algorithm>
#include <cstddef>
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
}

PluginHost::~PluginHost() {
    unload_all();
}

bool PluginHost::load(const std::filesystem::path& path, std::string_view owner_mod_id) {
    platform::DynamicLibrary library;
    if (!library.open(path)) {
        return false;
    }

    auto* raw_entry = library.symbol(ENRAGE_PLUGIN_ENTRY_NAME);
    if (!raw_entry) {
        return false;
    }

    const auto entry = reinterpret_cast<enrage_plugin_entry_fn>(raw_entry);
    const auto* descriptor = entry();
    if (!descriptor ||
        descriptor->abi_version != ENRAGE_ABI_VERSION ||
        descriptor->struct_size < sizeof(enrage_plugin_descriptor) ||
        !descriptor->id || !descriptor->name || !descriptor->version ||
        !descriptor->load) {
        return false;
    }

    const auto duplicate = std::ranges::find_if(plugins_, [descriptor](const LoadedPlugin& plugin) {
        return plugin.id == descriptor->id;
    });
    if (duplicate != plugins_.end()) {
        return false;
    }

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
        if (load_result == ENRAGE_OK && descriptor->unload) {
            descriptor->unload();
        }
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

void PluginHost::unload_all() noexcept {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (!it->descriptor || !it->descriptor->unload) {
            continue;
        }

        bool execute = true;
        if (execution_begin_ && !it->owner_mod_id.empty()) {
            execute = execution_begin_(it->owner_mod_id, it->id, "unload");
        }
        if (!execute) {
            continue;
        }

        it->descriptor->unload();
        if (execution_end_ && !it->owner_mod_id.empty()) {
            (void)execution_end_();
        }
    }

    tick_callbacks_.clear();
    plugins_.clear();
}

bool PluginHost::tick(double delta_seconds) {
    const auto callbacks = tick_callbacks_;
    for (const auto& registration : callbacks) {
        if (!registration.callback) {
            continue;
        }

        if (execution_begin_ && !registration.owner_mod_id.empty() &&
            !execution_begin_(registration.owner_mod_id, registration.plugin_id, "tick")) {
            return false;
        }

        registration.callback(delta_seconds, registration.user_data);

        if (execution_end_ && !registration.owner_mod_id.empty() && !execution_end_()) {
            return false;
        }
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

const std::vector<LoadedPlugin>& PluginHost::plugins() const noexcept {
    return plugins_;
}

std::size_t PluginHost::tick_callback_count() const noexcept {
    return tick_callbacks_.size();
}

void PluginHost::log_thunk(void*, uint32_t level, const char* message) {
    std::clog << "[ENRAGE:" << level << "] " << (message ? message : "") << '\n';
}

enrage_capability_bits PluginHost::capabilities_thunk(void* context) {
    if (!context) {
        return ENRAGE_CAPABILITY_NONE;
    }
    return static_cast<PluginHost*>(context)->capabilities_;
}

enrage_result PluginHost::register_tick_thunk(
    void* context,
    enrage_tick_callback callback,
    void* user_data) {
    if (!context || !callback) {
        return ENRAGE_INVALID_ARGUMENT;
    }

    auto* host = static_cast<PluginHost*>(context);
    if ((host->capabilities_ & ENRAGE_CAPABILITY_TICK) == 0) {
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

} // namespace enrage::core
