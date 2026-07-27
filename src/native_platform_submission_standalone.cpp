#include "native_platform_adapters.hpp"

namespace zevryon::text {

NativePlatformSubmission::NativePlatformSubmission(
    std::pmr::memory_resource* resource)
    : commands(resource), barriers(resource), descriptors(resource) {}

std::pmr::memory_resource* NativePlatformSubmission::resource() const noexcept {
    return commands.get_allocator().resource();
}

void NativePlatformSubmission::release() noexcept {
    commands.clear();
    barriers.clear();
    descriptors.clear();
    api_kind = NativeGpuApiKind::ReferenceCpu;
    surface = {};
    image = {};
    frame_id = 0U;
    ticket_id = 0U;
    wait_fence_value = 0U;
    command_generation = 0U;
    source_command_checksum = 0U;
    encoded_checksum = 0U;
}

} // namespace zevryon::text
