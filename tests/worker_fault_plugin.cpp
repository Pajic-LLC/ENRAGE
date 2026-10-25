#include "enrage/sdk.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

std::atomic_bool tick_active{};

[[noreturn]] void fault_now() {
#ifdef _WIN32
    RaiseException(0xE0421001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::_Exit(90);
#else
    std::raise(SIGABRT);
    std::_Exit(90);
#endif
}

void ENRAGE_CALL on_tick(double, void*) {
    tick_active.store(true, std::memory_order_release);
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds{1});
}

void ENRAGE_CALL worker_main(enrage_worker_id, void*) {
    while (!tick_active.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    fault_now();
}

enrage_result ENRAGE_CALL load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION ||
        host->struct_size < ENRAGE_HOST_API_V1_WORKER_SIZE ||
        !host->register_tick || !host->start_worker) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }

    const auto tick = host->register_tick(host->context, &on_tick, nullptr);
    if (tick != ENRAGE_OK) return tick;

    enrage_worker_id worker_id{};
    return host->start_worker(
        host->context,
        &worker_main,
        nullptr,
        nullptr,
        &worker_id);
}

void ENRAGE_CALL unload() {}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    ENRAGE_PLUGIN_DESCRIPTOR_V1_SIZE,
    "dev.enrage.worker-fault",
    "ENRAGE Worker Fault Fixture",
    "1.0.0",
    &load,
    &unload,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* ENRAGE_CALL enrage_plugin_entry(void) {
    return &descriptor;
}
