#include <os/logger.h>
#include <rex/logging.h>
#include <rex/diagnostics/policy.h>

#include <spdlog/sinks/ansicolor_sink.h>

void os::logger::PlatformInitSinks()
{
    if (!rex::diagnostics::IsEnabled(
            rex::diagnostics::Category::kLogging)) return;
    auto sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
    rex::AddSink(sink);
}

void os::logger::PlatformShutdownSinks()
{
    // spdlog handles sink cleanup on shutdown; nothing to do here.
}

// Same contract as the macOS logger: evict rex's early stdout sink without
// installing a console sink here, then let PlatformInitSinks own output.
void os::logger::Init()
{
    if (!rex::diagnostics::IsEnabled(
            rex::diagnostics::Category::kLogging)) return;
    rex::LogConfig cfg;
    cfg.log_to_console = false;      // PlatformInitSinks adds the real console sink.
    cfg.default_level  = spdlog::level::trace;
    cfg.flush_level    = spdlog::level::trace;
    rex::InitLogging(cfg);

    PlatformInitSinks();
}

void os::logger::Shutdown()
{
    PlatformShutdownSinks();
}
