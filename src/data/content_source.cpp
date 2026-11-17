#include "pat/data/content_source.hpp"

#include "pat/core/file_io.hpp"
#include "pat/core/path_utf8.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace pat::data {

// ---- MemorySource ----------------------------------------------------------------------------

MemorySource& MemorySource::add(std::string path, std::string text) {
    for (File& f : files_) {
        if (f.path == path) {
            f.text = std::move(text);
            f.readable = true;
            return *this;
        }
    }
    files_.push_back(File{std::move(path), std::move(text), true, {}});
    return *this;
}

MemorySource& MemorySource::add_unreadable(std::string path, std::string reason) {
    for (File& f : files_) {
        if (f.path == path) {
            f.readable = false;
            f.reason = std::move(reason);
            return *this;
        }
    }
    files_.push_back(File{std::move(path), {}, false, std::move(reason)});
    return *this;
}

MemorySource& MemorySource::fail_listing(std::string reason) {
    listing_fails_ = true;
    listing_reason_ = std::move(reason);
    return *this;
}

core::Result<std::vector<std::string>, std::string> MemorySource::list_files() const {
    using R = core::Result<std::vector<std::string>, std::string>;
    if (listing_fails_)
        return R::err(listing_reason_);
    std::vector<std::string> paths;
    paths.reserve(files_.size());
    for (const File& f : files_)
        paths.push_back(f.path);
    return R::ok(std::move(paths));
}

core::Result<std::string, std::string> MemorySource::read_file(std::string_view path) const {
    using R = core::Result<std::string, std::string>;
    for (const File& f : files_) {
        if (f.path == path)
            return f.readable ? R::ok(f.text) : R::err(f.reason);
    }
    return R::err("no such file");
}

// ---- DirectorySource -------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

}  // namespace

std::string DirectorySource::name() const {
    return core::path_to_utf8(directory_);
}

core::Result<std::vector<std::string>, std::string> DirectorySource::list_files() const {
    using R = core::Result<std::vector<std::string>, std::string>;
    std::error_code ec;
    const bool is_dir = fs::is_directory(directory_, ec);
    if (ec)
        return R::err(ec.message());
    if (!is_dir)
        return R::err("not a directory");

    std::vector<std::string> paths;
    fs::recursive_directory_iterator it(directory_, fs::directory_options::none, ec);
    if (ec)
        return R::err(ec.message());
    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry& entry = *it;
        std::error_code type_ec;
        if (entry.is_regular_file(type_ec) && !type_ec) {
            paths.push_back(core::path_to_utf8(entry.path().lexically_relative(directory_)));
        }
        it.increment(ec);
        if (ec)
            return R::err(ec.message());
    }
    return R::ok(std::move(paths));
}

core::Result<std::string, std::string> DirectorySource::read_file(std::string_view path) const {
    using R = core::Result<std::string, std::string>;
    auto text = core::read_file_text(directory_ / core::path_from_utf8(path));
    if (!text) {
        return R::err(text.error().stage == core::FileIoStage::kOpen ? "cannot open the file" : "read error");
    }
    return R::ok(std::move(text).value());
}

}  // namespace pat::data
