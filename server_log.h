#pragma once
//=============================================================================
// server_log.h
//
// Minimal leveled, timestamped logger for the dedicated server.
// Header-only; auto-flushes stdout so logs survive a crash.
//=============================================================================

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>

namespace ServerLog
{
    inline void Print(const char* level, const char* fmt, va_list args)
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        struct tm tm_local{};
#ifdef _WIN32
        localtime_s(&tm_local, &t);
#else
        localtime_r(&t, &tm_local);
#endif
        std::printf("[%02d:%02d:%02d.%03d][%s] ",
            tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
            static_cast<int>(ms.count()), level);
        std::vprintf(fmt, args);
        std::printf("\n");
        std::fflush(stdout);
    }

    inline void Info(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt); Print("INFO", fmt, args); va_end(args);
    }
    inline void Warn(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt); Print("WARN", fmt, args); va_end(args);
    }
    inline void Error(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt); Print("ERROR", fmt, args); va_end(args);
    }
}

#define SLOG_INFO(...)  ::ServerLog::Info(__VA_ARGS__)
#define SLOG_WARN(...)  ::ServerLog::Warn(__VA_ARGS__)
#define SLOG_ERROR(...) ::ServerLog::Error(__VA_ARGS__)
