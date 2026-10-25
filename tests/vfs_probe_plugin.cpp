#include "enrage/sdk.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace {

enrage_result ENRAGE_CALL load(const enrage_host_api* host) {
    if (!host) return ENRAGE_INVALID_ARGUMENT;
    if (host->abi_version != ENRAGE_ABI_VERSION ||
        host->struct_size < ENRAGE_HOST_API_V1_VFS_SIZE ||
        !host->capabilities || !host->vfs_read) {
        return ENRAGE_INCOMPATIBLE_ABI;
    }
    if ((host->capabilities(host->context) & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0) {
        return ENRAGE_UNSUPPORTED;
    }

    std::size_t required{};
    auto result = host->vfs_read(
        host->context,
        "vfs/probe.txt",
        nullptr,
        0,
        &required);
    if (result != ENRAGE_BUFFER_TOO_SMALL || required != 5) return ENRAGE_INTERNAL_ERROR;

    std::array<char, 5> bytes{};
    result = host->vfs_read(
        host->context,
        "vfs/probe.txt",
        bytes.data(),
        bytes.size(),
        &required);
    if (result != ENRAGE_OK || required != bytes.size() ||
        std::memcmp(bytes.data(), "hello", bytes.size()) != 0) {
        return ENRAGE_INTERNAL_ERROR;
    }

    required = 123;
    result = host->vfs_read(
        host->context,
        "vfs/missing.txt",
        nullptr,
        0,
        &required);
    if (result != ENRAGE_NOT_FOUND || required != 0) return ENRAGE_INTERNAL_ERROR;

    return ENRAGE_OK;
}

void ENRAGE_CALL unload() {}

const enrage_plugin_descriptor descriptor{
    ENRAGE_ABI_VERSION,
    static_cast<uint32_t>(sizeof(enrage_plugin_descriptor)),
    "dev.enrage.vfs-probe",
    "ENRAGE VFS Probe",
    "1.0.0",
    &load,
    &unload,
};

} // namespace

extern "C" ENRAGE_EXPORT const enrage_plugin_descriptor* ENRAGE_CALL enrage_plugin_entry(void) {
    return &descriptor;
}
