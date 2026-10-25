#include "enrage/sdk.h"

#include <stddef.h>
#include <stdint.h>

#define ENRAGE_C_ASSERT(name, expression) typedef char enrage_c_assert_##name[(expression) ? 1 : -1]

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
    NULL,
    NULL,
};

ENRAGE_C_ASSERT(host_tail_order, ENRAGE_HOST_API_V1_SIZE <= ENRAGE_HOST_API_V1_VFS_SIZE);
ENRAGE_C_ASSERT(host_worker_tail_order, ENRAGE_HOST_API_V1_VFS_SIZE <= ENRAGE_HOST_API_V1_WORKER_SIZE);
ENRAGE_C_ASSERT(host_worker_prefix_fits, ENRAGE_HOST_API_V1_WORKER_SIZE <= sizeof(enrage_host_api));
ENRAGE_C_ASSERT(plugin_state_tail_order, ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE <= ENRAGE_PLUGIN_DESCRIPTOR_V1_STATE_SIZE);
ENRAGE_C_ASSERT(plugin_state_tail_fits, ENRAGE_PLUGIN_DESCRIPTOR_V1_STATE_SIZE <= sizeof(enrage_plugin_descriptor));
ENRAGE_C_ASSERT(worker_id_width, sizeof(enrage_worker_id) == sizeof(uint64_t));
ENRAGE_C_ASSERT(worker_invalid_zero, ENRAGE_WORKER_ID_INVALID == 0ull);
ENRAGE_C_ASSERT(tick_capability_bit, ENRAGE_CAPABILITY_TICK == 1ull);
ENRAGE_C_ASSERT(vfs_capability_bit, ENRAGE_CAPABILITY_VIRTUAL_FILES == 2ull);

int main(void) {
    return descriptor.struct_size >= ENRAGE_PLUGIN_DESCRIPTOR_V1_STATE_SIZE ? 0 : 1;
}
