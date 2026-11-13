#include "pat/sim/save.hpp"

#include "pat/core/build_info.hpp"
#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/sim/command_log.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pat::sim {

namespace {

using core::Error;

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'S', 'V', '1'};
constexpr std::uint64_t kMaxPayload = 1ULL << 31;  // 2 GiB sanity limit

// Error codes of the container (docs/dev/save-format.md section 2.5).
constexpr int kErrFormat = 20;  // layout, sizes, flags, magic
constexpr int kErrNoMigration = 21;
constexpr int kErrNewer = 22;
constexpr int kErrPayloadHash = 23;
constexpr int kErrPending = 24;
constexpr int kErrSummaryCorrupt = 25;   // structure, hash, field length or value
constexpr int kErrSummaryMissing = 26;   // a required field is absent
constexpr int kErrSummaryTooLarge = 27;  // summary_size above kMaxSaveSummary
constexpr int kErrSummarySchemaNewer = 28;
constexpr int kErrSchemaMismatch = 29;  // summary and payload disagree on the schema

// Summary keys (docs/dev/save-format.md section 2.3).
enum SummaryKey : std::uint16_t {
    kKeyGameVersion = 1,
    kKeyStateSchema = 2,
    kKeyScenario = 3,
    kKeyContentHash = 4,
    kKeyPlayerDynasty = 5,
    kKeyDynastyKey = 6,
    kKeyHeadName = 7,
    kKeyHeadAge = 8,
    kKeyDate = 9,
    kKeyTreasury = 10,
    kKeyGoalKey = 11,
    kKeyGoalProgress = 12,
    kKeyPlayTime = 13,
    kKeyKind = 14,
    kKeyName = 15,
    kKeySavedAt = 16,
    kKeyWorldHash = 17,
    kKeyTick = 18,
    kKeyIronman = 19,
    kKeyPreview = 20,
};

constexpr std::size_t kMaxShortText = 64;
constexpr std::size_t kMaxHeadName = 96;
constexpr std::size_t kMaxName = 256;
constexpr std::size_t kMaxNameChars = 64;
constexpr std::uint32_t kMaxGoalProgressPpm = 1000000;

std::uint64_t payload_hash(std::span<const std::uint8_t> payload) {
    core::HashStream h;
    h.bytes(payload);
    return h.value();
}

Error save_error(int code, const char* message) {
    return Error{code, message};
}

/// Upgrades an older payload to the current layout. Versions 1 and 2 of the container carry the same
/// payload, so there is nothing to migrate yet; this is the single place where future migrations of
/// the raw payload bytes are added.
core::Result<std::vector<std::uint8_t>, Error> migrate_to_current(std::uint16_t version,
                                                                  std::vector<std::uint8_t> payload) {
    using R = core::Result<std::vector<std::uint8_t>, Error>;
    if (version != kSaveFormatVersionLegacy && version != kSaveFormatVersion) {
        return R::err(save_error(kErrNoMigration, "no migration path for this save version"));
    }
    return R::ok(std::move(payload));
}

// ---- UTF-8 ----------------------------------------------------------------------------------------

/// Length in bytes of the valid UTF-8 character at the start of `s`, 0 if it is malformed (overlong,
/// surrogate, beyond U+10FFFF, truncated) or a NUL.
std::size_t utf8_char_length(std::string_view s) {
    if (s.empty()) {
        return 0;
    }
    const auto b0 = static_cast<std::uint8_t>(s[0]);
    if (b0 == 0) {
        return 0;
    }
    if (b0 < 0x80) {
        return 1;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        len = 2;
        cp = b0 & 0x1FU;
        min = 0x80;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        len = 3;
        cp = b0 & 0x0FU;
        min = 0x800;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        len = 4;
        cp = b0 & 0x07U;
        min = 0x10000;
    } else {
        return 0;
    }
    if (s.size() < len) {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        const auto b = static_cast<std::uint8_t>(s[i]);
        if ((b & 0xC0U) != 0x80U) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3FU);
    }
    if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return 0;
    }
    return len;
}

