#include "native_shader_surface_vulkan.hpp"

#if defined(ZEVRYON_HAS_VULKAN_WSI)

#include <cassert>
#include <cstdint>

namespace {

zevryon::text::NativeShaderSurfaceView valid_view() {
    using namespace zevryon::text;
    NativeShaderSurfaceView view{};
    view.api_kind = NativeGpuApiKind::Vulkan;
    view.format = GpuSurfaceFormat::Bgra8Unorm;
    view.state = NativeShaderSurfaceState::ShaderRead;
    view.flags = kNativeShaderSurfaceReady |
        kNativeShaderSurfaceNonOwning |
        kNativeShaderSurfacePremultipliedAlpha;
    view.device_generation = 11U;
    view.runtime_generation = 12U;
    view.executor_generation = 13U;
    view.output_generation = 14U;
    view.frame_id = 15U;
    view.content_checksum = 16U;
    view.native_resource = 17U;
    view.width = 640U;
    view.height = 360U;
    return view;
}

bool decodes(const zevryon::text::NativeShaderSurfaceView& view) {
    zevryon::text::detail::VulkanShaderSurfaceSource source{};
    return zevryon::text::detail::decode_vulkan_shader_surface(
        view, 11U, 12U, 15U, 16U, 640U, 360U, &source);
}

} // namespace

int main() {
    using namespace zevryon::text;
    using namespace zevryon::text::detail;

    NativeShaderSurfaceView view = valid_view();
    VulkanShaderSurfaceSource source{};
    assert(decode_vulkan_shader_surface(
        view, 11U, 12U, 15U, 16U, 640U, 360U, &source));
    assert(source.image != VK_NULL_HANDLE);
    assert(vulkan_shader_surface_resource_id(source.image) ==
           view.native_resource);
    assert(source.output_generation == 14U);
    assert(source.frame_id == 15U);
    assert(source.content_checksum == 16U);
    assert(source.width == 640U);
    assert(source.height == 360U);

    NativeShaderSurfaceView invalid = view;
    invalid.api_kind = NativeGpuApiKind::Direct3D12;
    assert(!decodes(invalid));

    invalid = view;
    invalid.device_generation += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.runtime_generation += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.frame_id += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.content_checksum += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.width += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.height += 1U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.native_resource = 0U;
    assert(!decodes(invalid));

    invalid = view;
    invalid.state = NativeShaderSurfaceState::Undefined;
    assert(!decodes(invalid));

    assert(!decode_vulkan_shader_surface(
        view, 11U, 12U, 15U, 16U, 640U, 360U, nullptr));

    VkImageLayout layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    assert(!encode_vulkan_shader_surface_copy(
        VK_NULL_HANDLE, source, VK_NULL_HANDLE, &layout));

    VulkanShaderSurfaceSource empty{};
    assert(!encode_vulkan_shader_surface_copy(
        VK_NULL_HANDLE, empty, VK_NULL_HANDLE, nullptr));
    return 0;
}

#endif
