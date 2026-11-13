#include "d3d11_text_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

// Defined in the generated embedded_shaders.cpp (the contents of shaders/text.hlsl).
namespace pat::engine::embedded {
extern const char* const kTextHlsl;
}

namespace pat::engine {

namespace {

using namespace d3d11;

constexpr UINT kMaxQuadsPerBatch = 8192;

struct Vertex {
    float x;
    float y;
    float u;
    float v;
    float color[4];
    float outline[4];
};

// Must match TextConstants in text.hlsl.
struct TextCB {
    float viewport[2];
    float outline_width;
    float halo_soft;
};
static_assert(sizeof(TextCB) == 16, "constant buffers are 16-byte aligned");

class D3D11TextRenderer final : public ITextRenderer {
public:
    D3D11TextRenderer(const D3D11Shared& shared, const SdfAtlas& atlas)
        : shared_(shared), default_outline_width_(
                               std::min(0.45f, 3.0f / (2.0f * std::max(atlas.spread, 1.0f)) * 1.0f + 0.02f)),
          outline_width_(default_outline_width_),
          em_to_field_(atlas.font_px / (2.0f * std::max(atlas.spread, 1.0f))) {}

    void set_halo(const TextHalo& halo) override {
        if (halo.width_em >= 0.0f) {
            // Width and softness are in ems; the field maps `spread` atlas pixels to half its range.
            outline_width_ = std::clamp(halo.width_em * em_to_field_, 0.0f, 0.45f);
            halo_soft_ = std::clamp(halo.softness_em * em_to_field_, 0.0f, 0.45f);
        } else {
            outline_width_ = default_outline_width_;
            halo_soft_ = 0.0f;
        }
    }

