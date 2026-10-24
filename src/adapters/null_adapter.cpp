#include "adapters/null_adapter.hpp"

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
    return 0;
}

bool NullAdapter::initialize() {
    return true;
}

void NullAdapter::shutdown() noexcept {}

} // namespace enrage::adapters
