#include "native_shader_surface_d3d12.hpp"

#if defined(_WIN32)

#include <d3dcompiler.h>

#include <array>
#include <cstring>

namespace zevryon::text::detail {
namespace {

constexpr const char* kResolveHlsl = R"HLSL(
Texture2D<uint> Source : register(t0);

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 position;
    if (vertex_id == 0U) {
        position = float2(-1.0, -1.0);
    } else if (vertex_id == 1U) {
        position = float2(-1.0, 3.0);
    } else {
        position = float2(3.0, -1.0);
    }
    output.position = float4(position, 0.0, 1.0);
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target0 {
    uint2 coordinate = uint2(input.position.xy);
    uint value = Source.Load(int3(coordinate, 0));
    float blue = float(value & 255U) / 255.0;
    float green = float((value >> 8U) & 255U) / 255.0;
    float red = float((value >> 16U) & 255U) / 255.0;
    float alpha = float((value >> 24U) & 255U) / 255.0;
    return float4(red, green, blue, alpha);
}
)HLSL";

void set_error(HRESULT* output, HRESULT value) noexcept {
    if (output != nullptr) {
        *output = value;
    }
}

} // namespace

bool D3D12ShaderSurfaceResolver::configure(
    ID3D12Device* device,
    DXGI_FORMAT render_target_format,
    HRESULT* native_error) noexcept {
    set_error(native_error, S_OK);
    if (device == nullptr || render_target_format == DXGI_FORMAT_UNKNOWN) {
        set_error(native_error, E_INVALIDARG);
        return false;
    }
    if (device_ != nullptr && render_target_format_ == render_target_format) {
        return true;
    }
    reset();

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixel_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    HRESULT result = D3DCompile(
        kResolveHlsl,
        std::strlen(kResolveHlsl),
        "zevryon_shader_surface_resolve.hlsl",
        nullptr,
        nullptr,
        "vertex_main",
        "vs_5_1",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &vertex_shader,
        &diagnostics);
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }
    diagnostics.Reset();
    result = D3DCompile(
        kResolveHlsl,
        std::strlen(kResolveHlsl),
        "zevryon_shader_surface_resolve.hlsl",
        nullptr,
        nullptr,
        "pixel_main",
        "ps_5_1",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &pixel_shader,
        &diagnostics);
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;
    range.RegisterSpace = 0U;
    range.OffsetInDescriptorsFromTableStart = 0U;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1U;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> root_error;
    result = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &root_error);
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    result = device->CreateRootSignature(
        0U,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&root_signature));
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_signature.Get();
    pipeline.VS.pShaderBytecode = vertex_shader->GetBufferPointer();
    pipeline.VS.BytecodeLength = vertex_shader->GetBufferSize();
    pipeline.PS.pShaderBytecode = pixel_shader->GetBufferPointer();
    pipeline.PS.BytecodeLength = pixel_shader->GetBufferSize();
    pipeline.BlendState.AlphaToCoverageEnable = FALSE;
    pipeline.BlendState.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC blend{
        FALSE,
        FALSE,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL};
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : pipeline.BlendState.RenderTarget) {
        target = blend;
    }
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.FrontCounterClockwise = FALSE;
    pipeline.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pipeline.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipeline.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.RasterizerState.MultisampleEnable = FALSE;
    pipeline.RasterizerState.AntialiasedLineEnable = FALSE;
    pipeline.RasterizerState.ForcedSampleCount = 0U;
    pipeline.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.InputLayout = {nullptr, 0U};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1U;
    pipeline.RTVFormats[0] = render_target_format;
    pipeline.SampleDesc.Count = 1U;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state;
    result = device->CreateGraphicsPipelineState(
        &pipeline,
        IID_PPV_ARGS(&pipeline_state));
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1U;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptor_heap));
    if (FAILED(result)) {
        set_error(native_error, result);
        return false;
    }

    device_ = device;
    root_signature_ = std::move(root_signature);
    pipeline_state_ = std::move(pipeline_state);
    descriptor_heap_ = std::move(descriptor_heap);
    render_target_format_ = render_target_format;
    return true;
}

bool D3D12ShaderSurfaceResolver::encode(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    ID3D12Resource* target,
    D3D12_CPU_DESCRIPTOR_HANDLE target_rtv,
    std::uint32_t width,
    std::uint32_t height,
    HRESULT* native_error) noexcept {
    set_error(native_error, S_OK);
    if (command_list == nullptr || source == nullptr || target == nullptr ||
        width == 0U || height == 0U || pipeline_state_ == nullptr ||
        root_signature_ == nullptr || descriptor_heap_ == nullptr || device_ == nullptr) {
        set_error(native_error, E_INVALIDARG);
        return false;
    }
    const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_desc.Format != DXGI_FORMAT_R32_UINT ||
        source_desc.Width != width || source_desc.Height != height ||
        source_desc.DepthOrArraySize != 1U) {
        set_error(native_error, E_INVALIDARG);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 0U;
    srv.Texture2D.MipLevels = 1U;
    srv.Texture2D.PlaneSlice = 0U;
    srv.Texture2D.ResourceMinLODClamp = 0.0F;
    device_->CreateShaderResourceView(
        source,
        &srv,
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER to_render{};
    to_render.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_render.Transition.pResource = target;
    to_render.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_render.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_render.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1U, &to_render);

    ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
    command_list->SetDescriptorHeaps(1U, heaps);
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootDescriptorTable(
        0U,
        descriptor_heap_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<FLOAT>(width),
        static_cast<FLOAT>(height),
        0.0F,
        1.0F};
    const D3D12_RECT scissor{
        0L,
        0L,
        static_cast<LONG>(width),
        static_cast<LONG>(height)};
    command_list->RSSetViewports(1U, &viewport);
    command_list->RSSetScissorRects(1U, &scissor);
    command_list->OMSetRenderTargets(1U, &target_rtv, FALSE, nullptr);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->DrawInstanced(3U, 1U, 0U, 0U);

    D3D12_RESOURCE_BARRIER to_present = to_render;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1U, &to_present);
    return true;
}

void D3D12ShaderSurfaceResolver::reset() noexcept {
    descriptor_heap_.Reset();
    pipeline_state_.Reset();
    root_signature_.Reset();
    device_.Reset();
    render_target_format_ = DXGI_FORMAT_UNKNOWN;
}

} // namespace zevryon::text::detail

#endif
