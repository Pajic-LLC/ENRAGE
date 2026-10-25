#pragma once

#include "enrage/sdk.h"
#include "platform/dynamic_library.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enrage::core {

struct LoadedPlugin {
    std::filesystem::path path;
    std::filesystem::path loaded_path;
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
    using VirtualFileReader = std::function<std::optional<std::vector<std::byte>>(std::string_view)>;

    PluginHost();
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool load(const std::filesystem::path& path, std::string_view owner_mod_id = {});
    bool reload(
        std::string_view plugin_id,
        const std::optional<std::filesystem::path>& replacement_path = std::nullopt);
    bool unload(std::string_view plugin_id) noexcept;
    std::size_t unload_mod(std::string_view owner_mod_id) noexcept;
    void unload_all() noexcept;
    [[nodiscard]] bool tick(double delta_seconds);

    void set_capabilities(enrage_capability_bits capabilities) noexcept;
    void set_execution_observer(ExecutionBegin begin, ExecutionEnd end);
    void set_virtual_file_reader(VirtualFileReader reader);

    [[nodiscard]] const std::vector<LoadedPlugin>& plugins() const noexcept;
    [[nodiscard]] std::size_t tick_callback_count() const noexcept;

private:
    struct TickRegistration {
        enrage_tick_callback callback{};
        void* user_data{};
        std::string owner_mod_id;
        std::string plugin_id;
    };

    struct PreparedPlugin {
        std::filesystem::path source_path;
        std::filesystem::path loaded_path;
        std::string owner_mod_id;
        std::string id;
        std::string name;
        std::string version;
        const enrage_plugin_descriptor* descriptor{};
        platform::DynamicLibrary library;
    };

    [[nodiscard]] std::optional<std::filesystem::path> create_shadow_copy(
        const std::filesystem::path& source) const;
    [[nodiscard]] std::optional<PreparedPlugin> prepare_plugin(
        const std::filesystem::path& source,
        std::string_view owner_mod_id) const;
    void cleanup_loaded_path(const std::filesystem::path& path) const noexcept;
    bool unload_index(std::size_t index) noexcept;
    void remove_callbacks(std::string_view owner_mod_id, std::string_view plugin_id) noexcept;

    static void ENRAGE_CALL log_thunk(void* context, uint32_t level, const char* message);
    static enrage_capability_bits ENRAGE_CALL capabilities_thunk(void* context);
    static enrage_result ENRAGE_CALL register_tick_thunk(
        void* context,
        enrage_tick_callback callback,
        void* user_data);
    static enrage_result ENRAGE_CALL vfs_read_thunk(
        void* context,
        const char* logical_path,
        void* buffer,
        std::size_t buffer_size,
        std::size_t* required_size);

    enrage_host_api api_{};
    enrage_capability_bits capabilities_{};
    std::vector<TickRegistration> tick_callbacks_;
    std::vector<LoadedPlugin> plugins_;
    ExecutionBegin execution_begin_;
    ExecutionEnd execution_end_;
    VirtualFileReader virtual_file_reader_;
    std::string registering_mod_id_;
    std::string registering_plugin_id_;
    std::vector<TickRegistration>* registration_target_{};
};

} // namespace enrage::core
