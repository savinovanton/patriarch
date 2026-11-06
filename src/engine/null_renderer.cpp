#include "pat/engine/renderer.hpp"

namespace pat::engine {

// Non-Windows builds have no graphics backend yet (Direct3D 11 only, ADR-0001).
core::Result<std::unique_ptr<IRenderer>> create_renderer(const RendererDesc&) {
    return core::Result<std::unique_ptr<IRenderer>>::err(
        core::Error{1, "no renderer backend on this platform"});
}

}  // namespace pat::engine
