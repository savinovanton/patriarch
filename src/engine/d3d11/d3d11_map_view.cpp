#include "d3d11_map_view.hpp"

#include "pat/core/log.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

// Defined in the generated embedded_shaders.cpp (the contents of shaders/terrain.hlsl).
namespace pat::engine::embedded {
extern const char* const kTerrainHlsl;
}

namespace pat::engine {

namespace {

using namespace d3d11;

constexpr const char* kTerrainShaderFile = "terrain.hlsl";
constexpr std::size_t kMaxNodes = 4096;
constexpr std::size_t kMaxLevels = 16;
constexpr std::uint32_t kPaletteWidth = 256;

// Must match FrameConstants in terrain.hlsl (all members are float4-aligned).
struct FrameCB {
    float view_proj[16];
    float eye_zoom[4];
    float map_info[4];
    float select_info[4];
    float lod_ranges[kMaxLevels];
    float patch_info[4];
    float sun_dir[4];     // xyz = unit vector towards the sun (x east, y south, z up)
    float light_info[4];  // x = shadow strength, y = shadow steps, z = haze, w = longest shadow in texels
};
static_assert(sizeof(FrameCB) % 16 == 0, "constant buffers are 16-byte aligned");

struct MaterialCB {
    float colors[16][4];
};

// Surface colours of the Material enum (mapdata/map_data.hpp), linear-ish painterly values.
constexpr std::array<std::array<float, 3>, mapdata::kMaterialCount> kMaterialColors = {{
    {0.04f, 0.12f, 0.26f},  // DeepWater
    {0.17f, 0.36f, 0.50f},  // ShallowWater
    {0.80f, 0.74f, 0.55f},  // Beach
    {0.48f, 0.60f, 0.30f},  // Plains
    {0.68f, 0.64f, 0.36f},  // Steppe
    {0.17f, 0.36f, 0.19f},  // Forest
    {0.50f, 0.44f, 0.30f},  // Hills
    {0.50f, 0.47f, 0.44f},  // Rock
    {0.94f, 0.95f, 0.98f},  // Snow
    {0.62f, 0.66f, 0.58f},  // Tundra
}};

std::uint32_t hash_id(std::uint32_t id, std::uint32_t salt) {
    std::uint32_t h = id * 0x9E3779B1U + salt * 0x85EBCA6BU;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    h *= 0x297A2D39U;
    h ^= h >> 15U;
    return h;
}

std::uint32_t pack_rgba(float r, float g, float b) {
    const auto q = [](float c) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    return q(r) | (q(g) << 8U) | (q(b) << 16U) | (255U << 24U);  // R8G8B8A8_UNORM, little endian
}

/// Palette for one map mode: RGBA8 texels indexed by province id (id % 256, id / 256).
std::vector<std::uint32_t> build_palette(const mapdata::MapData& map, MapMode mode, std::uint32_t rows) {
    std::vector<std::uint32_t> pal(static_cast<std::size_t>(kPaletteWidth) * rows,
                                   pack_rgba(0.5f, 0.5f, 0.5f));
    for (const mapdata::Province& p : map.province_list) {
        std::uint32_t c = 0;
        switch (mode) {
            case MapMode::Owners: {
                const std::uint32_t rgb = p.color_rgb;
                c = pack_rgba(static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0f,
                              static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0f,
                              static_cast<float>(rgb & 0xFFU) / 255.0f);
                break;
            }
            case MapMode::Mood: {
                // Stand-in for a simulation value in [0, 1]: red (bad) through yellow to green (good).
                const float v = static_cast<float>(hash_id(p.id, 7U) % 1000U) / 999.0f;
                const float r = v < 0.5f ? 0.85f : 0.85f - (v - 0.5f) * 1.2f;
                const float g = v < 0.5f ? 0.25f + v * 1.1f : 0.80f;
                c = pack_rgba(r, g, 0.22f);
                break;
            }
            case MapMode::Random: {
                const std::uint32_t h = hash_id(p.id, 99U);
                c = pack_rgba(static_cast<float>(h & 0xFFU) / 255.0f,
                              static_cast<float>((h >> 8U) & 0xFFU) / 255.0f,
                              static_cast<float>((h >> 16U) & 0xFFU) / 255.0f);
                break;
            }
            default:
                c = pack_rgba(0.5f, 0.5f, 0.5f);
                break;
        }
        pal[p.id] = c;
    }
    return pal;
}

class D3D11MapView final : public IMapView {
public:
    D3D11MapView(const D3D11Shared& shared, const mapdata::MapData& map, const MapViewDesc& desc)
        : shared_(shared), map_(&map), desc_(desc),
          geometry_(TerrainGeometry::from_map(map, desc.relief_fraction)),
          source_(shared.shader_dir, kTerrainShaderFile, embedded::kTerrainHlsl) {}

