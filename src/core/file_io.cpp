#include "pat/core/file_io.hpp"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace pat::core {

namespace {

std::error_code last_errno() {
    return std::error_code(errno, std::generic_category());
}

}  // namespace

Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes,
                                            const AtomicWriteOptions& options) {
    using R = Result<void, FileIoError>;
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        errno = 0;
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return R::err(FileIoError{FileIoStage::kOpen, last_errno()});
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            const std::error_code ec = last_errno();
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return R::err(FileIoError{FileIoStage::kWrite, ec});
        }
    }

    std::error_code ec;
    if (options.verify && !options.verify(temp)) {
        std::filesystem::remove(temp, ec);
        return R::err(FileIoError{FileIoStage::kVerify, {}});
    }

    bool backed_up = false;
    if (!options.backup_path.empty() && std::filesystem::is_regular_file(path, ec)) {
        std::filesystem::rename(path, options.backup_path, ec);  // replaces an older backup
        if (ec) {
            const std::error_code reason = ec;
            std::filesystem::remove(temp, ec);
            return R::err(FileIoError{FileIoStage::kRename, reason});
        }
        backed_up = true;
    }

    std::filesystem::rename(temp, path, ec);  // atomic replace on the same volume
    if (ec) {
        const std::error_code reason = ec;
        std::filesystem::remove(temp, ec);
        if (backed_up) {
            std::filesystem::rename(options.backup_path, path, ec);
        }
        return R::err(FileIoError{FileIoStage::kRename, reason});
    }
    return R::ok();
}

Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes) {
    return write_file_atomic(path, bytes, AtomicWriteOptions{});
}

Result<std::vector<std::uint8_t>, FileIoError> read_file_bytes(const std::filesystem::path& path) {
    using R = Result<std::vector<std::uint8_t>, FileIoError>;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return R::err(FileIoError{FileIoStage::kOpen});
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return R::err(FileIoError{FileIoStage::kSize});
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!in) {
        return R::err(FileIoError{FileIoStage::kRead});
    }
    return R::ok(std::move(bytes));
}

Result<std::string, FileIoError> read_file_text(const std::filesystem::path& path) {
    using R = Result<std::string, FileIoError>;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return R::err(FileIoError{FileIoStage::kOpen});
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return R::err(FileIoError{FileIoStage::kRead});
    }
    return R::ok(std::move(text));
}

}  // namespace pat::core
