#include "pat/engine/ui.hpp"

#include "pat/core/log.hpp"
#include "pat/core/profile.hpp"
#include "pat/engine/window.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace pat::engine {

namespace {

constexpr const char* kLogCat = "ui";

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool g_ui_alive = false;  // RmlUi has process-global state: one UiContext at a time

// ------------------------------------------------------------------ key and modifier mapping

Rml::Input::KeyIdentifier map_key(int scancode) {
    using namespace Rml::Input;
    // USB HID usage ids (SDL scancodes): 4-29 letters, 30-39 digits 1..9,0, 58-69 F1-F12.
    if (scancode >= 4 && scancode <= 29) {
        return static_cast<KeyIdentifier>(KI_A + (scancode - 4));
    }
    if (scancode >= 30 && scancode <= 38) {
        return static_cast<KeyIdentifier>(KI_1 + (scancode - 30));
    }
    if (scancode == 39) {
        return KI_0;
    }
    if (scancode >= 58 && scancode <= 69) {
        return static_cast<KeyIdentifier>(KI_F1 + (scancode - 58));
    }
    switch (scancode) {
        case 40:
            return KI_RETURN;
        case 41:
            return KI_ESCAPE;
        case 42:
            return KI_BACK;
        case 43:
            return KI_TAB;
        case 44:
            return KI_SPACE;
        case 45:
            return KI_OEM_MINUS;
        case 46:
            return KI_OEM_PLUS;
        case 54:
            return KI_OEM_COMMA;
        case 55:
            return KI_OEM_PERIOD;
        case 56:
            return KI_OEM_2;
        case 73:
            return KI_INSERT;
        case 74:
            return KI_HOME;
        case 75:
            return KI_PRIOR;
        case 76:
            return KI_DELETE;
        case 77:
            return KI_END;
        case 78:
            return KI_NEXT;
        case 79:
            return KI_RIGHT;
        case 80:
            return KI_LEFT;
        case 81:
            return KI_DOWN;
        case 82:
            return KI_UP;
        case 88:
            return KI_NUMPADENTER;
        default:
            return KI_UNKNOWN;
    }
}

int map_modifiers(std::uint16_t mods) {
    int out = 0;
    if ((mods & event::mod::ctrl) != 0) {
        out |= Rml::Input::KM_CTRL;
    }
    if ((mods & event::mod::shift) != 0) {
        out |= Rml::Input::KM_SHIFT;
    }
    if ((mods & event::mod::alt) != 0) {
        out |= Rml::Input::KM_ALT;
    }
    if ((mods & event::mod::gui) != 0) {
        out |= Rml::Input::KM_META;
    }
    return out;
}

// ------------------------------------------------------------------ RmlUi interfaces

class UiSystemInterface final : public Rml::SystemInterface {
public:
    explicit UiSystemInterface(Window* window) : window_(window), start_(std::chrono::steady_clock::now()) {}

