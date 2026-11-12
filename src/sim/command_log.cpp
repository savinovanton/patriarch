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

// The layout of CreatePerson, shared with the newborn of Birth.
void write_init(core::ByteWriter& out, const CharacterInit& i) {
    out.u32(i.dynasty.value());
    out.i32(i.birth_date.days());
    out.u8(static_cast<std::uint8_t>(i.sex));
    out.i32(i.health.raw());
    out.u32(i.father.value());
    out.u32(i.mother.value());
    out.u32(i.name.value());
    out.u32(i.branch.value());
    out.u16(i.portrait);
    out.u8(static_cast<std::uint8_t>(i.legitimacy));
    for (const core::Fixed32 skill : i.skills) {
        out.i32(skill.raw());
    }
    for (const core::Fixed32 v :
         {i.fertility, i.stress, i.piety, i.ambition, i.loyalty_head, i.loyalty_emperor}) {
        out.i32(v.raw());
    }
    for (const std::uint16_t trait : i.traits) {
        out.u16(trait);
    }
    out.u8(i.education.focus);
    out.u32(i.education.tutor.value());
    out.u8(i.education.level);
    out.i32(i.planned_death.days());
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
    void operator()(const cmd::GrantTrait& c) const {
        out.u32(c.character.value());
        out.u16(c.trait);
    }
    void operator()(const cmd::RevokeTrait& c) const {
        out.u32(c.character.value());
        out.u16(c.trait);
    }
    void operator()(const cmd::CreatePerson& c) const { write_init(out, c.init); }
    void operator()(const cmd::Conceive& c) const {
        out.u32(c.mother.value());
        out.u32(c.father.value());
        out.i32(c.conceived.days());
        out.i32(c.due.days());
        out.u8(static_cast<std::uint8_t>(c.end));
        out.u8(static_cast<std::uint8_t>(c.legitimacy));
    }
    void operator()(const cmd::Birth& c) const {
        out.u32(c.mother.value());
        out.u8(static_cast<std::uint8_t>(c.outcome));
        write_init(out, c.newborn);
    }
    void operator()(const cmd::Die& c) const {
        out.u32(c.character.value());
        out.u8(static_cast<std::uint8_t>(c.cause));
    }
    void operator()(const cmd::Marry& c) const {
        out.u32(c.a.value());
        out.u32(c.b.value());
    }
    void operator()(const cmd::EndMarriage& c) const {
        out.u32(c.a.value());
        out.u8(static_cast<std::uint8_t>(c.reason));
    }
    void operator()(const cmd::PlanDeath& c) const {
        out.u32(c.character.value());
        out.i32(c.date.days());
    }
    void operator()(const cmd::RecordChronicle& c) const {
        out.u32(c.dynasty.value());
        out.u8(static_cast<std::uint8_t>(c.kind));
        out.u32(c.a.value());
        out.u32(c.b.value());
        out.i32(c.param);
    }
    void operator()(const cmd::AssignProvinceAdministrator& c) const {
        out.u32(c.province.value());
        out.u32(c.administrator.value());
    }
    void operator()(const cmd::SetTaxPolicy& c) const {
        out.u32(c.dynasty.value());
        out.u8(c.level);
    }
    void operator()(const cmd::SetRecruitPolicy& c) const {
        out.u32(c.dynasty.value());
        out.u8(c.level);
    }
    void operator()(const cmd::ConscriptRecruits& c) const {
        out.u32(c.province.value());
        out.u32(c.count);
    }
    void operator()(const cmd::StartConstruction& c) const {
        out.u32(c.province.value());
        out.u8(c.building);
    }
    void operator()(const cmd::CancelConstruction& c) const {
        out.u32(c.province.value());
        out.u8(c.slot);
    }
    void operator()(const cmd::TakeLoan& c) const { out.i64(c.amount_kop); }
    void operator()(const cmd::RepayLoan& c) const {
        out.u32(c.loan_id);
        out.i64(c.amount_kop);
    }
    void operator()(const cmd::AdjustOpinion& c) const {
        out.u32(c.from.value());
        out.u32(c.to.value());
        out.i32(c.delta.raw());
        out.u16(c.reason);
    }
    void standing_change(DynastyId dynasty, std::int32_t delta, StandingReason reason) const {
        out.u32(dynasty.value());
        out.i32(delta);
        out.u16(reason);
    }
    void operator()(const cmd::AdjustUnity& c) const { standing_change(c.dynasty, c.delta.raw(), c.reason); }
    void operator()(const cmd::AdjustPrestige& c) const {
        standing_change(c.dynasty, c.delta.raw(), c.reason);
    }
    void operator()(const cmd::AdjustHonorWithReason& c) const {
        standing_change(c.dynasty, c.delta, c.reason);
    }
};

