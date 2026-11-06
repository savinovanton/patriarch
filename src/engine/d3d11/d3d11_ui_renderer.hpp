#pragma once

#include "pat/engine/renderer.hpp"

#include <RmlUi/Core/RenderInterface.h>

#include <memory>

#include "d3d11_map_view.hpp"  // D3D11Shared

namespace pat::engine {

/// Creates the RmlUi render interface that draws into the renderer's current target
/// (`shared.target_size` must stay valid and is read at draw time).
[[nodiscard]] core::Result<std::unique_ptr<Rml::RenderInterface>>
create_d3d11_ui_renderer(const D3D11Shared& shared);

}  // namespace pat::engine
