#include "pat/ui/focus_visibility.hpp"

#include <RmlUi/Core.h>

namespace pat::ui {

namespace {

constexpr const char* kKeyboardClass = "kbd";

/// Sets the class `kbd` on the root element of the context after a Tab/arrow key and clears it on a mouse
/// press: the stylesheet draws the focus ring only under `.kbd`, so a mouse click does not leave a gold ring.
class FocusVisibility final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* root = root_;
        if (root == nullptr) {
            return;
        }
        if (event.GetId() == Rml::EventId::Mousedown) {
            root->SetClass(kKeyboardClass, false);
            return;
        }
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", static_cast<int>(Rml::Input::KI_UNKNOWN)));
        if (key == Rml::Input::KI_TAB || key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT ||
            key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
            root->SetClass(kKeyboardClass, true);
        }
    }

    // Attached once per event type: the listener deletes itself when the last registration goes away.
    void OnAttach(Rml::Element* element) override {
        root_ = element;
        ++attached_;
    }
    void OnDetach(Rml::Element*) override {
        if (--attached_ == 0) {
            root_ = nullptr;
            delete this;
        }
    }

private:
    Rml::Element* root_ = nullptr;
    int attached_ = 0;
};

}  // namespace

void install_focus_visibility(Rml::Context& context) {
    Rml::Element* root = context.GetRootElement();
    if (root == nullptr) {
        return;
    }
    auto* listener = new FocusVisibility();
    root->AddEventListener(Rml::EventId::Keydown, listener, true);
    root->AddEventListener(Rml::EventId::Mousedown, listener, true);
}

}  // namespace pat::ui
