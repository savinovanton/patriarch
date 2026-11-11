#pragma once

namespace Rml {
class Context;
}

namespace pat::ui {

/// Makes the keyboard focus ring of the shared components (apps/spike/ui/components.rcss) show only after the
/// keyboard was used: the root element of `context` gets the class `kbd` on Tab and arrow keys and loses it
/// on a mouse press. RmlUi has no `:focus-visible`. Called by install_theme; one listener per context, it
/// deletes itself with the context.
void install_focus_visibility(Rml::Context& context);

}  // namespace pat::ui
