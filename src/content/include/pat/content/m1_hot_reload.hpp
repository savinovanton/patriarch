#pragma once

#include "pat/content/m1_scenario.hpp"
#include "pat/data/content_source.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pat::content {

/// Dev-only refresh of definitions and localisation. The source and the validated map must outlive
/// this object. Poll and register subscribers on the presentation thread.
class M1HotReload {
public:
    using Snapshot = std::shared_ptr<const M1Definitions>;
    using Subscriber = std::function<void(Snapshot)>;

    M1HotReload(const data::ContentSource& source, const mapdata::MapData& map)
        : source_(source), map_(map) {}

    /// Register before polling. Callbacks run synchronously after publication and must not re-enter
    /// subscribe() or poll(). A callback may retain its immutable snapshot.
    void subscribe(Subscriber subscriber);

    /// True when a new valid snapshot is published, false when the files are unchanged and valid.
    /// Errors preserve the current snapshot and are cached for unchanged invalid file contents.
    [[nodiscard]] core::Result<bool> poll();
    [[nodiscard]] Snapshot current() const { return current_; }

private:
    using Files = std::vector<std::pair<std::string, std::string>>;

    const data::ContentSource& source_;
    const mapdata::MapData& map_;
    std::optional<Files> last_attempt_;
    std::optional<core::Error> last_error_;
    Snapshot current_;
    std::vector<Subscriber> subscribers_;
};

}  // namespace pat::content
