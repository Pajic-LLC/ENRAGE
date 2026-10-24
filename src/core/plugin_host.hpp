#pragma once

#include "enrage/sdk.h"
#include "platform/dynamic_library.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enrage::core {

struct LoadedPlugin {
    std::filesystem::path path;
    std::string owner_mod_id;
    std::string id;
    std::string name;
    std::string version;
    const enrage_plugin_descriptor* descriptor{};
    platform::DynamicLibrary library;
};

class PluginHost {
public:
    using ExecutionBegin = std::function<bool(
        std::string_view mod_id,
        std::string_view plugin_id,
        std::string_view callback)>;
    using ExecutionEnd = std::function<bool()>;

    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool load(const std::filesystem::path& path, std::string_view owner_mod_id = {});
    void unload_all() noexcept;
    [[nodiscard]] bool tick(double delta_seconds);

    void set_capabilities(enrage_capability_bits capabilities) noexcept;
    void set_execution_observer(ExecutionBegin begin, ExecutionEnd end);

    [[nodiscard]] const std::vector<LoadedPlugin>& plugins() const noexcept;
    [[nodiscard]] std::size_t tick_callback_count() const noexcept;

private:
    struct TickRegistration {
        enrage_tick_callback callback{};
        void* user_data{};
        std::string owner_mod_id;
        std::string plugin_id;
    };

    static void log_thunk(void* context, uint32_t level, const char* message);
    static enrage_capability_bits capabilities_thunk(void* context);
    static enrage_result register_tick_thunk(
        void* context,
        enrage_tick_callback callback,
        void* user_data);

    enrage_host_api api_{};
    enrage_capability_bits capabilities_{};
    std::vector<TickRegistration> tick_callbacks_;
    std::vector<LoadedPlugin> plugins_;
    ExecutionBegin execution_begin_;
    ExecutionEnd execution_end_;
    std::string registering_mod_id_;
    std::string registering_plugin_id_;
};

} // namespace enrage::core
