#include "enrage/sdk.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {

const enrage_host_api* host_api{};
enrage_worker_id worker_id{ENRAGE_WORKER_ID_INVALID};
std::atomic_bool ignore_stop{};
std::atomic_uint32_t started{};
std::atomic_uint32_t stopped{};
std::atomic_uint32_t stop_requests{};
std::mutex wake_mutex;
std::condition_variable wake_cv;

std::filesystem::path marker_path() {
    return std::filesystem::temp_directory_path() / "enrage-worker-lifetime-starts.log";
}

void ENRAGE_CALL worker_main(enrage_worker_id id, void*) {
    {
        std::ofstream marker(marker_path(), std::ios::app);
        marker << "start\n";
    }
    started.fetch_add(1, std::memory_order_release);

    for (;;) {
        const auto* host = host_api;
        if (!ignore_stop.load(std::memory_order_acquire) && host &&
            host->worker_stop_requested(host->context, id) != 0) {
            break;
        }

        std::unique_lock lock(wake_mutex);
        wake_cv.wait_for(lock, std::chrono::milliseconds{10});
    }

    stopped.fetch_add(1, std::memory_order_release);
}

void ENRAGE_CALL request_stop(enrage_worker_id, void*) {
    stop_requests.fetch_add(1, std::memory_order_release);
    wake_cv.notify_all();
}

enrage_result ENRAGE_CALL load(const enrage_host_api* host) {
    if (!host || host->abi_version != ENRAGE_ABI_VERSION ||
        host->struct_size < ENRAGE_HOST_API_V1_WORKER_SIZE ||
        !host->start_worker || !host->worker_stop_requested) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }

    host_api = host;
    worker_id = ENRAGE_WORKER_ID_INVALID;
    return host->start_worker(
        host->context,
        &worker_main,
        &request_stop,
        nullptr,
        &worker_id);
}

void ENRAGE_CALL unload() {
    host_api = nullptr;
    worker_id = ENRAGE_WORKER_ID_INVALID;
}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.worker-fixture",
    "Worker Fixture",
    "1.0.0",
    &load,
    &unload,
    nullptr,
    nullptr,
};

} // namespace

extern "C" ENRAGE_EXPORT void ENRAGE_CALL enrage_test_worker_set_ignore_stop(uint32_t value) {
    ignore_stop.store(value != 0, std::memory_order_release);
    wake_cv.notify_all();
}

extern "C" ENRAGE_EXPORT uint32_t ENRAGE_CALL enrage_test_worker_started(void) {
    return started.load(std::memory_order_acquire);
}

extern "C" ENRAGE_EXPORT uint32_t ENRAGE_CALL enrage_test_worker_stopped(void) {
    return stopped.load(std::memory_order_acquire);
}

extern "C" ENRAGE_EXPORT uint32_t ENRAGE_CALL enrage_test_worker_stop_requests(void) {
    return stop_requests.load(std::memory_order_acquire);
}

ENRAGE_EXPORT const enrage_plugin_descriptor* ENRAGE_CALL enrage_plugin_entry(void) {
    return &descriptor;
}
