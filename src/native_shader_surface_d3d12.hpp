#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace zevryon::text::detail {

class D3D12ShaderSurfaceResolver final {
public:
    bool configure(
        ID3D12Device* device,
        DXGI_FORMAT render_target_format,
        HRESULT* native_error) noexcept;

    bool encode(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* source,
        ID3D12Resource* target,
        D3D12_CPU_DESCRIPTOR_HANDLE target_rtv,
        std::uint32_t width,
        std::uint32_t height,
        HRESULT* native_error) noexcept;

    void reset() noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
    DXGI_FORMAT render_target_format_{DXGI_FORMAT_UNKNOWN};
    UINT descriptor_increment_{0U};
    std::array<ID3D12Resource*, 16U> descriptor_targets_{};
};

} // namespace zevryon::text::detail

#endif
