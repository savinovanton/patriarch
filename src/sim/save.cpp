#include "pat/sim/save.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/sim/command_log.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace pat::sim {

namespace {

using core::Error;

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'S', 'V', '1'};
constexpr std::size_t kHeaderSize = 4 + 2 + 2 + 8 + 8 + 8;

std::uint64_t payload_hash(std::span<const std::uint8_t> payload) {
    core::HashStream h;
    h.bytes(payload);
    return h.value();
}

Error save_error(int code, const char* message) {
    return Error{code, message};
}

/// Upgrades an older payload to the current layout. Version 1 is the first one, so there is
/// nothing to migrate yet; this is the single place where future migrations are added.
core::Result<std::vector<std::uint8_t>, Error> migrate_to_current(std::uint16_t version,
                                                                  std::vector<std::uint8_t> payload) {
    using R = core::Result<std::vector<std::uint8_t>, Error>;
    if (version != kSaveFormatVersion) {
        return R::err(save_error(21, "no migration path for this save version"));
    }
    return R::ok(std::move(payload));
}

}  // namespace

std::vector<std::uint8_t> serialize_save(const World& world, std::span<const Command> pending_internal) {
    core::ByteWriter payload;
    world.save_state(payload);
    payload.u32(static_cast<std::uint32_t>(pending_internal.size()));
    for (const Command& c : pending_internal) {
        write_command(payload, c);
    }

    const std::vector<std::uint8_t> compressed = core::compress(payload.data());

    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kSaveFormatVersion);
    out.u16(0);  // flags
    out.u64(payload.size());
    out.u64(payload_hash(payload.data()));
    out.u64(compressed.size());
    out.bytes(compressed);
    return out.take();
}

core::Result<SaveData, Error> deserialize_save(std::span<const std::uint8_t> bytes) {
    using R = core::Result<SaveData, Error>;
    if (bytes.size() < kHeaderSize) {
        return R::err(save_error(20, "save file is too short"));
    }

    core::ByteReader header(bytes);
    const auto magic = header.bytes(kMagic.size());
    if (!std::equal(magic.begin(), magic.end(), kMagic.begin())) {
        return R::err(save_error(20, "not a Patriarch save (bad magic)"));
    }
    const std::uint16_t version = header.u16();
    if (header.u16() != 0) {  // flags: none defined yet, so any set bit means damage or a newer writer
        return R::err(save_error(20, "unsupported save flags"));
    }
    const std::uint64_t uncompressed_size = header.u64();
    const std::uint64_t expected_hash = header.u64();
    const std::uint64_t compressed_size = header.u64();

    if (version == 0 || version > kSaveFormatVersion) {
        return R::err(save_error(22,
                                 version == 0 ? "invalid save version"
                                              : "this save was created by a newer version of the game"));
    }
    if (compressed_size != header.remaining()) {
        return R::err(save_error(20, "save file size does not match its header"));
    }
    constexpr std::uint64_t kMaxPayload = 1ULL << 31;  // 2 GiB sanity limit
    if (uncompressed_size > kMaxPayload) {
        return R::err(save_error(20, "implausible save size"));
    }

    auto decompressed = core::decompress(header.bytes(static_cast<std::size_t>(compressed_size)),
                                         static_cast<std::size_t>(uncompressed_size));
    if (!decompressed.has_value()) {
        return R::err(decompressed.error());
    }
    if (payload_hash(decompressed.value()) != expected_hash) {
        return R::err(save_error(23, "save is corrupted (hash mismatch)"));
    }

    auto migrated = migrate_to_current(version, std::move(decompressed).value());
    if (!migrated.has_value()) {
        return R::err(migrated.error());
    }
    const std::vector<std::uint8_t> payload = std::move(migrated).value();

    core::ByteReader in(payload);
    auto world = World::load_state(in);
    if (!world.has_value()) {
        return R::err(world.error());
    }

    const std::uint32_t pending_count = in.u32();
    if (!in.ok() || static_cast<std::uint64_t>(pending_count) * 6 > in.remaining()) {
        return R::err(save_error(24, "invalid pending command list"));
    }
    std::vector<Command> pending;
    pending.reserve(pending_count);
    for (std::uint32_t i = 0; i < pending_count; ++i) {
        Command c;
        if (!read_command(in, c)) {
            return R::err(save_error(24, "invalid pending command"));
        }
        pending.push_back(c);
    }
    if (!in.at_end()) {
        return R::err(save_error(24, "trailing bytes after save payload"));
    }

    return R::ok(SaveData{std::move(world).value(), std::move(pending)});
}

core::Result<void, Error> write_file_atomic(const std::filesystem::path& path,
                                            std::span<const std::uint8_t> bytes) {
    using R = core::Result<void, Error>;
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return R::err(save_error(30, "cannot open the temporary file for writing"));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return R::err(save_error(31, "failed while writing the temporary file"));
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);  // atomic replace on the same volume
    if (ec) {
        std::filesystem::remove(temp, ec);
        return R::err(save_error(32, "cannot move the temporary file into place"));
    }
    return R::ok();
}

core::Result<std::vector<std::uint8_t>, Error> read_file(const std::filesystem::path& path) {
    using R = core::Result<std::vector<std::uint8_t>, Error>;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return R::err(save_error(33, "cannot open the file for reading"));
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return R::err(save_error(33, "cannot determine the file size"));
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!in) {
        return R::err(save_error(34, "failed while reading the file"));
    }
    return R::ok(std::move(bytes));
}

std::future<core::Result<void, Error>> save_async(std::shared_ptr<const World> snapshot,
                                                  std::vector<Command> pending_internal,
                                                  std::filesystem::path path) {
    return std::async(
        std::launch::async,
        [snapshot = std::move(snapshot), pending = std::move(pending_internal), path = std::move(path)] {
            const std::vector<std::uint8_t> bytes = serialize_save(*snapshot, pending);
            return write_file_atomic(path, bytes);
        });
}

}  // namespace pat::sim
