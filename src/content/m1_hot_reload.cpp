#include "pat/content/m1_hot_reload.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace pat::content {

void M1HotReload::subscribe(Subscriber subscriber) {
    if (subscriber) {
        subscribers_.push_back(std::move(subscriber));
    }
}

core::Result<bool> M1HotReload::poll() {
    using Result = core::Result<bool>;
    auto paths = source_.list_files();
    if (!paths) {
        return Result::err({1, std::format("{}: {}", source_.name(), paths.error())});
    }
    std::sort(paths.value().begin(), paths.value().end());
    Files files;
    for (const auto& path : paths.value()) {
        if (!path.ends_with(".pd")) {
            continue;
        }
        auto text = source_.read_file(path);
        if (!text) {
            return Result::err({1, std::format("{}/{}: {}", source_.name(), path, text.error())});
        }
        files.emplace_back(path, std::move(text).value());
    }
    if (last_attempt_ && files == *last_attempt_) {
        return last_error_ ? Result::err(*last_error_) : Result::ok(false);
    }

    // Validate the captured bytes, so the parser and localiser see the same files that were checked
    // for changes. An editor may already have written another revision to disk.
    data::MemorySource snapshot(source_.name());
    for (const auto& [path, text] : files) {
        snapshot.add(path, text);
    }
    auto candidate = load_m1_definitions(snapshot, map_);
    last_attempt_ = std::move(files);
    if (!candidate) {
        last_error_ = candidate.error();
        return Result::err(*last_error_);
    }

    last_error_.reset();
    current_ = std::make_shared<const M1Definitions>(std::move(candidate).value());
    for (const auto& subscriber : subscribers_) {
        subscriber(current_);
    }
    return Result::ok(true);
}

}  // namespace pat::content
