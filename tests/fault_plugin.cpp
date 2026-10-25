#include "enrage/sdk.h"

#include <cstdlib>

namespace {

void on_tick(double, void*) {
    std::_Exit(86);
}

enrage_result load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION || !host->register_tick) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }
    return host->register_tick(host->context, &on_tick, nullptr);
}

void unload() {}

// Intentionally advertises only the original descriptor prefix even though it
// is compiled against the current header. This guards binary compatibility for
// plugins built before the optional state-handoff tail existed.
const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE,
    "dev.enrage.fault",
    "ENRAGE Fault Fixture",
    "1.0.0",
    &load,
    &unload,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void) {
    return &descriptor;
}
