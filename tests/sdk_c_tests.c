#include "enrage/sdk.h"

#include <stddef.h>
#include <stdint.h>

static void ENRAGE_CALL on_tick(double delta_seconds, void* user_data) {
    (void)delta_seconds;
    (void)user_data;
}

static enrage_result ENRAGE_CALL on_load(const enrage_host_api* host) {
    if (!host) return ENRAGE_INVALID_ARGUMENT;
    if (host->abi_version != ENRAGE_ABI_VERSION) return ENRAGE_INCOMPATIBLE_ABI;
    if (host->struct_size < ENRAGE_HOST_API_V1_SIZE) return ENRAGE_INCOMPATIBLE_ABI;
    if (!host->capabilities || !host->register_tick) return ENRAGE_INCOMPATIBLE_ABI;
    if ((host->capabilities(host->context) & ENRAGE_CAPABILITY_TICK) == 0) {
        return ENRAGE_UNSUPPORTED;
    }
    return host->register_tick(host->context, on_tick, NULL);
}

static void ENRAGE_CALL on_unload(void) {}

static const enrage_plugin_descriptor descriptor = {
    ENRAGE_ABI_VERSION,
    (uint32_t)sizeof(enrage_plugin_descriptor),
    "dev.enrage.c-fixture",
    "C Fixture",
    "1.0.0",
    on_load,
    on_unload,
};

_Static_assert(ENRAGE_HOST_API_V1_SIZE <= sizeof(enrage_host_api), "host ABI prefix must fit");
_Static_assert(ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE <= sizeof(enrage_plugin_descriptor), "plugin ABI prefix must fit");
_Static_assert(ENRAGE_CAPABILITY_TICK == 1ull, "capability bit changed");
_Static_assert(ENRAGE_CAPABILITY_VIRTUAL_FILES == 2ull, "capability bit changed");

int main(void) {
    return descriptor.struct_size >= ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE ? 0 : 1;
}