    ~UiSystemInterface() override {
        for (auto& [name, cursor] : cursors_) {
            if (cursor != nullptr) {
                SDL_DestroyCursor(cursor);
            }
        }
    }

    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
            case Rml::Log::LT_ERROR:
            case Rml::Log::LT_ASSERT:
                PAT_LOG_ERROR(kLogCat, "{}", message);
                break;
            case Rml::Log::LT_WARNING:
                PAT_LOG_WARN(kLogCat, "{}", message);
                break;
            case Rml::Log::LT_INFO:
                PAT_LOG_INFO(kLogCat, "{}", message);
                break;
            default:
                PAT_LOG_DEBUG(kLogCat, "{}", message);
                break;
        }
        return type != Rml::Log::LT_ASSERT;
    }

    void SetMouseCursor(const Rml::String& cursor_name) override {
        if (window_ == nullptr) {
            return;
        }
        static const std::map<std::string, SDL_SystemCursor> kNames = {
            {"arrow", SDL_SYSTEM_CURSOR_DEFAULT},
            {"move", SDL_SYSTEM_CURSOR_MOVE},
            {"pointer", SDL_SYSTEM_CURSOR_POINTER},
            {"resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE},
            {"cross", SDL_SYSTEM_CURSOR_CROSSHAIR},
            {"text", SDL_SYSTEM_CURSOR_TEXT},
            {"unavailable", SDL_SYSTEM_CURSOR_NOT_ALLOWED}};
        const auto it = kNames.find(cursor_name.empty() ? "arrow" : cursor_name);
        const SDL_SystemCursor id = it != kNames.end() ? it->second : SDL_SYSTEM_CURSOR_DEFAULT;
        SDL_Cursor*& cursor = cursors_[id];
        if (cursor == nullptr) {
            cursor = SDL_CreateSystemCursor(id);
        }
        if (cursor != nullptr) {
            SDL_SetCursor(cursor);
        }
    }

    void SetClipboardText(const Rml::String& text) override {
        if (window_ != nullptr) {
            SDL_SetClipboardText(text.c_str());
        }
    }

    void GetClipboardText(Rml::String& text) override {
        if (window_ == nullptr) {
            return;
        }
        char* clip = SDL_GetClipboardText();
        if (clip != nullptr) {
            text = clip;
            SDL_free(clip);
        }
    }

    void ActivateKeyboard(Rml::Vector2f, float) override {
        if (window_ != nullptr) {
            window_->set_text_input(true);
        }
    }

    void DeactivateKeyboard() override {
        if (window_ != nullptr) {
            window_->set_text_input(false);
        }
    }

private:
    Window* window_;
    std::chrono::steady_clock::time_point start_;
    std::map<SDL_SystemCursor, SDL_Cursor*> cursors_;
};

/// File access through the C stdio API with UTF-8 paths (the default interface breaks on non-ASCII
/// paths on Windows).
class UiFileInterface final : public Rml::FileInterface {
public:
    void set_provider(UiContext::FileProvider provider) { provider_ = std::move(provider); }

    Rml::FileHandle Open(const Rml::String& path) override {
        const std::filesystem::path p = from_utf8(path);
        if (provider_) {
            if (std::optional<std::string> content = provider_(p)) {
                auto* handle = new Handle;
                handle->memory = std::move(*content);
                handle->in_memory = true;
                return reinterpret_cast<Rml::FileHandle>(handle);
            }
        }
#ifdef _WIN32
        std::FILE* f = _wfopen(p.c_str(), L"rb");
#else
        std::FILE* f = std::fopen(p.c_str(), "rb");
#endif
        if (f == nullptr) {
            return 0;
        }
        auto* handle = new Handle;
        handle->file = f;
        return reinterpret_cast<Rml::FileHandle>(handle);
    }
    void Close(Rml::FileHandle file) override {
        auto* handle = reinterpret_cast<Handle*>(file);
        if (handle->file != nullptr) {
            std::fclose(handle->file);
        }
        delete handle;
    }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* handle = reinterpret_cast<Handle*>(file);
        if (!handle->in_memory) {
            return std::fread(buffer, 1, size, handle->file);
        }
        const size_t count = std::min(size, handle->memory.size() - handle->position);
        std::copy_n(handle->memory.data() + handle->position, count, static_cast<char*>(buffer));
        handle->position += count;
        return count;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* handle = reinterpret_cast<Handle*>(file);
        if (!handle->in_memory) {
            return std::fseek(handle->file, offset, origin) == 0;
        }
        long base = 0;
        if (origin == SEEK_CUR) {
            base = static_cast<long>(handle->position);
        } else if (origin == SEEK_END) {
            base = static_cast<long>(handle->memory.size());
        }
        const long target = base + offset;
        if (target < 0 || target > static_cast<long>(handle->memory.size())) {
            return false;
        }
        handle->position = static_cast<size_t>(target);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override {
        auto* handle = reinterpret_cast<Handle*>(file);
        return handle->in_memory ? handle->position : static_cast<size_t>(std::ftell(handle->file));
    }

private:
    /// An open file: a C stdio stream, or text produced by the provider.
    struct Handle {
        std::FILE* file = nullptr;
        bool in_memory = false;
        std::string memory;
        size_t position = 0;
    };
    UiContext::FileProvider provider_;
};