/// Number of characters if `s` is valid UTF-8 without NUL, otherwise nullopt.
std::optional<std::size_t> utf8_count(std::string_view s) {
    std::size_t chars = 0;
    while (!s.empty()) {
        const std::size_t len = utf8_char_length(s);
        if (len == 0) {
            return std::nullopt;
        }
        s.remove_prefix(len);
        ++chars;
    }
    return chars;
}

/// Writer side: drops malformed bytes and NULs, stops before the byte or character limit is exceeded.
std::string sanitize_text(std::string_view s, std::size_t max_bytes, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    while (!s.empty()) {
        const std::size_t len = utf8_char_length(s);
        if (len == 0) {
            s.remove_prefix(1);
            continue;
        }
        if (out.size() + len > max_bytes || chars + 1 > max_chars) {
            break;
        }
        out.append(s.substr(0, len));
        s.remove_prefix(len);
        ++chars;
    }
    return out;
}

// ---- summary --------------------------------------------------------------------------------------

void put_field(core::ByteWriter& out, SummaryKey key, std::span<const std::uint8_t> value) {
    out.u16(key);
    out.u16(static_cast<std::uint16_t>(value.size()));
    out.bytes(value);
}

void put_text(core::ByteWriter& out, SummaryKey key, const std::string& text) {
    put_field(out,
              key,
              std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

template <class Write>
void put_with(core::ByteWriter& out, SummaryKey key, Write&& write) {
    core::ByteWriter value;
    write(value);
    put_field(out, key, value.data());
}

/// The serialized summary: u16 key | u16 length | bytes, keys ascending. Optional fields that are
/// empty are not written.
std::vector<std::uint8_t> encode_summary(const SaveSummary& in) {
    core::ByteWriter out;
    put_text(out, kKeyGameVersion, sanitize_text(in.game_version, kMaxShortText, kMaxShortText));
    put_with(out, kKeyStateSchema, [&](core::ByteWriter& w) { w.u32(in.state_schema); });
    put_text(out, kKeyScenario, sanitize_text(in.scenario, kMaxShortText, kMaxShortText));
    put_with(out, kKeyContentHash, [&](core::ByteWriter& w) { w.u64(in.content_hash); });
    put_with(out, kKeyPlayerDynasty, [&](core::ByteWriter& w) { w.u32(in.player_dynasty); });
    if (const std::string v = sanitize_text(in.dynasty_key, kMaxShortText, kMaxShortText); !v.empty()) {
        put_text(out, kKeyDynastyKey, v);
    }
    if (const std::string v = sanitize_text(in.head_name, kMaxHeadName, kMaxHeadName); !v.empty()) {
        put_text(out, kKeyHeadName, v);
    }
    if (in.head_age) {
        put_with(out, kKeyHeadAge, [&](core::ByteWriter& w) { w.u8(*in.head_age); });
    }
    put_with(out, kKeyDate, [&](core::ByteWriter& w) { w.i32(in.date); });
    if (in.treasury) {
        put_with(out, kKeyTreasury, [&](core::ByteWriter& w) { w.i64(*in.treasury); });
    }
    if (const std::string v = sanitize_text(in.goal_key, kMaxShortText, kMaxShortText); !v.empty()) {
        put_text(out, kKeyGoalKey, v);
    }
    if (in.goal_progress_ppm) {
        const std::uint32_t ppm = std::min(*in.goal_progress_ppm, kMaxGoalProgressPpm);
        put_with(out, kKeyGoalProgress, [&](core::ByteWriter& w) { w.u32(ppm); });
    }
    put_with(out, kKeyPlayTime, [&](core::ByteWriter& w) { w.u64(in.play_time_s); });
    const std::uint8_t kind = static_cast<std::uint8_t>(in.kind) > kMaxSaveKind
                                  ? static_cast<std::uint8_t>(SaveKind::Manual)
                                  : static_cast<std::uint8_t>(in.kind);
    put_with(out, kKeyKind, [&](core::ByteWriter& w) { w.u8(kind); });
    if (const std::string v = sanitize_text(in.name, kMaxName, kMaxNameChars); !v.empty()) {
        put_text(out, kKeyName, v);
    }
    put_with(out, kKeySavedAt, [&](core::ByteWriter& w) { w.i64(in.saved_at); });
    put_with(out, kKeyWorldHash, [&](core::ByteWriter& w) { w.u64(in.world_hash); });
    put_with(out, kKeyTick, [&](core::ByteWriter& w) { w.i64(in.tick); });
    if (in.ironman != 0) {
        put_with(out, kKeyIronman, [&](core::ByteWriter& w) { w.u8(in.ironman); });
    }
    // The preview goes last and is dropped if it does not fit: it must never make the file unreadable.
    if (!in.preview.empty() && in.preview.size() <= 0xFFFFU &&
        out.size() + 4 + in.preview.size() <= kMaxSaveSummary) {
        put_field(out, kKeyPreview, in.preview);
    }
    return out.take();
}

core::Result<SaveSummary, Error> decode_summary(std::span<const std::uint8_t> bytes) {
    using R = core::Result<SaveSummary, Error>;
    SaveSummary out;
    std::array<bool, 64> seen{};
    core::ByteReader in(bytes);
    std::uint16_t previous = 0;
    std::size_t fields = 0;

    const auto bad = [](const char* message) { return R::err(save_error(kErrSummaryCorrupt, message)); };

    const auto text = [](std::span<const std::uint8_t> v, std::size_t max_bytes, std::string& dst) {
        if (v.size() > max_bytes) {
            return false;
        }
        const std::string_view sv(reinterpret_cast<const char*>(v.data()), v.size());
        if (!utf8_count(sv)) {
            return false;
        }
        dst.assign(sv);
        return true;
    };

    while (!in.at_end()) {
        const std::uint16_t key = in.u16();
        const std::uint16_t length = in.u16();
        const auto value = in.bytes(length);
        if (!in.ok()) {
            return bad("summary field is truncated");
        }
        if (key <= previous) {
            return bad("summary keys are not strictly ascending");
        }
        previous = key;
        if (++fields > kMaxSummaryFields) {
            return bad("too many summary fields");
        }
        if (key < seen.size()) {
            seen[key] = true;
        }
        core::ByteReader v(value);
        bool ok = true;
        switch (key) {
            case kKeyGameVersion:
                ok = text(value, kMaxShortText, out.game_version);
                break;
            case kKeyStateSchema:
                ok = length == 4;
                out.state_schema = v.u32();
                break;
            case kKeyScenario:
                ok = text(value, kMaxShortText, out.scenario);
                break;
            case kKeyContentHash:
                ok = length == 8;
                out.content_hash = v.u64();
                break;
            case kKeyPlayerDynasty:
                ok = length == 4;
                out.player_dynasty = v.u32();
                break;
            case kKeyDynastyKey:
                ok = text(value, kMaxShortText, out.dynasty_key);
                break;
            case kKeyHeadName:
                ok = text(value, kMaxHeadName, out.head_name);
                break;
            case kKeyHeadAge:
                ok = length == 1;
                out.head_age = v.u8();
                break;
            case kKeyDate:
                ok = length == 4;
                out.date = v.i32();
                break;
            case kKeyTreasury:
                ok = length == 8;
                out.treasury = v.i64();
                break;
            case kKeyGoalKey:
                ok = text(value, kMaxShortText, out.goal_key);
                break;
            case kKeyGoalProgress:
                ok = length == 4;
                out.goal_progress_ppm = v.u32();
                ok = ok && *out.goal_progress_ppm <= kMaxGoalProgressPpm;
                break;
            case kKeyPlayTime:
                ok = length == 8;
                out.play_time_s = v.u64();
                break;
            case kKeyKind: {
                ok = length == 1;
                const std::uint8_t kind = v.u8();
                ok = ok && kind <= kMaxSaveKind;
                out.kind = static_cast<SaveKind>(kind);
                break;
            }
            case kKeyName: {
                ok = text(value, kMaxName, out.name);
                const auto chars = utf8_count(out.name);
                ok = ok && chars && *chars >= 1 && *chars <= kMaxNameChars;
                break;
            }
            case kKeySavedAt:
                ok = length == 8;
                out.saved_at = v.i64();
                break;
            case kKeyWorldHash:
                ok = length == 8;
                out.world_hash = v.u64();
                break;
            case kKeyTick:
                ok = length == 8;
                out.tick = v.i64();
                break;
            case kKeyIronman:
                ok = length == 1;
                out.ironman = v.u8();
                break;
            case kKeyPreview:
                out.preview.assign(value.begin(), value.end());
                break;
            default:
                break;  // an unknown key: the summary grows without a new container version
        }
        if (!ok) {
            return bad("summary field has an invalid length or value");
        }
    }

    for (const SummaryKey required : {kKeyGameVersion,
                                      kKeyStateSchema,
                                      kKeyScenario,
                                      kKeyContentHash,
                                      kKeyPlayerDynasty,
                                      kKeyDate,
                                      kKeyPlayTime,
                                      kKeyKind,
                                      kKeySavedAt,
                                      kKeyWorldHash,
                                      kKeyTick}) {
        if (!seen[required]) {
            return R::err(save_error(kErrSummaryMissing, "a required summary field is missing"));
        }
    }
    return R::ok(std::move(out));
}

// ---- container header -----------------------------------------------------------------------------

struct Container {
    SaveInfo info;
    std::uint64_t payload_hash = 0;
    std::span<const std::uint8_t> body;  ///< the zstd frame; empty when only a prefix was given
};

/// Parses the header and (version 2) the summary. `file_size` = size of the whole file, 0 if `bytes`
/// is the whole file. Does not touch zstd.
core::Result<Container, Error> parse_container(std::span<const std::uint8_t> bytes, std::uint64_t file_size) {
    using R = core::Result<Container, Error>;
    const std::uint64_t total = file_size != 0 ? file_size : bytes.size();
    if (total < bytes.size()) {
        return R::err(save_error(kErrFormat, "save file size does not match its header"));
    }
    if (bytes.size() < 8) {
        return R::err(save_error(kErrFormat, "save file is too short"));
    }

    core::ByteReader header(bytes);
    const auto magic = header.bytes(kMagic.size());
    if (!std::equal(magic.begin(), magic.end(), kMagic.begin())) {
        return R::err(save_error(kErrFormat, "not a Patriarch save (bad magic)"));
    }
    const std::uint16_t version = header.u16();
    const std::uint16_t flags = header.u16();
    // The version is judged before the rest of the header: a newer writer may lay it out differently.
    if (version == 0 || version > kSaveFormatVersion) {
        return R::err(save_error(kErrNewer,
                                 version == 0 ? "invalid save version"
                                              : "this save was created by a newer version of the game"));
    }
    if (flags != 0) {  // none defined yet, so any set bit means damage or a newer writer
        return R::err(save_error(kErrFormat, "unsupported save flags"));
    }

    Container c;
    c.info.format_version = version;

    if (version == kSaveFormatVersionLegacy) {
        if (bytes.size() < kSaveHeaderSizeV1 || total < kSaveHeaderSizeV1) {
            return R::err(save_error(kErrFormat, "save file is too short"));
        }
        c.info.uncompressed_size = header.u64();
        c.payload_hash = header.u64();
        c.info.compressed_size = header.u64();
        if (c.info.compressed_size != total - kSaveHeaderSizeV1) {
            return R::err(save_error(kErrFormat, "save file size does not match its header"));
        }
        if (c.info.uncompressed_size > kMaxPayload) {
            return R::err(save_error(kErrFormat, "implausible save size"));
        }
        if (bytes.size() == total) {
            c.body = bytes.subspan(kSaveHeaderSizeV1);
        }
        return R::ok(std::move(c));
    }

    if (bytes.size() < kSaveHeaderSize) {
        return R::err(save_error(kErrFormat, "save file is too short"));
    }
    const std::uint32_t summary_size = header.u32();
    const std::uint64_t summary_hash = header.u64();
    c.info.uncompressed_size = header.u64();
    c.payload_hash = header.u64();
    c.info.compressed_size = header.u64();

    if (summary_size > kMaxSaveSummary) {
        return R::err(save_error(kErrSummaryTooLarge, "save summary is too large"));
    }
    if (summary_size == 0) {
        return R::err(save_error(kErrSummaryCorrupt, "save summary is empty"));
    }
    if (total < kSaveHeaderSize + summary_size || bytes.size() < kSaveHeaderSize + summary_size ||
        c.info.compressed_size != total - kSaveHeaderSize - summary_size) {
        return R::err(save_error(kErrFormat, "save file size does not match its header"));
    }
    if (c.info.uncompressed_size > kMaxPayload) {
        return R::err(save_error(kErrFormat, "implausible save size"));
    }

    const auto summary_bytes = bytes.subspan(kSaveHeaderSize, summary_size);
    if (payload_hash(summary_bytes) != summary_hash) {
        return R::err(save_error(kErrSummaryCorrupt, "save summary is corrupted (hash mismatch)"));
    }
    auto summary = decode_summary(summary_bytes);
    if (!summary.has_value()) {
        return R::err(summary.error());
    }
    c.info.summary = std::move(summary).value();
    if (bytes.size() == total) {
        c.body = bytes.subspan(kSaveHeaderSize + summary_size);
    }
    return R::ok(std::move(c));
}

}  // namespace

SaveSummary summary_from_world(const World& world) {
    SaveSummary s;
    s.game_version = std::string(core::version());
    s.state_schema = World::kStateSchema;
    s.content_hash = world.economy().meta.rules_hash;
    s.date = world.today().days();
    s.tick = world.calendar().tick_count();
    s.world_hash = world.compute_hash().total;
    return s;
}

std::vector<std::uint8_t>
serialize_save(const World& world, std::span<const Command> pending_internal, const SaveSummary& summary) {
    core::ByteWriter payload;
    world.save_state(payload);
    payload.u32(static_cast<std::uint32_t>(pending_internal.size()));
    for (const Command& c : pending_internal) {
        write_command(payload, c);
    }

    const std::vector<std::uint8_t> compressed = core::compress(payload.data());
    const std::vector<std::uint8_t> summary_bytes = encode_summary(summary);

    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kSaveFormatVersion);
    out.u16(0);  // flags
    out.u32(static_cast<std::uint32_t>(summary_bytes.size()));
    out.u64(payload_hash(summary_bytes));
    out.u64(payload.size());
    out.u64(payload_hash(payload.data()));
    out.u64(compressed.size());
    out.bytes(summary_bytes);
    out.bytes(compressed);
    return out.take();
}

