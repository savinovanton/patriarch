#include "pat/core/assert.hpp"
#include "pat/core/log.hpp"
#include "pat/engine/renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "d3d11_common.hpp"
#include "d3d11_map_view.hpp"
#include "d3d11_ui_renderer.hpp"

// Defined in the generated embedded_shaders.cpp (the contents of shaders/quad.hlsl).
namespace pat::engine::embedded {
extern const char* const kQuadHlsl;
}

namespace pat::engine {

namespace {

using namespace d3d11;

constexpr const char* kQuadShaderFile = "quad.hlsl";
constexpr UINT kMaxQuadsPerBatch = 8192;
constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

struct FrameConstants {
    float viewport_w;
    float viewport_h;
    float padding[2];
};
static_assert(sizeof(FrameConstants) == 16, "constant buffers are 16-byte aligned");

struct ShaderPair {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
};

class D3D11Renderer final : public IRenderer {
public:
    [[nodiscard]] core::Result<void> init(const RendererDesc& desc) {
        vsync_ = desc.vsync;
        shader_dir_ = desc.shader_dir;
        quad_source_ = std::make_unique<ShaderSource>(desc.shader_dir, kQuadShaderFile, embedded::kQuadHlsl);
        size_ = desc.size;
        windowed_ = desc.native_window != nullptr;

        if (auto r = create_device(desc); !r) {
            return r;
        }
        if (windowed_) {
            if (auto r = create_swapchain(static_cast<HWND>(desc.native_window)); !r) {
                return r;
            }
        }
        if (auto r = create_target(); !r) {
            return r;
        }
        if (auto r = create_pipeline_objects(); !r) {
            return r;
        }
        if (auto r = build_shaders(); !r) {
            return r;
        }
        if (context_.As(&annotation_) != S_OK) {
            annotation_.Reset();  // Annotations are optional (only used by capture tools).
        }
        PAT_LOG_INFO(kLogCat,
                     "D3D11 renderer ready: adapter '{}', feature level {}.{}, {}",
                     adapter_name_,
                     static_cast<int>(feature_level_) >> 12,
                     (static_cast<int>(feature_level_) >> 8) & 0xF,
                     windowed_ ? "swapchain" : "offscreen target");
        return core::Result<void>::ok();
    }

    // ---------------------------------------------------------------- IRenderer

    void resize(PixelSize size) override {
        size = {std::max(size.w, 0), std::max(size.h, 0)};
        if (size == size_) {
            return;
        }
        size_ = size;
        rtv_.Reset();
        dsv_.Reset();
        depth_.Reset();
        target_.Reset();
        staging_.Reset();
        if (size_.w == 0 || size_.h == 0) {
            return;  // Minimized: wait for a real size.
        }
        if (windowed_) {
            const HRESULT hr = swapchain_->ResizeBuffers(0,
                                                         static_cast<UINT>(size_.w),
                                                         static_cast<UINT>(size_.h),
                                                         DXGI_FORMAT_UNKNOWN,
                                                         swapchain_flags_);
            if (FAILED(hr)) {
                PAT_LOG_ERROR(kLogCat, "ResizeBuffers failed: {}", hr_string(hr));
                device_lost_ = true;
                return;
            }
        }
        if (auto r = create_target(); !r) {
            PAT_LOG_ERROR(kLogCat, "{}", r.error().message);
            device_lost_ = true;
        }
    }

    [[nodiscard]] PixelSize size() const override { return size_; }

    [[nodiscard]] bool begin_frame(const Color& clear) override {
        stats_ = {};
        frame_active_ = false;
        if (device_lost_ || size_.w <= 0 || size_.h <= 0 || !rtv_) {
            return false;
        }

        FrameConstants fc{};
        fc.viewport_w = static_cast<float>(size_.w);
        fc.viewport_h = static_cast<float>(size_.h);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &fc, sizeof(fc));
            context_->Unmap(constants_.Get(), 0);
        }

        const float color[4] = {clear.r, clear.g, clear.b, clear.a};
        ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
        context_->OMSetRenderTargets(1, rtvs, dsv_.Get());
        context_->ClearRenderTargetView(rtv_.Get(), color);
        context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        D3D11_VIEWPORT vp{};
        vp.Width = fc.viewport_w;
        vp.Height = fc.viewport_h;
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        bind_pipeline();
        pipeline_dirty_ = false;
        frame_active_ = true;
        return true;
    }