/// Documents and the files they depend on (the .rml itself and every .rcss/.rml in its directory).
struct WatchedDocument {
    std::filesystem::path path;
    std::map<std::filesystem::path, std::filesystem::file_time_type> stamps;
    Rml::ElementDocument* document = nullptr;
};

std::map<std::filesystem::path, std::filesystem::file_time_type>
scan_directory(const std::filesystem::path& file) {
    std::map<std::filesystem::path, std::filesystem::file_time_type> stamps;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
        const auto ext = entry.path().extension();
        if (ext == ".rml" || ext == ".rcss") {
            const auto t = std::filesystem::last_write_time(entry.path(), ec);
            if (!ec) {
                stamps[entry.path()] = t;
            }
        }
    }
    return stamps;
}

}  // namespace

struct UiContext::Impl {
    UiSystemInterface system;
    UiFileInterface files;
    std::unique_ptr<Rml::RenderInterface> render;
    Rml::Context* context = nullptr;
    std::vector<WatchedDocument> documents;
    float scale = 1.0f;
    bool initialised = false;

    explicit Impl(Window* window) : system(window) {}

    ~Impl() {
        if (context != nullptr) {
            documents.clear();
            Rml::RemoveContext("main");
            context = nullptr;
        }
        if (initialised) {
            Rml::Shutdown();  // releases textures and geometry through `render`, which is still alive here
            g_ui_alive = false;
        }
    }
};

UiContext::UiContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
UiContext::~UiContext() = default;

std::optional<std::string> UiContext::read_text_file(const std::filesystem::path& path) {
    const Rml::FileHandle handle = impl_->files.Open(to_utf8(path));
    if (handle == 0) {
        return std::nullopt;
    }
    std::string text;
    char buffer[4096];
    for (size_t count = impl_->files.Read(buffer, sizeof(buffer), handle); count > 0;
         count = impl_->files.Read(buffer, sizeof(buffer), handle)) {
        text.append(buffer, count);
    }
    impl_->files.Close(handle);
    return text;
}

void UiContext::set_file_provider(FileProvider provider) {
    impl_->files.set_provider(std::move(provider));
}

core::Result<std::unique_ptr<UiContext>>
UiContext::create(IRenderer& renderer, Window* window, const UiDesc& desc) {
    using R = core::Result<std::unique_ptr<UiContext>>;
    if (g_ui_alive) {
        return R::err(core::Error{80, "only one UiContext can exist at a time"});
    }
    auto impl = std::make_unique<Impl>(window);

    auto render = renderer.create_ui_render_interface();
    if (!render) {
        return R::err(render.error());
    }
    impl->render = std::move(render).value();

    Rml::SetSystemInterface(&impl->system);
    Rml::SetFileInterface(&impl->files);
    Rml::SetRenderInterface(impl->render.get());
    if (!Rml::Initialise()) {
        return R::err(core::Error{81, "RmlUi initialisation failed"});
    }
    impl->initialised = true;
    g_ui_alive = true;

    bool first = true;
    for (const auto& font : desc.font_files) {
        if (!Rml::LoadFontFace(to_utf8(font), /*fallback_face=*/first)) {
            PAT_LOG_WARN(kLogCat, "cannot load font '{}'", font.generic_string());
        } else {
            first = false;
        }
    }

    impl->context = Rml::CreateContext("main", Rml::Vector2i(desc.size.w, desc.size.h));
    if (impl->context == nullptr) {
        return R::err(core::Error{82, "cannot create the UI context"});
    }
    impl->scale = desc.ui_scale;
    impl->context->SetDensityIndependentPixelRatio(desc.ui_scale);
    return R::ok(std::unique_ptr<UiContext>(new UiContext(std::move(impl))));
}

