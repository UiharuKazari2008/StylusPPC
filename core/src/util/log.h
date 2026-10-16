#pragma once
#include <cstdio>

namespace wm {
enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };
void logf(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern LogLevel g_logLevel;
}

#define LOGD(...) do { if (wm::g_logLevel <= wm::LOG_DEBUG) wm::logf(wm::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGI(...) wm::logf(wm::LOG_INFO, __VA_ARGS__)
#define LOGW(...) wm::logf(wm::LOG_WARN, __VA_ARGS__)
#define LOGE(...) wm::logf(wm::LOG_ERROR, __VA_ARGS__)
