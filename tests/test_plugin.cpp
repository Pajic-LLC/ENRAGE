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

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.test",
    "ENRAGE Test Plugin",
    "1.0.0",
    &load,
    &unload,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void) {
    return &descriptor;
}