std::vector<std::uint8_t> serialize_save(const World& world, std::span<const Command> pending_internal) {
    return serialize_save(world, pending_internal, summary_from_world(world));
}

core::Result<SaveInfo, Error> read_save_summary(std::span<const std::uint8_t> bytes,
                                                std::uint64_t file_size) {
    using R = core::Result<SaveInfo, Error>;
    auto container = parse_container(bytes, file_size);
    if (!container.has_value()) {
        return R::err(container.error());
    }
    return R::ok(std::move(container).value().info);
}

core::Result<SaveInfo, Error> read_save_summary(const std::filesystem::path& path) {
    using R = core::Result<SaveInfo, Error>;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return R::err(save_error(33, "cannot open the file for reading"));
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return R::err(save_error(33, "cannot determine the file size"));
    }
    in.seekg(0);
    const auto file_size = static_cast<std::size_t>(size);

    // The header first; then exactly as much as it says the summary takes (never past the bound).
    std::vector<std::uint8_t> bytes(std::min(file_size, kSaveHeaderSize));
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!in) {
        return R::err(save_error(34, "failed while reading the file"));
    }
    if (bytes.size() == kSaveHeaderSize) {
        core::ByteReader peek(bytes);
        (void)peek.bytes(4);
        const std::uint16_t version = peek.u16();
        (void)peek.u16();
        const std::size_t summary_size = peek.u32();
        if (version == kSaveFormatVersion) {
            const std::size_t more =
                std::min(std::min(summary_size, kMaxSaveSummary), file_size - kSaveHeaderSize);
            bytes.resize(kSaveHeaderSize + more);
            in.read(reinterpret_cast<char*>(bytes.data() + kSaveHeaderSize),
                    static_cast<std::streamsize>(more));
            if (!in) {
                return R::err(save_error(34, "failed while reading the file"));
            }
        }
    }
    return read_save_summary(bytes, static_cast<std::uint64_t>(file_size));
}