    [[nodiscard]] core::Result<void> init(const SdfAtlas& atlas) {
        auto* device = shared_.device.Get();

        // The atlas.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(atlas.width);
        td.Height = static_cast<UINT>(atlas.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = atlas.pixels.data();
        init.SysMemPitch = static_cast<UINT>(atlas.width);
        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&td, &init, &tex);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(110, "SDF atlas texture creation failed", hr));
        }
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, &atlas_srv_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(111, "SDF atlas view creation failed", hr));
        }

        // Shaders.
        ShaderSource source({}, "text.hlsl", embedded::kTextHlsl);
        auto text = source.load();
        if (!text) {
            return core::Result<void>::err(text.error());
        }
        auto vs_blob = compile_hlsl(text.value(), "text.hlsl", "vs_main", "vs_5_0");
        if (!vs_blob) {
            return core::Result<void>::err(vs_blob.error());
        }
        auto ps_blob = compile_hlsl(text.value(), "text.hlsl", "ps_main", "ps_5_0");
        if (!ps_blob) {
            return core::Result<void>::err(ps_blob.error());
        }
        hr = device->CreateVertexShader(
            vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), nullptr, &vs_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(112, "text vertex shader creation failed", hr));
        }
        hr = device->CreatePixelShader(
            ps_blob.value()->GetBufferPointer(), ps_blob.value()->GetBufferSize(), nullptr, &ps_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(113, "text pixel shader creation failed", hr));
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",
             0,
             DXGI_FORMAT_R32G32B32A32_FLOAT,
             0,
             offsetof(Vertex, color),
             D3D11_INPUT_PER_VERTEX_DATA,
             0},
            {"COLOR",
             1,
             DXGI_FORMAT_R32G32B32A32_FLOAT,
             0,
             offsetof(Vertex, outline),
             D3D11_INPUT_PER_VERTEX_DATA,
             0},
        };
        hr = device->CreateInputLayout(
            elements, 4, vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), &layout_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(114, "text input layout creation failed", hr));
        }

        // Buffers and state.
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(sizeof(Vertex) * 6 * kMaxQuadsPerBatch);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&bd, nullptr, &vertex_buffer_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(115, "text vertex buffer creation failed", hr));
        }
        bd.ByteWidth = sizeof(TextCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&bd, nullptr, &constants_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(116, "text constant buffer creation failed", hr));
        }

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&bld, &blend_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(117, "text blend state creation failed", hr));
        }
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(118, "text rasterizer state creation failed", hr));
        }
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        hr = device->CreateDepthStencilState(&dd, &depth_off_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(119, "text depth state creation failed", hr));
        }
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(120, "text sampler creation failed", hr));
        }
        return core::Result<void>::ok();
    }

    void draw(std::span<const TextQuad> quads) override {
        if (quads.empty() || shared_.target_size == nullptr) {
            return;
        }
        auto* ctx = shared_.context.Get();

        TextCB cb{};
        cb.viewport[0] = static_cast<float>(std::max(shared_.target_size->w, 1));
        cb.viewport[1] = static_cast<float>(std::max(shared_.target_size->h, 1));
        cb.outline_width = outline_width_;
        cb.halo_soft = halo_soft_;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(constants_.Get(), 0);

        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vb = vertex_buffer_.Get();
        ID3D11Buffer* cbuf = constants_.Get();
        ID3D11ShaderResourceView* srv = atlas_srv_.Get();
        ID3D11SamplerState* sampler = sampler_.Get();
        const float blend_factor[4] = {0, 0, 0, 0};
        ctx->IASetInputLayout(layout_.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);  // the pixel shader reads outline_width
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(depth_off_.Get(), 0);
        ctx->RSSetState(raster_.Get());

        std::size_t index = 0;
        while (index < quads.size()) {
            const std::size_t count = std::min<std::size_t>(quads.size() - index, kMaxQuadsPerBatch);
            if (FAILED(ctx->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                break;
            }
            auto* v = static_cast<Vertex*>(mapped.pData);
            for (std::size_t i = 0; i < count; ++i) {
                const TextQuad& q = quads[index + i];
                const TextQuadGeometry& g = q.geometry;
                const auto make = [&](float x, float y, float u, float vv) {
                    Vertex out{x,
                               y,
                               u,
                               vv,
                               {q.color.r, q.color.g, q.color.b, q.color.a},
                               {q.outline.r, q.outline.g, q.outline.b, q.outline.a}};
                    return out;
                };
                const Vertex a = make(g.x, g.y, g.u0, g.v0);
                const Vertex b = make(g.x + g.w, g.y, g.u1, g.v0);
                const Vertex c = make(g.x + g.w, g.y + g.h, g.u1, g.v1);
                const Vertex d = make(g.x, g.y + g.h, g.u0, g.v1);
                v[0] = a;
                v[1] = b;
                v[2] = c;
                v[3] = a;
                v[4] = c;
                v[5] = d;
                v += 6;
            }
            ctx->Unmap(vertex_buffer_.Get(), 0);
            ctx->Draw(static_cast<UINT>(count * 6), 0);
            index += count;
        }

        ID3D11ShaderResourceView* none[1] = {};
        ctx->PSSetShaderResources(0, 1, none);
        if (shared_.invalidate_pipeline != nullptr) {
            *shared_.invalidate_pipeline = true;
        }
    }

private:
    D3D11Shared shared_;
    float default_outline_width_;
    float outline_width_;
    float halo_soft_ = 0.0f;
    float em_to_field_;  // field units per em of the font size
    ComPtr<ID3D11ShaderResourceView> atlas_srv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_off_;
    ComPtr<ID3D11SamplerState> sampler_;
};

}  // namespace

core::Result<std::unique_ptr<ITextRenderer>> create_d3d11_text_renderer(const D3D11Shared& shared,
                                                                        const SdfAtlas& atlas) {
    using R = core::Result<std::unique_ptr<ITextRenderer>>;
    if (atlas.width <= 0 || atlas.height <= 0 ||
        atlas.pixels.size() !=
            static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height)) {
        return R::err(core::Error{121, "invalid SDF atlas"});
    }
    auto renderer = std::make_unique<D3D11TextRenderer>(shared, atlas);
    if (auto r = renderer->init(atlas); !r) {
        return R::err(r.error());
    }
    return R::ok(std::move(renderer));
}

}  // namespace pat::engine
