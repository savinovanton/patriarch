#pragma once

#include "pat/engine/renderer.hpp"

#include <memory>

#include "d3d11_map_view.hpp"  // D3D11Shared

namespace pat::engine {

/// Creates the SDF text renderer (uploads the atlas); draws into the renderer's current target.
[[nodiscard]] core::Result<std::unique_ptr<ITextRenderer>>
create_d3d11_text_renderer(const D3D11Shared& shared, const SdfAtlas& atlas);

}  // namespace pat::engine
