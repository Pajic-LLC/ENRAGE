#include "enrage/sdk.h"

#include <cstdlib>

namespace {

const enrage_host_api* host_api{};

void on_tick(double, void*) {
    if (!host_api || !host_api->capabilities) {
        std::abort();
    }
    const auto capabilities = host_api->capabilities(host_api->context);
    if ((capabilities & ENRAGE_CAPABILITY_TICK) == 0) {
        std::abort();
    }
}

enrage_result load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION ||
        !host->capabilities || !host->register_tick) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }

    host_api = host;
    return host->register_tick(host->context, &on_tick, nullptr);
}

void unload() {
    host_api = nullptr;
}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.capability-probe",
    "ENRAGE Capability Probe",
    "1.0.0",
    &load,
    &unload,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void) {
    return &descriptor;
}
