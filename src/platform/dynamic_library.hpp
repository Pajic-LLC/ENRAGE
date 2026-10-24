#pragma once

#include <filesystem>
#include <utility>

namespace enrage::platform {

class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    bool open(const std::filesystem::path& path);
    void close() noexcept;

    [[nodiscard]] void* symbol(const char* name) const noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    void* handle_{};
};

} // namespace enrage::platform
