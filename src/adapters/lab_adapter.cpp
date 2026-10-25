#include "adapters/lab_adapter.hpp"

#include <utility>

namespace enrage::adapters {

LabAdapter::LabAdapter() : LabAdapter(std::make_shared<LabState>()) {}

LabAdapter::LabAdapter(std::shared_ptr<LabState> state)
    : state_(state ? std::move(state) : std::make_shared<LabState>()) {}

std::string_view LabAdapter::name() const noexcept {
    return state_->adapter_name;
}

bool LabAdapter::probe() {
    ++state_->probe_count;
    state_->events.push_back("probe");
    return state_->probe_result;
}

BuildIdentity LabAdapter::build() const {
    return state_->build;
}

std::uint64_t LabAdapter::capabilities() const noexcept {
    return state_->capability_map.resolve(state_->capability_bits);
}

EnvironmentState LabAdapter::environment_state() const noexcept {
    return state_->environment;
}

bool LabAdapter::initialize() {
    ++state_->initialize_count;
    state_->events.push_back("initialize");
    if (!state_->initialize_result) return false;
    state_->initialized = true;
    return true;
}

void LabAdapter::shutdown() noexcept {
    ++state_->shutdown_count;
    state_->events.push_back("shutdown");
    state_->initialized = false;
}

const std::shared_ptr<LabState>& LabAdapter::state() const noexcept {
    return state_;
}

} // namespace enrage::adapters
