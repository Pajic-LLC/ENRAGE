#include "enrage/sdk.h"

#include <cstdint>
#include <cstring>

namespace {

std::uint64_t tick_count{};

void on_tick(double, void*) {
    ++tick_count;
}

enrage_result load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION || !host->register_tick) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }

    return host->register_tick(host->context, &on_tick, nullptr);
}

void unload() {}

enrage_result save_state(void* buffer, std::size_t buffer_size, std::size_t* required_size) {
    if (!required_size) return ENRAGE_INVALID_ARGUMENT;
    *required_size = sizeof(tick_count);
    if (!buffer || buffer_size < sizeof(tick_count)) return ENRAGE_BUFFER_TOO_SMALL;
    std::memcpy(buffer, &tick_count, sizeof(tick_count));
    return ENRAGE_OK;
}

enrage_result restore_state(const void* buffer, std::size_t buffer_size) {
    if (!buffer || buffer_size != sizeof(tick_count)) return ENRAGE_INVALID_ARGUMENT;
    std::uint64_t restored{};
    std::memcpy(&restored, buffer, sizeof(restored));
    if (restored == 0) return ENRAGE_INTERNAL_ERROR;
    tick_count = restored;
    return ENRAGE_OK;
}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.test",
    "ENRAGE Test Plugin",
    "1.0.0",
    &load,
    &unload,
    &save_state,
    &restore_state,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* enrage_plugin_entry(void) {
    return &descriptor;
}
