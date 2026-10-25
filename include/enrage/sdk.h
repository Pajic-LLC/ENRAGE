#ifndef ENRAGE_SDK_H
#define ENRAGE_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define ENRAGE_EXPORT __declspec(dllexport)
#if defined(_MSC_VER)
#define ENRAGE_CALL __cdecl
#else
#define ENRAGE_CALL __attribute__((cdecl))
#endif
#else
#define ENRAGE_EXPORT __attribute__((visibility("default")))
#define ENRAGE_CALL
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

typedef void (ENRAGE_CALL *enrage_tick_callback)(double delta_seconds, void* user_data);
typedef void (ENRAGE_CALL *enrage_log_callback)(void* context, uint32_t level, const char* message);
typedef enrage_capability_bits (ENRAGE_CALL *enrage_capabilities_callback)(void* context);
typedef enrage_result (ENRAGE_CALL *enrage_register_tick_callback)(
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

#define ENRAGE_HOST_API_V1_SIZE \
    (offsetof(enrage_host_api, register_tick) + sizeof(((enrage_host_api*)0)->register_tick))

typedef enrage_result (ENRAGE_CALL *enrage_plugin_load_callback)(const enrage_host_api* host);
typedef void (ENRAGE_CALL *enrage_plugin_unload_callback)(void);

typedef struct enrage_plugin_descriptor {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* id;
    const char* name;
    const char* version;

    enrage_plugin_load_callback load;
    enrage_plugin_unload_callback unload;
} enrage_plugin_descriptor;

#define ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE \
    (offsetof(enrage_plugin_descriptor, unload) + sizeof(((enrage_plugin_descriptor*)0)->unload))

typedef const enrage_plugin_descriptor* (ENRAGE_CALL *enrage_plugin_entry_fn)(void);

ENRAGE_EXPORT const enrage_plugin_descriptor* ENRAGE_CALL enrage_plugin_entry(void);

#ifdef __cplusplus
}
#endif

#endif