    [[nodiscard]] core::Result<void> init() {
        if (auto r = create_textures(); !r) {
            return r;
        }
        if (auto r = create_mesh(); !r) {
            return r;
        }
        if (auto r = create_state(); !r) {
            return r;
        }
        if (auto r = build_shaders(); !r) {
            return r;
        }
        return core::Result<void>::ok();
    }

    void set_mode(MapMode mode) override {
        if (mode == mode_ && palette_valid_) {
            return;
        }
        mode_ = mode;
        upload_palette(palettes_[static_cast<std::size_t>(mode)]);
        palette_valid_ = true;
    }

    void set_palette(MapMode mode, std::span<const std::uint32_t> colors) override {
        const auto idx = static_cast<std::size_t>(mode);
        if (idx >= palettes_.size()) {
            return;
        }
        std::vector<std::uint32_t>& pal = palettes_[idx];
        const std::size_t n = std::min(colors.size(), pal.size());
        std::copy_n(colors.begin(), n, pal.begin());
        if (mode == mode_ && palette_valid_) {
            upload_palette(pal);
        }
    }
    [[nodiscard]] MapMode mode() const override { return mode_; }

    void draw(const MapFrame& frame) override {
        auto* ctx = shared_.context.Get();

        // Select and upload the visible nodes.
        const Frustum frustum = Frustum::from_view_proj(frame.view_proj);
        select_terrain_nodes(geometry_, frame.eye, &frustum, desc_.lod, nodes_);
        if (nodes_.size() > kMaxNodes) {
            nodes_.resize(kMaxNodes);
        }
        last_nodes_ = nodes_.size();
        if (nodes_.empty()) {
            return;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(instance_vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        auto* inst = static_cast<float*>(mapped.pData);
        for (const TerrainNode& n : nodes_) {
            inst[0] = n.x;
            inst[1] = n.y;
            inst[2] = n.size;
            inst[3] = static_cast<float>(n.level);
            inst += 4;
        }
        ctx->Unmap(instance_vb_.Get(), 0);

        FrameCB cb{};
        std::memcpy(cb.view_proj, frame.view_proj.data(), sizeof(cb.view_proj));
        cb.eye_zoom[0] = frame.eye.x;
        cb.eye_zoom[1] = frame.eye.y;
        cb.eye_zoom[2] = frame.eye.z;
        cb.eye_zoom[3] = frame.zoom_fraction;
        cb.map_info[0] = geometry_.width;
        cb.map_info[1] = geometry_.height;
        cb.map_info[2] = geometry_.relief;
        cb.map_info[3] = static_cast<float>(geometry_.sea_level) / 65535.0f;
        cb.select_info[0] = static_cast<float>(frame.hovered);
        cb.select_info[1] = static_cast<float>(frame.selected);
        cb.select_info[2] = mode_ == MapMode::Terrain ? 0.0f : 1.0f;
        cb.select_info[3] = frame.time_s;
        for (std::size_t l = 0; l < kMaxLevels; ++l) {
            cb.lod_ranges[l] = l <= static_cast<std::size_t>(geometry_.max_level)
                                   ? lod_range(geometry_, static_cast<int>(l), desc_.lod)
                                   : 1.0f;
        }
        cb.patch_info[0] = static_cast<float>(geometry_.patch_cells);
        {
            constexpr float kDegToRad = 3.14159265f / 180.0f;
            const float az = frame.light.sun_azimuth_deg * kDegToRad;
            const float el = std::clamp(frame.light.sun_elevation_deg, 5.0f, 89.0f) * kDegToRad;
            cb.sun_dir[0] = std::cos(el) * std::sin(az);
            cb.sun_dir[1] = -std::cos(el) * std::cos(az);
            cb.sun_dir[2] = std::sin(el);
            cb.light_info[0] = std::clamp(frame.light.shadow_strength, 0.0f, 1.0f);
            cb.light_info[1] = static_cast<float>(std::clamp(frame.light.shadow_steps, 0, 64));
            cb.light_info[2] = std::max(frame.light.haze, 0.0f);
            // A shadow cannot be longer than the relief divided by the slope of the sun's rays.
            cb.light_info[3] =
                std::min(geometry_.relief / std::max(std::sin(el), 0.15f), 0.2f * geometry_.width);
        }
        if (SUCCEEDED(ctx->Map(frame_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(frame_cb_.Get(), 0);
        }

        // Pipeline state for the terrain pass.
        ID3D11Buffer* vbs[2] = {grid_vb_.Get(), instance_vb_.Get()};
        const UINT strides[2] = {sizeof(float) * 2, sizeof(float) * 4};
        const UINT offsets[2] = {0, 0};
        ctx->IASetInputLayout(shaders_.layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        ctx->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->VSSetShader(shaders_.vs.Get(), nullptr, 0);
        ctx->PSSetShader(shaders_.ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {frame_cb_.Get(), material_cb_.Get()};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* srvs[5] = {height_srv_.Get(),
                                             normal_srv_.Get(),
                                             province_srv_.Get(),
                                             material_srv_.Get(),
                                             palette_srv_.Get()};
        ctx->VSSetShaderResources(0, 5, srvs);
        ctx->PSSetShaderResources(0, 5, srvs);
        ID3D11SamplerState* samplers[1] = {sampler_.Get()};
        ctx->VSSetSamplers(0, 1, samplers);
        ctx->PSSetSamplers(0, 1, samplers);
        const float blend_factor[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(depth_on_.Get(), 0);
        ctx->RSSetState(raster_.Get());

        ctx->DrawIndexedInstanced(index_count_, static_cast<UINT>(nodes_.size()), 0, 0, 0);

        // Leave no terrain resources bound (the quad pass and later passes expect a clean slate).
        ID3D11ShaderResourceView* none[5] = {};
        ctx->VSSetShaderResources(0, 5, none);
        ctx->PSSetShaderResources(0, 5, none);
        if (shared_.invalidate_pipeline != nullptr) {
            *shared_.invalidate_pipeline = true;
        }
    }

    [[nodiscard]] const TerrainGeometry& geometry() const override { return geometry_; }
    [[nodiscard]] std::size_t last_node_count() const override { return last_nodes_; }

    bool reload_shaders_if_changed() override {
        if (!source_.changed()) {
            return false;
        }
        PAT_LOG_INFO(kLogCat, "shader source changed, recompiling '{}'", kTerrainShaderFile);
        auto r = build_shaders();
        if (!r) {
            PAT_LOG_ERROR(
                kLogCat, "terrain shader reload failed, keeping the previous one:\n{}", r.error().message);
            return false;
        }
        return true;
    }

private:
    struct ShaderSet {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
    };

    template <class T>
    [[nodiscard]] core::Result<void> make_texture(DXGI_FORMAT format,
                                                  const void* data,
                                                  std::uint32_t bytes_per_texel,
                                                  ComPtr<ID3D11ShaderResourceView>& srv,
                                                  const char* what) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = map_->width;
        td.Height = map_->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = data;
        init.SysMemPitch = map_->width * bytes_per_texel;
        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = shared_.device->CreateTexture2D(&td, &init, &tex);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(40, what, hr));
        }
        hr = shared_.device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(41, what, hr));
        }
        return core::Result<void>::ok();
    }

    void upload_palette(const std::vector<std::uint32_t>& pal) {
        D3D11_BOX box{0, 0, 0, kPaletteWidth, palette_rows_, 1};
        shared_.context->UpdateSubresource(palette_tex_.Get(), 0, &box, pal.data(), kPaletteWidth * 4U, 0);
    }

    [[nodiscard]] core::Result<void> create_textures() {
        if (auto r = make_texture<std::uint16_t>(DXGI_FORMAT_R16_UNORM,
                                                 map_->heights.data(),
                                                 2,
                                                 height_srv_,
                                                 "height texture creation failed");
            !r) {
            return r;
        }
        if (auto r = make_texture<std::uint16_t>(DXGI_FORMAT_R16_UINT,
                                                 map_->provinces.data(),
                                                 2,
                                                 province_srv_,
                                                 "province texture creation failed");
            !r) {
            return r;
        }
        if (auto r = make_texture<std::uint8_t>(DXGI_FORMAT_R8_UINT,
                                                map_->materials.data(),
                                                1,
                                                material_srv_,
                                                "material texture creation failed");
            !r) {
            return r;
        }

        // Normals are derived data (not stored in the map file): world-space slopes of the displayed surface.
        const double metres_per_pixel = (static_cast<double>(map_->metres_per_unit_milli) / 1000.0) *
                                        65535.0 / std::max(static_cast<double>(geometry_.relief), 1e-6);
        const std::vector<std::int8_t> normals = mapdata::compute_normals(*map_, metres_per_pixel);
        if (auto r = make_texture<std::int8_t>(
                DXGI_FORMAT_R8G8_SNORM, normals.data(), 2, normal_srv_, "normal texture creation failed");
            !r) {
            return r;
        }

        // Palette: one texel per province id; one prebuilt palette per mode, swapped with UpdateSubresource.
        palette_rows_ =
            static_cast<std::uint32_t>((map_->province_list.size() + kPaletteWidth) / kPaletteWidth);
        for (std::size_t m = 0; m < static_cast<std::size_t>(MapMode::Count); ++m) {
            palettes_[m] = build_palette(*map_, static_cast<MapMode>(m), palette_rows_);
        }
        D3D11_TEXTURE2D_DESC pd{};
        pd.Width = kPaletteWidth;
        pd.Height = palette_rows_;
        pd.MipLevels = 1;
        pd.ArraySize = 1;
        pd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        pd.Usage = D3D11_USAGE_DEFAULT;
        pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = shared_.device->CreateTexture2D(&pd, nullptr, &palette_tex_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(42, "palette texture creation failed", hr));
        }
        hr = shared_.device->CreateShaderResourceView(palette_tex_.Get(), nullptr, &palette_srv_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(43, "palette view creation failed", hr));
        }
        set_mode(MapMode::Owners);
        return core::Result<void>::ok();
    }

