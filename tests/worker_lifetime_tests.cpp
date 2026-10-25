#include "core/plugin_host.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using set_ignore_fn = void (ENRAGE_CALL *)(uint32_t);
using counter_fn = uint32_t (ENRAGE_CALL *)(void);

std::filesystem::path marker_path() {
    return std::filesystem::temp_directory_path() / "enrage-worker-lifetime-starts.log";
}

std::size_t marker_lines() {
    std::ifstream input(marker_path());
    std::size_t count{};
    std::string line;
    while (std::getline(input, line)) ++count;
    return count;
}

bool wait_for_lines(std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (marker_lines() >= expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return marker_lines() >= expected;
}

bool wait_for_counter(counter_fn counter, uint32_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter() >= expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return counter() >= expected;
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];
    std::filesystem::remove(marker_path());

    enrage::core::PluginHost host;
    assert(host.load(plugin_path, "example.worker-owner"));
    assert(host.plugins().size() == 1);
    assert(host.worker_count() == 1);
    assert(wait_for_lines(1));

    const auto first_shadow = host.plugins()[0].loaded_path;
    assert(host.reload("dev.enrage.worker-fixture"));
    assert(host.plugins().size() == 1);
    assert(host.worker_count() == 1);
    assert(host.plugins()[0].loaded_path != first_shadow);
    assert(!std::filesystem::exists(first_shadow));
    assert(wait_for_lines(2));

    // Make the current worker deliberately refuse the cooperative stop request.
    // Reload must time out without unloading this generation, and the staged
    // replacement worker must remain parked and never enter plugin code.
    auto* raw_set_ignore = host.plugins()[0].library.symbol("enrage_test_worker_set_ignore_stop");
    auto* raw_started = host.plugins()[0].library.symbol("enrage_test_worker_started");
    auto* raw_stopped = host.plugins()[0].library.symbol("enrage_test_worker_stopped");
    auto* raw_stop_requests = host.plugins()[0].library.symbol("enrage_test_worker_stop_requests");
    assert(raw_set_ignore && raw_started && raw_stopped && raw_stop_requests);

    const auto set_ignore = reinterpret_cast<set_ignore_fn>(raw_set_ignore);
    const auto started = reinterpret_cast<counter_fn>(raw_started);
    const auto stopped = reinterpret_cast<counter_fn>(raw_stopped);
    const auto stop_requests = reinterpret_cast<counter_fn>(raw_stop_requests);
    assert(started() >= 1);

    set_ignore(1);
    const auto live_shadow = host.plugins()[0].loaded_path;
    const auto starts_before_rejected_reload = marker_lines();
    assert(!host.reload("dev.enrage.worker-fixture"));
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].loaded_path == live_shadow);
    assert(std::filesystem::exists(live_shadow));
    assert(host.worker_count() == 1);
    assert(stop_requests() >= 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    assert(marker_lines() == starts_before_rejected_reload);

    // The old generation stayed loaded, so it is still safe to release its
    // test worker. Once it observes the already-published stop request, a
    // second unload can join it and remove the generation normally.
    set_ignore(0);
    assert(wait_for_counter(stopped, 1));
    assert(host.unload("dev.enrage.worker-fixture"));
    assert(host.plugins().empty());
    assert(host.worker_count() == 0);
    assert(!std::filesystem::exists(live_shadow));

    std::filesystem::remove(marker_path());
    return 0;
}
