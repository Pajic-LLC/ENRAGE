#include "adapters/null_adapter.hpp"

#include "enrage/sdk.h"

namespace enrage::adapters {

std::string_view NullAdapter::name() const noexcept {
    return "development";
}

bool NullAdapter::probe() {
    return true;
}

BuildIdentity NullAdapter::build() const {
    return {"development", 0};
}

std::uint64_t NullAdapter::capabilities() const noexcept {
    return ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES;
}

EnvironmentState NullAdapter::environment_state() const noexcept {
    return EnvironmentState::local;
}

bool NullAdapter::initialize() {
    return true;
}

void NullAdapter::shutdown() noexcept {}

} // namespace enrage::adapters
