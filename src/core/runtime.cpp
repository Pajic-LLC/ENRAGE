#include "core/runtime.hpp"

#include <utility>

namespace enrage::core {

Runtime::Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root)
    : adapter_(std::move(adapter)), mods_(std::move(mod_root)) {}

Runtime::~Runtime() {
    stop();
}

bool Runtime::start() {
    if (running_ || !adapter_ || !adapter_->probe()) {
        return false;
    }

    if (!adapter_->initialize()) {
        return false;
    }

    mods_.discover();
    running_ = true;
    return true;
}

void Runtime::stop() noexcept {
    if (!running_) {
        return;
    }

    adapter_->shutdown();
    running_ = false;
}

bool Runtime::running() const noexcept {
    return running_;
}

const ModRegistry& Runtime::mods() const noexcept {
    return mods_;
}

const adapters::Adapter& Runtime::adapter() const noexcept {
    return *adapter_;
}

} // namespace enrage::core
