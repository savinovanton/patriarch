#include "pat/sim/command_log.hpp"

#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace pat::sim {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'C', 'L', '1'};

core::Error format_error(const char* message) {
    return core::Error{1, message};
}

std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

struct Writer {
    core::ByteWriter& out;

    void operator()(const cmd::SetProvinceOwner& c) const {
        out.u32(c.province.value());
        out.u32(c.new_owner.value());
    }
    void operator()(const cmd::CreateCharacter& c) const {
        out.u32(c.dynasty.value());
        out.i32(c.birth_date.days());
        out.u8(static_cast<std::uint8_t>(c.sex));
        out.u32(c.father.value());
        out.u32(c.mother.value());
    }
    void operator()(const cmd::KillCharacter& c) const { out.u32(c.character.value()); }
    void operator()(const cmd::SetDynastyHead& c) const {
        out.u32(c.dynasty.value());
        out.u32(c.head.value());
    }
    void operator()(const cmd::AdjustHonor& c) const {
        out.u32(c.dynasty.value());
        out.i32(c.delta);
    }
};

}  // namespace

void write_command(core::ByteWriter& out, const Command& command) {
    out.u8(static_cast<std::uint8_t>(type_of(command)));
    out.u32(command.actor.value());
    std::visit(Writer{out}, command.payload);
}

bool read_command(core::ByteReader& in, Command& out) {
    const std::uint8_t tag = in.u8();
    out.actor = DynastyId(in.u32());
    switch (static_cast<CommandType>(tag)) {
        case CommandType::SetProvinceOwner: {
            cmd::SetProvinceOwner c;
            c.province = ProvinceId(in.u32());
            c.new_owner = DynastyId(in.u32());
            out.payload = c;
            break;
        }
        case CommandType::CreateCharacter: {
            cmd::CreateCharacter c;
            c.dynasty = DynastyId(in.u32());
            c.birth_date = Date::from_days(in.i32());
            const std::uint8_t sex = in.u8();
            if (sex > 1) {
                return false;
            }
            c.sex = static_cast<Sex>(sex);
            c.father = CharacterId(in.u32());
            c.mother = CharacterId(in.u32());
            out.payload = c;
            break;
        }
        case CommandType::KillCharacter: {
            cmd::KillCharacter c;
            c.character = CharacterId(in.u32());
            out.payload = c;
            break;
        }
        case CommandType::SetDynastyHead: {
            cmd::SetDynastyHead c;
            c.dynasty = DynastyId(in.u32());
            c.head = CharacterId(in.u32());
            out.payload = c;
            break;
        }
        case CommandType::AdjustHonor: {
            cmd::AdjustHonor c;
            c.dynasty = DynastyId(in.u32());
            c.delta = in.i32();
            out.payload = c;
            break;
        }
        default:
            return false;  // unknown tag
    }
    return in.ok();
}

std::vector<Command> CommandLog::external_commands_on(Date date) const {
    std::vector<Command> out;
    for (const LoggedCommand& e : entries_) {
        if (e.date == date && e.source == CommandSource::External && e.accepted()) {
            out.push_back(e.command);
        }
    }
    return out;
}

std::vector<std::uint8_t> CommandLog::serialize() const {
    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kFormatVersion);
    out.u32(static_cast<std::uint32_t>(entries_.size()));
    for (const LoggedCommand& e : entries_) {
        out.i32(e.date.days());
        out.u8(static_cast<std::uint8_t>(e.source));
        out.u8(static_cast<std::uint8_t>(e.outcome));
        write_command(out, e.command);
    }
    out.u64(checksum(out.data()));
    return out.take();
}

core::Result<CommandLog, core::Error> CommandLog::deserialize(std::span<const std::uint8_t> bytes) {
    using R = core::Result<CommandLog, core::Error>;
    if (bytes.size() < kMagic.size() + 2 + 4 + 8) {
        return R::err(format_error("command log is too short"));
    }

    // Integrity first: the last 8 bytes are a checksum of everything before them.
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 8);
    core::ByteReader tail(bytes.last(8));
    if (tail.u64() != checksum(body)) {
        return R::err(format_error("command log checksum mismatch"));
    }

    core::ByteReader in(body);
    const std::span<const std::uint8_t> magic = in.bytes(kMagic.size());
    if (!in.ok() || !std::equal(magic.begin(), magic.end(), kMagic.begin())) {
        return R::err(format_error("not a command log (bad magic)"));
    }
    if (in.u16() != kFormatVersion) {
        return R::err(format_error("unsupported command log version"));
    }
    const std::uint32_t count = in.u32();

    CommandLog log;
    for (std::uint32_t i = 0; i < count; ++i) {
        LoggedCommand entry;
        entry.date = Date::from_days(in.i32());
        const std::uint8_t source = in.u8();
        const std::uint8_t outcome = in.u8();
        if (!in.ok() || source > 1 || outcome > static_cast<std::uint8_t>(Rejection::InvalidState)) {
            return R::err(format_error("corrupt command log entry header"));
        }
        entry.source = static_cast<CommandSource>(source);
        entry.outcome = static_cast<Rejection>(outcome);
        if (!read_command(in, entry.command)) {
            return R::err(format_error("corrupt or unknown command in log"));
        }
        log.append(entry);
    }
    if (!in.at_end()) {
        return R::err(format_error("trailing bytes in command log"));
    }
    return R::ok(std::move(log));
}

}  // namespace pat::sim