    [[nodiscard]] core::Result<void> create_mesh() {
        const int cells = geometry_.patch_cells;
        const int verts = cells + 1;

        std::vector<float> grid;
        grid.reserve(static_cast<std::size_t>(verts) * static_cast<std::size_t>(verts) * 2);
        for (int j = 0; j < verts; ++j) {
            for (int i = 0; i < verts; ++i) {
                grid.push_back(static_cast<float>(i));
                grid.push_back(static_cast<float>(j));
            }
        }
        std::vector<std::uint16_t> indices;
        indices.reserve(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells) * 6);
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                const auto a = static_cast<std::uint16_t>(j * verts + i);
                const auto b = static_cast<std::uint16_t>(a + 1);
                const auto c = static_cast<std::uint16_t>(a + verts);
                const auto d = static_cast<std::uint16_t>(c + 1);
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        }
        index_count_ = static_cast<UINT>(indices.size());

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(grid.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = grid.data();
        HRESULT hr = shared_.device->CreateBuffer(&bd, &init, &grid_vb_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(44, "patch vertex buffer creation failed", hr));
        }
        bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = indices.data();
        hr = shared_.device->CreateBuffer(&bd, &init, &index_buffer_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(45, "patch index buffer creation failed", hr));
        }

        bd = {};
        bd.ByteWidth = static_cast<UINT>(kMaxNodes * sizeof(float) * 4);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = shared_.device->CreateBuffer(&bd, nullptr, &instance_vb_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(46, "instance buffer creation failed", hr));
        }
        return core::Result<void>::ok();
    }

    [[nodiscard]] core::Result<void> create_state() {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FrameCB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = shared_.device->CreateBuffer(&bd, nullptr, &frame_cb_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(47, "frame constant buffer creation failed", hr));
        }

        MaterialCB mc{};
        for (std::size_t i = 0; i < kMaterialColors.size(); ++i) {
            mc.colors[i][0] = kMaterialColors[i][0];
            mc.colors[i][1] = kMaterialColors[i][1];
            mc.colors[i][2] = kMaterialColors[i][2];
            mc.colors[i][3] = 1.0f;
        }
        bd = {};
        bd.ByteWidth = sizeof(MaterialCB);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = &mc;
        hr = shared_.device->CreateBuffer(&bd, &init, &material_cb_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(48, "material constant buffer creation failed", hr));
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = shared_.device->CreateSamplerState(&sd, &sampler_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(49, "sampler creation failed", hr));
        }

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = shared_.device->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(50, "rasterizer state creation failed", hr));
        }

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        hr = shared_.device->CreateDepthStencilState(&dd, &depth_on_);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(51, "depth state creation failed", hr));
        }
        return core::Result<void>::ok();
    }

    /// Compiles everything first and swaps the shaders in only if all of it succeeded.
    [[nodiscard]] core::Result<void> build_shaders() {
        auto source = source_.load();
        if (!source) {
            return core::Result<void>::err(source.error());
        }
        auto vs_blob = compile_hlsl(source.value(), kTerrainShaderFile, "vs_main", "vs_5_0");
        if (!vs_blob) {
            return core::Result<void>::err(vs_blob.error());
        }
        auto ps_blob = compile_hlsl(source.value(), kTerrainShaderFile, "ps_main", "ps_5_0");
        if (!ps_blob) {
            return core::Result<void>::err(ps_blob.error());
        }
        ShaderSet next;
        HRESULT hr = shared_.device->CreateVertexShader(
            vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), nullptr, &next.vs);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(52, "terrain vertex shader creation failed", hr));
        }
        hr = shared_.device->CreatePixelShader(
            ps_blob.value()->GetBufferPointer(), ps_blob.value()->GetBufferSize(), nullptr, &next.ps);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(53, "terrain pixel shader creation failed", hr));
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        hr = shared_.device->CreateInputLayout(
            elements, 2, vs_blob.value()->GetBufferPointer(), vs_blob.value()->GetBufferSize(), &next.layout);
        if (FAILED(hr)) {
            return core::Result<void>::err(make_error(54, "terrain input layout creation failed", hr));
        }
        shaders_ = std::move(next);
        return core::Result<void>::ok();
    }

    D3D11Shared shared_;
    const mapdata::MapData* map_;
    MapViewDesc desc_;
    TerrainGeometry geometry_;
    ShaderSource source_;

    ComPtr<ID3D11ShaderResourceView> height_srv_;
    ComPtr<ID3D11ShaderResourceView> normal_srv_;
    ComPtr<ID3D11ShaderResourceView> province_srv_;
    ComPtr<ID3D11ShaderResourceView> material_srv_;
    ComPtr<ID3D11Texture2D> palette_tex_;
    ComPtr<ID3D11ShaderResourceView> palette_srv_;
    ComPtr<ID3D11Buffer> grid_vb_;
    ComPtr<ID3D11Buffer> index_buffer_;
    ComPtr<ID3D11Buffer> instance_vb_;
    ComPtr<ID3D11Buffer> frame_cb_;
    ComPtr<ID3D11Buffer> material_cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_on_;
    ShaderSet shaders_;

    std::array<std::vector<std::uint32_t>, static_cast<std::size_t>(MapMode::Count)> palettes_;
    std::uint32_t palette_rows_ = 1;
    MapMode mode_ = MapMode::Terrain;
    bool palette_valid_ = false;
    UINT index_count_ = 0;
    std::vector<TerrainNode> nodes_;
    std::size_t last_nodes_ = 0;
};

}  // namespace

core::Result<std::unique_ptr<IMapView>>
create_d3d11_map_view(const D3D11Shared& shared, const mapdata::MapData& map, const MapViewDesc& desc) {
    if (map.pixel_count() == 0 || map.heights.size() != map.pixel_count() ||
        map.provinces.size() != map.pixel_count() || map.materials.size() != map.pixel_count() ||
        map.province_list.size() > 65535) {
        return core::Result<std::unique_ptr<IMapView>>::err(core::Error{60, "map layers are inconsistent"});
    }
    auto view = std::make_unique<D3D11MapView>(shared, map, desc);
    if (auto r = view->init(); !r) {
        return core::Result<std::unique_ptr<IMapView>>::err(r.error());
    }
    return core::Result<std::unique_ptr<IMapView>>::ok(std::move(view));
}

}  // namespace pat::engine
