#include "ncaf/internal/logging.h"

#include <spdlog/spdlog.h>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace ncaf::internal::logging {
using ncaf::logging::LogLevel;

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarn:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kFatal:
      return spdlog::level::critical;
  }
  NCAF_THROW << "Invalid log level: " << level;
}

std::unique_ptr<spdlog::logger> CreateLoggerUsingConfig(const ncaf::logging::LogConfig& config) {
  constexpr char kLoggerName[] = "ncaf";
  std::unique_ptr<spdlog::logger> logger;
  if (config.log_file_path.empty()) {
    logger = std::make_unique<spdlog::logger>(kLoggerName, std::make_unique<spdlog::sinks::stdout_color_sink_mt>());
  } else {
    logger = std::make_unique<spdlog::logger>(
        kLoggerName, std::make_unique<spdlog::sinks::basic_file_sink_mt>(config.log_file_path));
  }
  logger->set_level(ToSpdlogLevel(config.level));
  logger->set_pattern(kDefaultLoggerPattern);
  return logger;
}

std::unique_ptr<spdlog::logger>& GlobalLogger() {
  static std::unique_ptr<spdlog::logger> global_logger = CreateLoggerUsingConfig({});
  return global_logger;
}

void InstallFallbackExceptionHandler() {
  std::set_terminate([] {
    if (auto ex = std::current_exception()) {
      try {
        std::rethrow_exception(ex);
      } catch (const std::exception& e) {
        logging::Critical("terminate called with an active exception, type: {}, what: {}", typeid(e).name(), e.what());
      } catch (...) {
        logging::Critical("terminate called with an unknown exception");
      }
    } else {
      logging::Critical("terminate called without an active exception");
    }
    std::abort();
  });
};
}  // namespace ncaf::internal::logging