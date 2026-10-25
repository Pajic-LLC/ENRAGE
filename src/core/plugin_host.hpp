#pragma once

#include "enrage/sdk.h"
#include "platform/dynamic_library.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
    std::uint64_t generation{};
    const enrage_plugin_descriptor* descriptor{};
    platform::DynamicLibrary library;
};

struct PluginSnapshot {
    std::filesystem::path path;
    std::filesystem::path loaded_path;
    std::string owner_mod_id;
    std::string id;
    std::string name;
    std::string version;
    std::uint64_t generation{};
};

struct PluginExecutionContext {
    std::string_view mod_id;
    std::string_view plugin_id;
    std::string_view callback;
    std::uint64_t generation{};
    std::uint64_t worker_id{};
};

class PluginHost {
public:
    using ExecutionBegin = std::function<bool(
        std::string_view mod_id,
        std::string_view plugin_id,
        std::string_view callback)>;
    using DetailedExecutionBegin = std::function<bool(const PluginExecutionContext& context)>;
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
    void set_detailed_execution_observer(DetailedExecutionBegin begin, ExecutionEnd end);
    void set_virtual_file_reader(VirtualFileReader reader);

    [[nodiscard]] static std::optional<PluginExecutionContext> current_execution_context() noexcept;

    // plugins() is retained for single-threaded inspection and existing callers.
    // Use snapshot() whenever metadata can be observed concurrently with reloads.
    [[nodiscard]] const std::vector<LoadedPlugin>& plugins() const noexcept;
    [[nodiscard]] std::vector<PluginSnapshot> snapshot() const {
        std::scoped_lock lock(lifecycle_mutex_);
        std::vector<PluginSnapshot> result;
        result.reserve(plugins_.size());
        for (const auto& plugin : plugins_) {
            result.push_back({
                plugin.path,
                plugin.loaded_path,
                plugin.owner_mod_id,
                plugin.id,
                plugin.name,
                plugin.version,
                plugin.generation,
            });
        }
        return result;
    }
    [[nodiscard]] std::size_t tick_callback_count() const noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    struct TickRegistration {
        enrage_tick_callback callback{};
        void* user_data{};
        std::string owner_mod_id;
        std::string plugin_id;
        std::uint64_t generation{};
    };

    struct PendingWorker {
        enrage_worker_id id{ENRAGE_WORKER_ID_INVALID};
        enrage_worker_main_callback main_callback{};
        enrage_worker_request_stop_callback request_stop_callback{};
        void* user_data{};
        std::string owner_mod_id;
        std::string plugin_id;
        std::uint64_t generation{};
    };

    struct WorkerRegistration {
        enrage_worker_id id{ENRAGE_WORKER_ID_INVALID};
        enrage_worker_main_callback main_callback{};
        enrage_worker_request_stop_callback request_stop_callback{};
        void* user_data{};
        std::string owner_mod_id;
        std::string plugin_id;
        std::uint64_t generation{};
        std::atomic_bool stop_requested{};
        std::mutex state_mutex;
        std::condition_variable state_cv;
        bool start_allowed{};
        bool cancel_before_start{};
        bool finished{};
        std::thread thread;
    };

    struct PreparedPlugin {
        std::filesystem::path source_path;
        std::filesystem::path loaded_path;
        std::string owner_mod_id;
        std::string id;
        std::string name;
        std::string version;
        std::uint64_t generation{};
        const enrage_plugin_descriptor* descriptor{};
        platform::DynamicLibrary library;
    };

    class CallbackGuard {
    public:
        explicit CallbackGuard(const PluginHost* host) noexcept;
        ~CallbackGuard();

        CallbackGuard(const CallbackGuard&) = delete;
        CallbackGuard& operator=(const CallbackGuard&) = delete;

    private:
        const PluginHost* previous_{};
    };

    [[nodiscard]] bool callback_active_on_this_thread() const noexcept;
    [[nodiscard]] bool begin_execution(const PluginExecutionContext& context) const;
    [[nodiscard]] bool end_execution() const;
    [[nodiscard]] std::optional<std::filesystem::path> create_shadow_copy(
        const std::filesystem::path& source) const;
    [[nodiscard]] std::optional<PreparedPlugin> prepare_plugin(
        const std::filesystem::path& source,
        std::string_view owner_mod_id) const;
    void cleanup_loaded_path(const std::filesystem::path& path) const noexcept;
    bool unload_index(std::size_t index, bool wait_forever = false) noexcept;
    void remove_callbacks(std::string_view owner_mod_id, std::string_view plugin_id) noexcept;

    [[nodiscard]] bool stage_workers(
        const std::vector<PendingWorker>& pending,
        std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept;
    [[nodiscard]] bool reserve_worker_capacity(std::size_t additional) noexcept;
    void cancel_staged_workers(
        std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept;
    void publish_workers(
        std::vector<std::unique_ptr<WorkerRegistration>>& staged) noexcept;
    [[nodiscard]] bool stop_workers(
        std::string_view owner_mod_id,
        std::string_view plugin_id,
        bool wait_forever) noexcept;

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
    static enrage_result ENRAGE_CALL start_worker_thunk(
        void* context,
        enrage_worker_main_callback main_callback,
        enrage_worker_request_stop_callback request_stop_callback,
        void* user_data,
        enrage_worker_id* worker_id);
    static uint32_t ENRAGE_CALL worker_stop_requested_thunk(
        void* context,
        enrage_worker_id worker_id);

    static thread_local const PluginHost* callback_host_;
    static thread_local PluginHost* registration_host_;
    static thread_local std::string registration_mod_id_;
    static thread_local std::string registration_plugin_id_;
    static thread_local std::uint64_t registration_generation_;
    static thread_local std::vector<TickRegistration>* registration_target_;
    static thread_local std::vector<PendingWorker>* registration_worker_target_;
    static thread_local bool execution_context_active_;
    static thread_local PluginExecutionContext execution_context_;

    enrage_host_api api_{};
    std::atomic<enrage_capability_bits> capabilities_{};
    std::vector<TickRegistration> tick_callbacks_;
    std::vector<LoadedPlugin> plugins_;
    std::vector<std::unique_ptr<WorkerRegistration>> workers_;
    ExecutionBegin execution_begin_;
    DetailedExecutionBegin detailed_execution_begin_;
    ExecutionEnd execution_end_;
    VirtualFileReader virtual_file_reader_;
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex api_mutex_;
    mutable std::mutex worker_mutex_;
};

} // namespace enrage::core
