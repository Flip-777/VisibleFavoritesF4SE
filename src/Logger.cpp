#include "Logger.h"

#include <filesystem>

#include <spdlog/sinks/basic_file_sink.h>

void InitializeLog() {
    auto path = logger::log_directory();
    if (!path) {
        return;
    }

    *path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
    std::error_code ec;
    if (std::filesystem::exists(*path, ec)) {
        auto old = *path;
        old.replace_extension(".old.log");
        std::filesystem::remove(old, ec);
        std::filesystem::rename(*path, old, ec);
    }
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::trace);
    log->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v"s);

    logger::info("{} v{}", Version::PROJECT, Version::NAME);
}