core::Result<SaveData, Error> deserialize_save(std::span<const std::uint8_t> bytes) {
    using R = core::Result<SaveData, Error>;
    auto container = parse_container(bytes, 0);
    if (!container.has_value()) {
        return R::err(container.error());
    }
    const Container& c = container.value();
    const std::uint16_t version = c.info.format_version;
    if (c.info.summary && c.info.summary->state_schema > World::kStateSchema) {
        return R::err(
            save_error(kErrSummarySchemaNewer, "this save was created by a newer version of the game"));
    }

    auto decompressed = core::decompress(c.body, static_cast<std::size_t>(c.info.uncompressed_size));
    if (!decompressed.has_value()) {
        return R::err(decompressed.error());
    }
    if (payload_hash(decompressed.value()) != c.payload_hash) {
        return R::err(save_error(kErrPayloadHash, "save is corrupted (hash mismatch)"));
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
        return R::err(save_error(kErrPending, "invalid pending command list"));
    }
    std::vector<Command> pending;
    pending.reserve(pending_count);
    for (std::uint32_t i = 0; i < pending_count; ++i) {
        Command cmd;
        if (!read_command(in, cmd)) {
            return R::err(save_error(kErrPending, "invalid pending command"));
        }
        pending.push_back(cmd);
    }
    if (!in.at_end()) {
        return R::err(save_error(kErrPending, "trailing bytes after save payload"));
    }

    SaveData data{std::move(world).value(), std::move(pending), {}, version};
    if (c.info.summary) {
        // The summary and the payload must agree on the schema word that opens the payload.
        core::ByteReader schema_word(payload);
        if (schema_word.u32() != c.info.summary->state_schema) {
            return R::err(save_error(kErrSchemaMismatch, "save summary and payload disagree on the schema"));
        }
        data.summary = *c.info.summary;
    } else {
        // A legacy file has no summary: the minimum that the world itself knows.
        data.summary = summary_from_world(data.world);
        data.summary.game_version.clear();  // the writer's build is unknown
    }
    return R::ok(std::move(data));
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

std::future<core::Result<void, Error>> save_async(std::shared_ptr<const World> snapshot,
                                                  std::vector<Command> pending_internal,
                                                  std::filesystem::path path,
                                                  SaveSummary summary) {
    return std::async(std::launch::async,
                      [snapshot = std::move(snapshot),
                       pending = std::move(pending_internal),
                       path = std::move(path),
                       summary = std::move(summary)] {
                          const std::vector<std::uint8_t> bytes = serialize_save(*snapshot, pending, summary);
                          return write_file_atomic(path, bytes);
                      });
}

}  // namespace pat::sim
