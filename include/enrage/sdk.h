#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define ENRAGE_EXPORT extern "C" __declspec(dllexport)
#else
#define ENRAGE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define ENRAGE_ABI_VERSION 1u

namespace enrage {

enum class Result : std::uint32_t {
    ok = 0,
    invalid_argument,
    incompatible_abi,
    unsupported,
    internal_error,
};

enum Capability : std::uint64_t {
    capability_none = 0,
    capability_tick = 1ull << 0,
    capability_virtual_files = 1ull << 1,
    capability_native_calls = 1ull << 2,
    capability_assets = 1ull << 3,
};

using TickCallback = void (*)(double delta_seconds, void* user_data);
using LogCallback = void (*)(std::uint32_t level, const char* message);

struct HostApi {
    std::uint32_t abi_version;
    std::uint32_t struct_size;

    LogCallback log;
    std::uint64_t (*capabilities)();
    Result (*register_tick)(TickCallback callback, void* user_data);
};

struct PluginDescriptor {
    std::uint32_t abi_version;
    std::uint32_t struct_size;

    const char* id;
    const char* name;
    const char* version;

    Result (*load)(const HostApi* host);
    void (*unload)();
};

using PluginEntry = const PluginDescriptor* (*)();

} // namespace enrage

// Native plugins export a function with this exact name and signature.
// ENRAGE validates abi_version and struct_size before invoking the plugin.
#define ENRAGE_PLUGIN_ENTRY enrage_plugin_entry