    void draw_quads(std::span<const Quad> quads) override {
        PAT_ASSERT(frame_active_, "draw_quads outside begin_frame/end_frame");
        if (!frame_active_ || quads.empty()) {
            return;
        }
        if (pipeline_dirty_) {
            bind_pipeline();
            pipeline_dirty_ = false;
        }
        std::size_t index = 0;
        while (index < quads.size()) {
            const std::size_t count = std::min<std::size_t>(quads.size() - index, kMaxQuadsPerBatch);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            auto* v = static_cast<Vertex*>(mapped.pData);
            for (std::size_t i = 0; i < count; ++i) {
                const Quad& q = quads[index + i];
                const float x0 = q.x;
                const float y0 = q.y;
                const float x1 = q.x + q.w;
                const float y1 = q.y + q.h;
                const Color& c = q.color;
                const Vertex a{x0, y0, c.r, c.g, c.b, c.a};
                const Vertex b{x1, y0, c.r, c.g, c.b, c.a};
                const Vertex d{x1, y1, c.r, c.g, c.b, c.a};
                const Vertex e{x0, y1, c.r, c.g, c.b, c.a};
                v[0] = a;
                v[1] = b;
                v[2] = d;
                v[3] = a;
                v[4] = d;
                v[5] = e;
                v += 6;
            }
            context_->Unmap(vertex_buffer_.Get(), 0);
            context_->Draw(static_cast<UINT>(count * 6), 0);
            ++stats_.draw_calls;
            stats_.quads += static_cast<std::uint32_t>(count);
            index += count;
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> capture() override {
        std::vector<std::uint8_t> pixels;
        if (!frame_active_ || !target_) {
            return pixels;
        }
        if (!staging_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(size_.w);
            td.Height = static_cast<UINT>(size_.h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = kTargetFormat;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
                return pixels;
            }
        }
        context_->CopyResource(staging_.Get(), target_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return pixels;
        }
        const std::size_t row = static_cast<std::size_t>(size_.w) * 4;
        pixels.resize(row * static_cast<std::size_t>(size_.h));
        for (int y = 0; y < size_.h; ++y) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(y) * mapped.RowPitch,
                        row);
        }
        context_->Unmap(staging_.Get(), 0);
        return pixels;
    }

    void end_frame() override {
        if (!frame_active_) {
            return;
        }
        frame_active_ = false;
        if (!windowed_) {
            context_->Flush();
            return;
        }
        const UINT sync = vsync_ ? 1U : 0U;
        const UINT flags = (!vsync_ && tearing_supported_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;
        const HRESULT hr = swapchain_->Present(sync, flags);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            PAT_LOG_ERROR(kLogCat,
                          "device lost on Present: {} (removed reason {})",
                          hr_string(hr),
                          hr_string(device_->GetDeviceRemovedReason()));
            device_lost_ = true;
        } else if (FAILED(hr)) {
            PAT_LOG_WARN(kLogCat, "Present failed: {}", hr_string(hr));
        }
    }

    void set_vsync(bool enabled) override { vsync_ = enabled; }
    [[nodiscard]] bool vsync() const override { return vsync_; }

    bool reload_shaders_if_changed() override {
        bool reloaded = false;
        if (quad_source_->changed()) {
            PAT_LOG_INFO(kLogCat, "shader source changed, recompiling '{}'", kQuadShaderFile);
            const std::uint32_t before = shader_generation_;
            auto r = build_shaders();
            if (!r) {
                PAT_LOG_ERROR(
                    kLogCat, "shader reload failed, keeping the previous shader:\n{}", r.error().message);
            } else {
                reloaded = shader_generation_ != before;
            }
        }
        return reloaded;
    }

    [[nodiscard]] std::uint32_t shader_generation() const override { return shader_generation_; }

    void push_marker(std::string_view name) override {
        if (annotation_) {
            annotation_->BeginEvent(utf8_to_wide(name).c_str());
        }
    }
    void pop_marker() override {
        if (annotation_) {
            annotation_->EndEvent();
        }
    }

