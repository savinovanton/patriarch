#include "pat/core/file_io.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace pat::core {

Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes) {
    using R = Result<void, FileIoError>;
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return R::err(FileIoError{FileIoStage::kOpen});
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return R::err(FileIoError{FileIoStage::kWrite});
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);  // atomic replace on the same volume
    if (ec) {
        std::filesystem::remove(temp, ec);
        return R::err(FileIoError{FileIoStage::kRename});
    }
    return R::ok();
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