bool UiContext::handle_event(const Event& e) {
    Rml::Context* ctx = impl_->context;
    // RmlUi returns true when the event was NOT consumed by the UI.
    if (const auto* move = std::get_if<event::MouseMove>(&e)) {
        return !ctx->ProcessMouseMove(static_cast<int>(move->x), static_cast<int>(move->y), 0);
    }
    if (const auto* button = std::get_if<event::MouseButton>(&e)) {
        // SDL: 1 left, 2 middle, 3 right. RmlUi: 0 left, 1 right, 2 middle.
        const int index = button->button == 1 ? 0 : (button->button == 3 ? 1 : (button->button == 2 ? 2 : 3));
        return !(button->down ? ctx->ProcessMouseButtonDown(index, 0) : ctx->ProcessMouseButtonUp(index, 0));
    }
    if (const auto* wheel = std::get_if<event::MouseWheel>(&e)) {
        // SDL: positive = away from the user (scroll up); RmlUi: positive = scroll down.
        return !ctx->ProcessMouseWheel(Rml::Vector2f(-wheel->x, -wheel->y), 0);
    }
    if (const auto* key = std::get_if<event::Key>(&e)) {
        const auto id = map_key(key->scancode);
        const int mods = map_modifiers(key->mods);
        return !(key->down ? ctx->ProcessKeyDown(id, mods) : ctx->ProcessKeyUp(id, mods));
    }
    if (const auto* text = std::get_if<event::TextInput>(&e)) {
        return !ctx->ProcessTextInput(Rml::String(text->utf8));
    }
    if (const auto* resized = std::get_if<event::Resized>(&e)) {
        ctx->SetDimensions(Rml::Vector2i(resized->pixel_w, resized->pixel_h));
    }
    return false;
}

void UiContext::resize(PixelSize pixels, float ui_scale) {
    impl_->scale = ui_scale;
    impl_->context->SetDimensions(Rml::Vector2i(pixels.w, pixels.h));
    impl_->context->SetDensityIndependentPixelRatio(ui_scale);
}

Rml::ElementDocument* UiContext::load_document(const std::filesystem::path& path) {
    Rml::ElementDocument* doc = impl_->context->LoadDocument(to_utf8(path));
    if (doc == nullptr) {
        return nullptr;
    }
    doc->Show();
    impl_->documents.push_back({path, scan_directory(path), doc});
    return doc;
}

bool UiContext::reload_if_changed() {
    bool reloaded = false;
    for (WatchedDocument& w : impl_->documents) {
        const auto now = scan_directory(w.path);
        if (now == w.stamps) {
            continue;
        }
        PAT_LOG_INFO(kLogCat, "UI files changed, reloading '{}'", w.path.generic_string());
        w.stamps = now;
        Rml::Factory::ClearStyleSheetCache();
        Rml::Factory::ClearTemplateCache();
        const bool visible = w.document != nullptr && w.document->IsVisible();
        if (w.document != nullptr) {
            w.document->Close();
        }
        w.document = impl_->context->LoadDocument(to_utf8(w.path));
        if (w.document != nullptr && visible) {
            w.document->Show();
        }
        reloaded = true;
    }
    return reloaded;
}

void UiContext::update() {
    PAT_PROFILE_SCOPE("ui.update");
    impl_->context->Update();
}

void UiContext::render() {
    PAT_PROFILE_SCOPE("ui.render");
    impl_->context->Render();
}

bool UiContext::mouse_over_ui() const {
    return impl_->context->IsMouseInteracting();
}

Rml::Context* UiContext::context() const {
    return impl_->context;
}

float UiContext::ui_scale() const {
    return impl_->scale;
}

}  // namespace pat::engine