    core::Result<std::unique_ptr<IMapView>> create_map_view(const mapdata::MapData& map,
                                                            const MapViewDesc& desc) override {
        D3D11Shared shared;
        shared.device = device_;
        shared.context = context_;
        shared.shader_dir = shader_dir_;
        shared.invalidate_pipeline = &pipeline_dirty_;
        return create_d3d11_map_view(shared, map, desc);
    }

    core::Result<std::unique_ptr<Rml::RenderInterface>> create_ui_render_interface() override {
        D3D11Shared shared;
        shared.device = device_;
        shared.context = context_;
        shared.shader_dir = shader_dir_;
        shared.invalidate_pipeline = &pipeline_dirty_;
        shared.target_size = &size_;
        return create_d3d11_ui_renderer(shared);
    }

    [[nodiscard]] std::string_view backend_name() const override { return "Direct3D 11"; }
    [[nodiscard]] std::string_view adapter_name() const override { return adapter_name_; }
    [[nodiscard]] const FrameStats& last_frame_stats() const override { return stats_; }

private:
    // ---------------------------------------------------------------- creation

    [[nodiscard]] core::Result<void> create_device(const RendererDesc& desc) {
        const std::array<D3D_FEATURE_LEVEL, 2> levels = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        bool want_debug = desc.debug_layer;
        D3D_DRIVER_TYPE driver = desc.software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;

        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const UINT f = flags | (want_debug ? D3D11_CREATE_DEVICE_DEBUG : 0U);
            hr = D3D11CreateDevice(nullptr,
                                   driver,
                                   nullptr,
                                   f,
                                   levels.data(),
                                   static_cast<UINT>(levels.size()),
                                   D3D11_SDK_VERSION,
                                   &device_,
                                   &feature_level_,
                                   &context_);
            if (SUCCEEDED(hr)) {
                break;
            }
            if (want_debug) {
                PAT_LOG_WARN(kLogCat, "debug layer unavailable ({}), continuing without it", hr_string(hr));
                want_debug = false;
            } else if (driver == D3D_DRIVER_TYPE_HARDWARE) {
                PAT_LOG_WARN(kLogCat, "no hardware device ({}), falling back to WARP", hr_string(hr));
                driver = D3D_DRIVER_TYPE_WARP;
            } else {
                break;
            }
        }
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(10, "D3D11CreateDevice failed", hr));
        }

        ComPtr<IDXGIDevice> dxgi_device;
        if (SUCCEEDED(device_.As(&dxgi_device))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC ad{};
                if (SUCCEEDED(adapter->GetDesc(&ad))) {
                    adapter_name_ = wide_to_utf8(ad.Description);
                }
                adapter->GetParent(IID_PPV_ARGS(&factory_));
            }
        }
        return core::Result<void>::ok();
    }

    [[nodiscard]] core::Result<void> create_swapchain(HWND hwnd) {
        if (!factory_) {
            return core::Result<void>::err(core::Error{11, "no DXGI factory"});
        }
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory_.As(&factory5))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
                tearing_supported_ = allow != FALSE;
            }
        }
        swapchain_flags_ = tearing_supported_ ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0U;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(std::max(size_.w, 1));
        sd.Height = static_cast<UINT>(std::max(size_.h, 1));
        sd.Format = kTargetFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // flip model
        sd.Flags = swapchain_flags_;
        const HRESULT hr =
            factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swapchain_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(12, "CreateSwapChainForHwnd failed", hr));
        }
        factory_->MakeWindowAssociation(hwnd,
                                        DXGI_MWA_NO_ALT_ENTER);  // Fullscreen is handled by the window layer.
        return core::Result<void>::ok();
    }

    [[nodiscard]] core::Result<void> create_target() {
        if (size_.w <= 0 || size_.h <= 0) {
            return core::Result<void>::ok();
        }
        HRESULT hr = S_OK;
        if (windowed_) {
            hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&target_));
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(size_.w);
            td.Height = static_cast<UINT>(size_.h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = kTargetFormat;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device_->CreateTexture2D(&td, nullptr, &target_);
        }
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(13, "render target creation failed", hr));
        }
        hr = device_->CreateRenderTargetView(target_.Get(), nullptr, &rtv_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(14, "CreateRenderTargetView failed", hr));
        }

        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = static_cast<UINT>(size_.w);
        dd.Height = static_cast<UINT>(size_.h);
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = kDepthFormat;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = device_->CreateTexture2D(&dd, nullptr, &depth_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(25, "depth buffer creation failed", hr));
        }
        hr = device_->CreateDepthStencilView(depth_.Get(), nullptr, &dsv_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(26, "CreateDepthStencilView failed", hr));
        }
        return core::Result<void>::ok();
    }

    [[nodiscard]] core::Result<void> create_pipeline_objects() {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(sizeof(Vertex) * 6 * kMaxQuadsPerBatch);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateBuffer(&bd, nullptr, &vertex_buffer_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(15, "vertex buffer creation failed", hr));
        }

        bd.ByteWidth = sizeof(FrameConstants);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&bd, nullptr, &constants_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(16, "constant buffer creation failed", hr));
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
        hr = device_->CreateBlendState(&bld, &blend_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(17, "CreateBlendState failed", hr));
        }

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = device_->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(18, "CreateRasterizerState failed", hr));
        }

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        hr = device_->CreateDepthStencilState(&dd, &depth_off_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(19, "CreateDepthStencilState failed", hr));
        }
        return core::Result<void>::ok();
    }

    // ---------------------------------------------------------------- shaders

    /// Builds all shader objects first and swaps them in only if everything succeeded.
    [[nodiscard]] core::Result<void> build_shaders() {
        auto source = quad_source_->load();
        if (!source) {
            return core::Result<void>::err(source.error());
        }
        auto vs_blob = compile_hlsl(source.value(), kQuadShaderFile, "vs_main", "vs_5_0");
        if (!vs_blob) {
            return core::Result<void>::err(vs_blob.error());
        }
        auto ps_blob = compile_hlsl(source.value(), kQuadShaderFile, "ps_main", "ps_5_0");
        if (!ps_blob) {
            return core::Result<void>::err(ps_blob.error());
        }

        ShaderPair next;
        HRESULT hr = device_->CreateVertexShader(
            vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), nullptr, &next.vs);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(22, "CreateVertexShader failed", hr));
        }
        hr = device_->CreatePixelShader(
            ps_blob.value()->GetBufferPointer(), ps_blob.value()->GetBufferSize(), nullptr, &next.ps);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(23, "CreatePixelShader failed", hr));
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = device_->CreateInputLayout(
            elements, 2, vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), &next.layout);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(24, "CreateInputLayout failed", hr));
        }

        shaders_ = std::move(next);
        ++shader_generation_;
        return core::Result<void>::ok();
    }

    void bind_pipeline() {
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vb = vertex_buffer_.Get();
        ID3D11Buffer* cb = constants_.Get();
        context_->IASetInputLayout(shaders_.layout.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context_->VSSetShader(shaders_.vs.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, &cb);
        context_->PSSetShader(shaders_.ps.Get(), nullptr, 0);
        const float blend_factor[4] = {0, 0, 0, 0};
        context_->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFFu);
        context_->OMSetDepthStencilState(depth_off_.Get(), 0);
        context_->RSSetState(raster_.Get());
    }

    // ---------------------------------------------------------------- state

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3DUserDefinedAnnotation> annotation_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> depth_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_off_;
    ShaderPair shaders_;

    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
    std::string adapter_name_;
    std::filesystem::path shader_dir_;
    std::unique_ptr<ShaderSource> quad_source_;
    bool pipeline_dirty_ = false;
    std::uint32_t shader_generation_ = 0;

    PixelSize size_{};
    UINT swapchain_flags_ = 0;
    bool windowed_ = false;
    bool vsync_ = true;
    bool tearing_supported_ = false;
    bool frame_active_ = false;
    bool device_lost_ = false;
    FrameStats stats_{};
};

}  // namespace

core::Result<std::unique_ptr<IRenderer>> create_renderer(const RendererDesc& desc) {
    auto renderer = std::make_unique<D3D11Renderer>();
    if (auto r = renderer->init(desc); !r) {
        return core::Result<std::unique_ptr<IRenderer>>::err(r.error());
    }
    return core::Result<std::unique_ptr<IRenderer>>::ok(std::move(renderer));
}

}  // namespace pat::engine
