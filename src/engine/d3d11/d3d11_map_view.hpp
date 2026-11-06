#pragma once

#include "pat/engine/renderer.hpp"

#include <filesystem>
#include <memory>

#include "d3d11_common.hpp"

namespace pat::engine {

/// What the map view needs from the renderer that created it.
struct D3D11Shared {
    d3d11::ComPtr<ID3D11Device> device;
    d3d11::ComPtr<ID3D11DeviceContext> context;
    std::filesystem::path shader_dir;
    /// Set to true after the map view changed pipeline state, so the renderer rebinds its own.
    /// Points into the renderer, which therefore must outlive the view.
    bool* invalidate_pipeline = nullptr;
    /// Current render target size in pixels (owned by the renderer, read at draw time).
    const PixelSize* target_size = nullptr;
};

[[nodiscard]] core::Result<std::unique_ptr<IMapView>>
create_d3d11_map_view(const D3D11Shared& shared, const mapdata::MapData& map, const MapViewDesc& desc);

}  // namespace pat::engine
