#include "enrage/sdk.h"

namespace {

void on_tick(double, void*) {}

enrage_result load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION || !host->register_tick) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }
    return host->register_tick(host->context, &on_tick, nullptr);
}

void unload() {}

enrage_result restore_state(const void*, size_t) {
    return ENRAGE_INTERNAL_ERROR;
}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.test",
    "ENRAGE State Reject Fixture",
    "1.0.0-reject",
    &load,
    &unload,
    nullptr,
    &restore_state,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void) {
    return &descriptor;
}
