#include "d3d11_ui_renderer.hpp"

#include "pat/core/log.hpp"

#include <RmlUi/Core/Vertex.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

// Defined in the generated embedded_shaders.cpp (the contents of shaders/ui.hlsl).
namespace pat::engine::embedded {
extern const char* const kUiHlsl;
}

namespace pat::engine {

namespace {

using namespace d3d11;

// Must match UiConstants in ui.hlsl.
struct UiCB {
    float transform[16];
    float translation[2];
    float viewport[2];
};
static_assert(sizeof(UiCB) % 16 == 0, "constant buffers are 16-byte aligned");

struct CompiledGeometry {
    ComPtr<ID3D11Buffer> vertices;
    ComPtr<ID3D11Buffer> indices;
    UINT index_count = 0;
};

struct Texture {
    ComPtr<ID3D11ShaderResourceView> view;
};

class D3D11UiRenderer final : public Rml::RenderInterface {
public:
    explicit D3D11UiRenderer(const D3D11Shared& shared) : shared_(shared) {}

    [[nodiscard]] core::Result<void> init() {
        ShaderSource source({}, "ui.hlsl", embedded::kUiHlsl);
        auto text = source.load();
        if (!text) {
            return core::Result<void>::err(text.error());
        }
        auto vs_blob = compile_hlsl(text.value(), "ui.hlsl", "vs_main", "vs_5_0");
        if (!vs_blob) {
            return core::Result<void>::err(vs_blob.error());
        }
        auto ps_blob = compile_hlsl(text.value(), "ui.hlsl", "ps_main", "ps_5_0");
        if (!ps_blob) {
            return core::Result<void>::err(ps_blob.error());
        }
        auto* device = shared_.device.Get();
        HRESULT hr = device->CreateVertexShader(
            vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), nullptr, &vs_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(70, "UI vertex shader creation failed", hr));
        }
        hr = device->CreatePixelShader(
            ps_blob.value()->GetBufferPointer(), ps_blob.value()->GetBufferSize(), nullptr, &ps_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(71, "UI pixel shader creation failed", hr));
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION",
             0,
             DXGI_FORMAT_R32G32_FLOAT,
             0,
             offsetof(Rml::Vertex, position),
             D3D11_INPUT_PER_VERTEX_DATA,
             0},
            {"COLOR",
             0,
             DXGI_FORMAT_R8G8B8A8_UNORM,
             0,
             offsetof(Rml::Vertex, colour),
             D3D11_INPUT_PER_VERTEX_DATA,
             0},
            {"TEXCOORD",
             0,
             DXGI_FORMAT_R32G32_FLOAT,
             0,
             offsetof(Rml::Vertex, tex_coord),
             D3D11_INPUT_PER_VERTEX_DATA,
             0},
        };
        hr = device->CreateInputLayout(
            elements, 3, vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), &layout_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(72, "UI input layout creation failed", hr));
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(UiCB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&bd, nullptr, &constants_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(73, "UI constant buffer creation failed", hr));
        }

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // premultiplied alpha
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&bld, &blend_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(74, "UI blend state creation failed", hr));
        }

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        hr = device->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(75, "UI rasterizer state creation failed", hr));
        }

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        hr = device->CreateDepthStencilState(&dd, &depth_off_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(76, "UI depth state creation failed", hr));
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(77, "UI sampler creation failed", hr));
        }

        // 1x1 white texture for untextured geometry.
        const std::uint32_t white = 0xFFFFFFFFU;
        auto tex = make_texture(reinterpret_cast<const Rml::byte*>(&white), {1, 1});
        if (tex == 0) {
            return core::Result<void>::err(core::Error{78, "UI white texture creation failed"});
        }
        white_ = reinterpret_cast<Texture*>(tex);
        return core::Result<void>::ok();
    }

    ~D3D11UiRenderer() override { delete white_; }

    // ---------------------------------------------------------------- Rml::RenderInterface

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (vertices.size() == 0 || indices.size() == 0) {
            return 0;
        }
        auto geometry = std::make_unique<CompiledGeometry>();
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA init{};

        bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init.pSysMem = vertices.data();
        if (FAILED(shared_.device->CreateBuffer(&bd, &init, &geometry->vertices))) {
            return 0;
        }
        bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(int));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = indices.data();
        if (FAILED(shared_.device->CreateBuffer(&bd, &init, &geometry->indices))) {
            return 0;
        }
        geometry->index_count = static_cast<UINT>(indices.size());
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        auto* geometry = reinterpret_cast<CompiledGeometry*>(handle);
        auto* tex = texture != 0 ? reinterpret_cast<Texture*>(texture) : white_;
        if (geometry == nullptr || tex == nullptr || shared_.target_size == nullptr) {
            return;
        }
        auto* ctx = shared_.context.Get();
        bind_state();  // cheap; keeps the UI independent of whatever other passes bound in between
        if (shared_.invalidate_pipeline != nullptr) {
            *shared_.invalidate_pipeline = true;
        }

        UiCB cb{};
        if (has_transform_) {
            std::memcpy(cb.transform, transform_.data(), sizeof(cb.transform));
        } else {
            const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(cb.transform, identity.data(), sizeof(cb.transform));
        }
        cb.translation[0] = translation.x;
        cb.translation[1] = translation.y;
        cb.viewport[0] = static_cast<float>(std::max(shared_.target_size->w, 1));
        cb.viewport[1] = static_cast<float>(std::max(shared_.target_size->h, 1));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(constants_.Get(), 0);

        const UINT stride = sizeof(Rml::Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vb = geometry->vertices.Get();
        ID3D11ShaderResourceView* srv = tex->view.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetIndexBuffer(geometry->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->DrawIndexed(geometry->index_count, 0, 0);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<CompiledGeometry*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
        // Image decoding arrives with the art pipeline (E26); the spike UI uses no image files.
        (void)texture_dimensions;
        PAT_LOG_WARN(kLogCat, "UI image loading is not implemented yet: '{}'", source);
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        if (source.size() <
            static_cast<std::size_t>(dimensions.x) * static_cast<std::size_t>(dimensions.y) * 4) {
            return 0;
        }
        return make_texture(source.data(), dimensions);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override { delete reinterpret_cast<Texture*>(texture); }

    void EnableScissorRegion(bool enable) override {
        scissor_enabled_ = enable;
        if (!enable) {
            apply_scissor_full();
        } else {
            apply_scissor(scissor_);
        }
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = region;
        if (scissor_enabled_) {
            apply_scissor(region);
        }
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        has_transform_ = transform != nullptr;
        if (transform != nullptr) {
            transform_ = *transform;
        }
    }

private:
    Rml::TextureHandle make_texture(const Rml::byte* data, Rml::Vector2i dimensions) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(dimensions.x);
        td.Height = static_cast<UINT>(dimensions.y);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = data;
        init.SysMemPitch = static_cast<UINT>(dimensions.x) * 4U;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(shared_.device->CreateTexture2D(&td, &init, &tex))) {
            return 0;
        }
        auto texture = std::make_unique<Texture>();
        if (FAILED(shared_.device->CreateShaderResourceView(tex.Get(), nullptr, &texture->view))) {
            return 0;
        }
        return reinterpret_cast<Rml::TextureHandle>(texture.release());
    }

    void bind_state() {
        auto* ctx = shared_.context.Get();
        ctx->IASetInputLayout(layout_.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cb = constants_.Get();
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = sampler_.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
        const float blend_factor[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(depth_off_.Get(), 0);
        ctx->RSSetState(raster_.Get());
        if (scissor_enabled_) {
            apply_scissor(scissor_);
        } else {
            apply_scissor_full();
        }
    }

    void apply_scissor(Rml::Rectanglei r) {
        const D3D11_RECT rect{r.Left(), r.Top(), r.Right(), r.Bottom()};
        shared_.context->RSSetScissorRects(1, &rect);
    }

    void apply_scissor_full() {
        if (shared_.target_size == nullptr) {
            return;
        }
        const D3D11_RECT rect{0, 0, std::max(shared_.target_size->w, 0), std::max(shared_.target_size->h, 0)};
        shared_.context->RSSetScissorRects(1, &rect);
    }

    D3D11Shared shared_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_off_;
    ComPtr<ID3D11SamplerState> sampler_;
    Texture* white_ = nullptr;

    bool scissor_enabled_ = false;
    Rml::Rectanglei scissor_;
    bool has_transform_ = false;
    Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
};

}  // namespace

core::Result<std::unique_ptr<Rml::RenderInterface>> create_d3d11_ui_renderer(const D3D11Shared& shared) {
    auto renderer = std::make_unique<D3D11UiRenderer>(shared);
    if (auto r = renderer->init(); !r) {
        return core::Result<std::unique_ptr<Rml::RenderInterface>>::err(r.error());
    }
    return core::Result<std::unique_ptr<Rml::RenderInterface>>::ok(std::move(renderer));
}

}  // namespace pat::engine
