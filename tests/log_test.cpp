#include "pat/core/log.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace pat::core;

TEST_CASE("log: records reach the capture sink with formatting") {
    ScopedLogCapture capture;
    PAT_LOG_INFO("test", "value={} name={}", 42, "ivan");

    const auto entries = capture.sink().entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == LogLevel::Info);
    CHECK(entries[0].category == "test");
    CHECK(entries[0].message == "value=42 name=ivan");
}

TEST_CASE("log: records below the minimum level are dropped") {
    ScopedLogCapture capture;
    Logger::instance().set_min_level(LogLevel::Warn);

    PAT_LOG_DEBUG("test", "hidden {}", 1);
    PAT_LOG_INFO("test", "hidden {}", 2);
    PAT_LOG_WARN("test", "shown {}", 3);
    PAT_LOG_ERROR("test", "shown {}", 4);

    CHECK(capture.sink().size() == 2);
    CHECK(capture.sink().contains("shown 3"));
    CHECK_FALSE(capture.sink().contains("hidden"));
}

TEST_CASE("log: capture restores the previous logger state") {
    const LogLevel before = Logger::instance().min_level();
    {
        ScopedLogCapture capture;
        CHECK(Logger::instance().min_level() == LogLevel::Trace);
    }
    CHECK(Logger::instance().min_level() == before);
}

TEST_CASE("log: file sink appends formatted lines") {
    const auto path = std::filesystem::temp_directory_path() / "patriarch_log_test.log";
    std::filesystem::remove(path);
    {
        auto file_sink = std::make_shared<FileSink>(path.string());
        REQUIRE(file_sink->is_open());

        ScopedLogCapture capture;  // silences the console
        Logger::instance().add_sink(file_sink);
        PAT_LOG_INFO("file", "hello {}", "world");
    }

    std::string text;
    {
        std::ifstream in(path);  // must be closed before the file can be removed on Windows
        std::stringstream content;
        content << in.rdbuf();
        text = content.str();
    }
    CHECK(text.find("[INFO ] file: hello world") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("log: level names") {
    CHECK(to_string(LogLevel::Warn) == "WARN");
    CHECK(to_string(LogLevel::Fatal) == "FATAL");
}
