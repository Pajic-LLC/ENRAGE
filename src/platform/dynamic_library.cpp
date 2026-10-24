#include "platform/dynamic_library.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace enrage::platform {

DynamicLibrary::~DynamicLibrary() {
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

bool DynamicLibrary::open(const std::filesystem::path& path) {
    close();
#if defined(_WIN32)
    handle_ = static_cast<void*>(LoadLibraryW(path.c_str()));
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    return handle_ != nullptr;
}

void DynamicLibrary::close() noexcept {
    if (!handle_) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (!handle_ || !name) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

bool DynamicLibrary::is_open() const noexcept {
    return handle_ != nullptr;
}

} // namespace enrage::platform
