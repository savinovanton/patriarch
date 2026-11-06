#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pat::core {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view category;
    std::string_view message;
    std::int64_t elapsed_ms = 0;  // since logger creation
    const char* file = "";
    int line = 0;
};

/// Output target for log records. Implementations must be thread-safe or rely on Logger's lock
/// (Logger serializes calls to sinks).
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

/// Writes formatted lines to stderr.
class ConsoleSink final : public LogSink {
public:
    void write(const LogRecord& record) override;
};

/// Appends formatted lines to a file (flushed on every record).
class FileSink final : public LogSink {
public:
    explicit FileSink(const std::string& path);
    ~FileSink() override;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    void write(const LogRecord& record) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Stores records in memory; intended for tests and in-game log windows.
class MemorySink final : public LogSink {
public:
    struct Entry {
        LogLevel level = LogLevel::Info;
        std::string category;
        std::string message;
    };

    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<Entry> entries() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool contains(std::string_view substring) const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

/// Process-wide logger. Thread-safe. Starts with a ConsoleSink.
class Logger {
public:
    [[nodiscard]] static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// Returns the previous level.
    LogLevel set_min_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel min_level() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    void add_sink(std::shared_ptr<LogSink> sink);
    void clear_sinks();
    /// Replaces the sink list and returns the previous one.
    std::vector<std::shared_ptr<LogSink>> swap_sinks(std::vector<std::shared_ptr<LogSink>> sinks);

    void log(LogLevel level, std::string_view category, std::string_view message, const char* file, int line);

    template <class... Args>
    void logf(LogLevel level,
              std::string_view category,
              const char* file,
              int line,
              std::format_string<Args...> fmt,
              Args&&... args) {
        if (!enabled(level)) {
            return;
        }
        log(level, category, std::format(fmt, std::forward<Args>(args)...), file, line);
    }

private:
    Logger();

    std::atomic<LogLevel> min_level_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::chrono::steady_clock::time_point start_;
};

/// RAII helper for tests: captures everything logged during its lifetime and silences other sinks.
class ScopedLogCapture {
public:
    ScopedLogCapture();
    ~ScopedLogCapture();
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    [[nodiscard]] MemorySink& sink() noexcept { return *sink_; }
    [[nodiscard]] const MemorySink& sink() const noexcept { return *sink_; }

private:
    std::shared_ptr<MemorySink> sink_;
    std::vector<std::shared_ptr<LogSink>> saved_sinks_;
    LogLevel saved_level_;
};

}  // namespace pat::core

#define PAT_LOG(level, category, ...)                                                                        \
    do {                                                                                                     \
        auto& pat_logger_ = ::pat::core::Logger::instance();                                                 \
        if (pat_logger_.enabled(level)) {                                                                    \
            pat_logger_.logf(level, category, __FILE__, __LINE__, __VA_ARGS__);                              \
        }                                                                                                    \
    } while (false)

#define PAT_LOG_TRACE(category, ...) PAT_LOG(::pat::core::LogLevel::Trace, category, __VA_ARGS__)
#define PAT_LOG_DEBUG(category, ...) PAT_LOG(::pat::core::LogLevel::Debug, category, __VA_ARGS__)
#define PAT_LOG_INFO(category, ...) PAT_LOG(::pat::core::LogLevel::Info, category, __VA_ARGS__)
#define PAT_LOG_WARN(category, ...) PAT_LOG(::pat::core::LogLevel::Warn, category, __VA_ARGS__)
#define PAT_LOG_ERROR(category, ...) PAT_LOG(::pat::core::LogLevel::Error, category, __VA_ARGS__)
#define PAT_LOG_FATAL(category, ...) PAT_LOG(::pat::core::LogLevel::Fatal, category, __VA_ARGS__)
