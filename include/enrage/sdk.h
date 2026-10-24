#ifndef ENRAGE_SDK_H
#define ENRAGE_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define ENRAGE_EXPORT __declspec(dllexport)
#else
#define ENRAGE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ENRAGE_ABI_VERSION 1u
#define ENRAGE_PLUGIN_ENTRY_NAME "enrage_plugin_entry"

typedef enum enrage_result {
    ENRAGE_OK = 0,
    ENRAGE_INVALID_ARGUMENT = 1,
    ENRAGE_INCOMPATIBLE_ABI = 2,
    ENRAGE_UNSUPPORTED = 3,
    ENRAGE_INTERNAL_ERROR = 4
} enrage_result;

typedef uint64_t enrage_capability_bits;

enum {
    ENRAGE_CAPABILITY_NONE = 0,
    ENRAGE_CAPABILITY_TICK = 1ull << 0,
    ENRAGE_CAPABILITY_VIRTUAL_FILES = 1ull << 1,
    ENRAGE_CAPABILITY_NATIVE_CALLS = 1ull << 2,
    ENRAGE_CAPABILITY_ASSETS = 1ull << 3
};

typedef void (*enrage_tick_callback)(double delta_seconds, void* user_data);
typedef void (*enrage_log_callback)(void* context, uint32_t level, const char* message);
typedef enrage_capability_bits (*enrage_capabilities_callback)(void* context);
typedef enrage_result (*enrage_register_tick_callback)(
    void* context,
    enrage_tick_callback callback,
    void* user_data);

typedef struct enrage_host_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;

    enrage_log_callback log;
    enrage_capabilities_callback capabilities;
    enrage_register_tick_callback register_tick;
} enrage_host_api;

typedef enrage_result (*enrage_plugin_load_callback)(const enrage_host_api* host);
typedef void (*enrage_plugin_unload_callback)(void);

typedef struct enrage_plugin_descriptor {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* id;
    const char* name;
    const char* version;

    enrage_plugin_load_callback load;
    enrage_plugin_unload_callback unload;
} enrage_plugin_descriptor;

typedef const enrage_plugin_descriptor* (*enrage_plugin_entry_fn)(void);

ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void);

#ifdef __cplusplus
}
#endif

#endif