// Reads the layout written by write_init; false for a value the type cannot hold.
bool read_init(core::ByteReader& in, CharacterInit& i) {
    i.dynasty = DynastyId(in.u32());
    i.birth_date = Date::from_days(in.i32());
    const std::uint8_t sex = in.u8();
    if (sex > 1) {
        return false;
    }
    i.sex = static_cast<Sex>(sex);
    i.health = core::Fixed32::from_raw(in.i32());
    i.father = CharacterId(in.u32());
    i.mother = CharacterId(in.u32());
    i.name = core::StringId(in.u32());
    i.branch = BranchId(in.u32());
    i.portrait = in.u16();
    const std::uint8_t legitimacy = in.u8();
    if (legitimacy >= kLegitimacyCount) {
        return false;
    }
    i.legitimacy = static_cast<Legitimacy>(legitimacy);
    for (core::Fixed32& skill : i.skills) {
        skill = core::Fixed32::from_raw(in.i32());
    }
    for (core::Fixed32* v :
         {&i.fertility, &i.stress, &i.piety, &i.ambition, &i.loyalty_head, &i.loyalty_emperor}) {
        *v = core::Fixed32::from_raw(in.i32());
    }
    for (std::uint16_t& trait : i.traits) {
        trait = in.u16();
    }
    i.education.focus = in.u8();
    i.education.tutor = CharacterId(in.u32());
    i.education.level = in.u8();
    i.planned_death = Date::from_days(in.i32());
    return true;
}

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
        case CommandType::GrantTrait: {
            const CharacterId character(in.u32());
            out.payload = cmd::GrantTrait{character, in.u16()};
            break;
        }
        case CommandType::RevokeTrait: {
            const CharacterId character(in.u32());
            out.payload = cmd::RevokeTrait{character, in.u16()};
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
        case CommandType::CreatePerson: {
            cmd::CreatePerson c;
            if (!read_init(in, c.init)) {
                return false;
            }
            out.payload = c;
            break;
        }
        case CommandType::Conceive: {
            cmd::Conceive c;
            c.mother = CharacterId(in.u32());
            c.father = CharacterId(in.u32());
            c.conceived = Date::from_days(in.i32());
            c.due = Date::from_days(in.i32());
            const std::uint8_t end = in.u8();
            const std::uint8_t legitimacy = in.u8();
            if (end >= kPregnancyEndCount || legitimacy >= kLegitimacyCount) {
                return false;
            }
            c.end = static_cast<PregnancyEnd>(end);
            c.legitimacy = static_cast<Legitimacy>(legitimacy);
            out.payload = c;
            break;
        }
        case CommandType::Birth: {
            cmd::Birth c;
            c.mother = CharacterId(in.u32());
            const std::uint8_t outcome = in.u8();
            if (outcome >= kBirthOutcomeCount || !read_init(in, c.newborn)) {
                return false;
            }
            c.outcome = static_cast<BirthOutcome>(outcome);
            out.payload = c;
            break;
        }
        case CommandType::Die: {
            cmd::Die c;
            c.character = CharacterId(in.u32());
            const std::uint8_t cause = in.u8();
            if (cause >= kCauseOfDeathCount) {
                return false;
            }
            c.cause = static_cast<CauseOfDeath>(cause);
            out.payload = c;
            break;
        }
        case CommandType::Marry: {
            cmd::Marry c;
            c.a = CharacterId(in.u32());
            c.b = CharacterId(in.u32());
            out.payload = c;
            break;
        }
        case CommandType::EndMarriage: {
            cmd::EndMarriage c;
            c.a = CharacterId(in.u32());
            const std::uint8_t reason = in.u8();
            if (reason >= kMarriageEndCount) {
                return false;
            }
            c.reason = static_cast<MarriageEnd>(reason);
            out.payload = c;
            break;
        }
        case CommandType::PlanDeath: {
            const CharacterId character(in.u32());
            out.payload = cmd::PlanDeath{character, Date::from_days(in.i32())};
            break;
        }
        case CommandType::RecordChronicle: {
            cmd::RecordChronicle c;
            c.dynasty = DynastyId(in.u32());
            const auto kind = static_cast<ChronicleKind>(in.u8());
            if (kind < ChronicleKind::Birth || kind > ChronicleKind::Note) {
                return false;
            }
            c.kind = kind;
            c.a = CharacterId(in.u32());
            c.b = CharacterId(in.u32());
            c.param = in.i32();
            out.payload = c;
            break;
        }
        case CommandType::AdjustOpinion: {
            out.payload = cmd::AdjustOpinion{
                CharacterId(in.u32()), CharacterId(in.u32()), core::Fixed32::from_raw(in.i32()), in.u16()};
            break;
        }
        case CommandType::AssignProvinceAdministrator: {
            const ProvinceId province(in.u32());
            out.payload = cmd::AssignProvinceAdministrator{province, CharacterId(in.u32())};
            break;
        }
        case CommandType::SetTaxPolicy: {
            const DynastyId dynasty(in.u32());
            out.payload = cmd::SetTaxPolicy{dynasty, in.u8()};
            break;
        }
        case CommandType::SetRecruitPolicy: {
            const DynastyId dynasty(in.u32());
            out.payload = cmd::SetRecruitPolicy{dynasty, in.u8()};
            break;
        }
        case CommandType::ConscriptRecruits: {
            const ProvinceId province(in.u32());
            out.payload = cmd::ConscriptRecruits{province, in.u32()};
            break;
        }
        case CommandType::StartConstruction: {
            const ProvinceId province(in.u32());
            out.payload = cmd::StartConstruction{province, in.u8()};
            break;
        }
        case CommandType::CancelConstruction: {
            const ProvinceId province(in.u32());
            out.payload = cmd::CancelConstruction{province, in.u8()};
            break;
        }
        case CommandType::TakeLoan:
            out.payload = cmd::TakeLoan{in.i64()};
            break;
        case CommandType::RepayLoan: {
            const std::uint32_t loan_id = in.u32();
            out.payload = cmd::RepayLoan{loan_id, in.i64()};
            break;
        }
        case CommandType::AdjustUnity:
            out.payload = cmd::AdjustUnity{DynastyId(in.u32()), core::Fixed32::from_raw(in.i32()), in.u16()};
            break;
        case CommandType::AdjustPrestige:
            out.payload =
                cmd::AdjustPrestige{DynastyId(in.u32()), core::Fixed32::from_raw(in.i32()), in.u16()};
            break;
        case CommandType::AdjustHonorWithReason:
            out.payload = cmd::AdjustHonorWithReason{DynastyId(in.u32()), in.i32(), in.u16()};
            break;
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
