#include "pat/core/file_io.hpp"

#include <fstream>
#include <system_error>

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

}  // namespace pat::core
