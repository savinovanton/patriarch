#include "pat/core/log.hpp"

#include <cstdio>
#include <fstream>

namespace pat::core {

namespace {

std::string format_line(const LogRecord& r) {
    return std::format("[{:>8}.{:03}] [{:<5}] {}: {}\n",
                       r.elapsed_ms / 1000,
                       r.elapsed_ms % 1000,
                       to_string(r.level),
                       r.category,
                       r.message);
}

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
        case LogLevel::Off:
            return "OFF";
    }
    return "?";
}

void ConsoleSink::write(const LogRecord& record) {
    const std::string line = format_line(record);
    std::fwrite(line.data(), 1, line.size(), stderr);
}

struct FileSink::Impl {
    std::ofstream stream;
};

FileSink::FileSink(const std::string& path) : impl_(std::make_unique<Impl>()) {
    impl_->stream.open(path, std::ios::out | std::ios::app);
}

FileSink::~FileSink() = default;

bool FileSink::is_open() const noexcept {
    return impl_->stream.is_open();
}

void FileSink::write(const LogRecord& record) {
    if (!impl_->stream.is_open()) {
        return;
    }
    const std::string line = format_line(record);
    impl_->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->stream.flush();
}

void MemorySink::write(const LogRecord& record) {
    std::scoped_lock lock(mutex_);
    entries_.push_back(Entry{record.level, std::string(record.category), std::string(record.message)});
}

std::vector<MemorySink::Entry> MemorySink::entries() const {
    std::scoped_lock lock(mutex_);
    return entries_;
}

std::size_t MemorySink::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

bool MemorySink::contains(std::string_view substring) const {
    std::scoped_lock lock(mutex_);
    for (const Entry& e : entries_) {
        if (e.message.find(substring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void MemorySink::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

Logger::Logger()
    :
#ifdef NDEBUG
      min_level_(LogLevel::Info),
#else
      min_level_(LogLevel::Debug),
#endif
      start_(std::chrono::steady_clock::now()) {
    sinks_.push_back(std::make_shared<ConsoleSink>());
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

LogLevel Logger::set_min_level(LogLevel level) noexcept {
    return min_level_.exchange(level);
}

LogLevel Logger::min_level() const noexcept {
    return min_level_.load();
}

bool Logger::enabled(LogLevel level) const noexcept {
    return level >= min_level_.load();
}

void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    std::scoped_lock lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clear_sinks() {
    std::scoped_lock lock(mutex_);
    sinks_.clear();
}

std::vector<std::shared_ptr<LogSink>> Logger::swap_sinks(std::vector<std::shared_ptr<LogSink>> sinks) {
    std::scoped_lock lock(mutex_);
    sinks_.swap(sinks);
    return sinks;
}

void Logger::log(
    LogLevel level, std::string_view category, std::string_view message, const char* file, int line) {
    if (!enabled(level)) {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    const LogRecord record{level, category, message, elapsed.count(), file, line};
    std::scoped_lock lock(mutex_);
    for (const auto& sink : sinks_) {
        sink->write(record);
    }
}

ScopedLogCapture::ScopedLogCapture() : sink_(std::make_shared<MemorySink>()) {
    Logger& logger = Logger::instance();
    saved_level_ = logger.set_min_level(LogLevel::Trace);
    saved_sinks_ = logger.swap_sinks({sink_});
}

ScopedLogCapture::~ScopedLogCapture() {
    Logger& logger = Logger::instance();
    logger.swap_sinks(std::move(saved_sinks_));
    logger.set_min_level(saved_level_);
}

}  // namespace pat::core
