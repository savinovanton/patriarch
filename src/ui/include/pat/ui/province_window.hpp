#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/ui/province_snapshot.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// Holding inspector of board 01, bound to read-only data from game. Starts hidden.
class ProvinceWindow {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<ProvinceWindow>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~ProvinceWindow();
    ProvinceWindow(const ProvinceWindow&) = delete;
    ProvinceWindow& operator=(const ProvinceWindow&) = delete;

    void set_snapshot(const ProvinceSnapshot& snapshot);
    [[nodiscard]] const ProvinceSnapshot& snapshot() const;
    void relocalize();
    [[nodiscard]] const Explanation* explanation(std::string_view key) const;
    /// "overview", "income" or "buildings"; unknown names leave the active tab unchanged.
    void set_tab(std::string_view tab);
    [[nodiscard]] std::string_view tab() const;
    void set_close_handler(std::function<void()> handler);
    void set_manage_handler(std::function<void()> handler);
    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);
    [[nodiscard]] bool visible() const;

private:
    struct Impl;
    explicit ProvinceWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
