#pragma once

#include "pat/core/bytes.hpp"
#include "pat/core/result.hpp"
#include "pat/sim/command.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace pat::sim {

/// Binary encoding of single commands (used by the log and by replays).
void write_command(core::ByteWriter& out, const Command& command);
/// Returns false (and leaves `out` unspecified) on truncated data or an unknown command tag.
[[nodiscard]] bool read_command(core::ByteReader& in, Command& out);

struct LoggedCommand {
    Date date;  // the tick on which the command was processed
    CommandSource source = CommandSource::External;
    Rejection outcome = Rejection::None;  // None = accepted and applied
    Command command;

    [[nodiscard]] bool accepted() const noexcept { return outcome == Rejection::None; }
    [[nodiscard]] bool operator==(const LoggedCommand&) const = default;
};

/// Ordered record of every processed command. Serializes to a versioned, checksummed byte stream.
class CommandLog {
public:
    static constexpr std::uint16_t kFormatVersion = 1;

    void append(const LoggedCommand& entry) { entries_.push_back(entry); }
    void clear() { entries_.clear(); }

    [[nodiscard]] std::span<const LoggedCommand> entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Accepted External commands processed on `date`, in processing order (replay input).
    [[nodiscard]] std::vector<Command> external_commands_on(Date date) const;

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] static core::Result<CommandLog, core::Error>
    deserialize(std::span<const std::uint8_t> bytes);

    [[nodiscard]] bool operator==(const CommandLog&) const = default;

private:
    std::vector<LoggedCommand> entries_;
};

}  // namespace pat::sim
