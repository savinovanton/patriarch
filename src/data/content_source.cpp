#include "pat/data/content_source.hpp"

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

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path from_utf8(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

std::string DirectorySource::name() const {
    return to_utf8(directory_);
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
            paths.push_back(to_utf8(entry.path().lexically_relative(directory_)));
        }
        it.increment(ec);
        if (ec)
            return R::err(ec.message());
    }
    return R::ok(std::move(paths));
}

core::Result<std::string, std::string> DirectorySource::read_file(std::string_view path) const {
    using R = core::Result<std::string, std::string>;
    std::ifstream in(directory_ / from_utf8(path), std::ios::binary);
    if (!in)
        return R::err("cannot open the file");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return R::err("read error");
    return R::ok(std::move(text));
}

}  // namespace pat::data
