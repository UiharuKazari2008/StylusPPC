#include "log.h"
#include <cstdarg>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace wm {

LogLevel g_logLevel = LOG_INFO;
static bool s_stderr = true;

void logf(LogLevel level, const char *fmt, ...) {
    if (level < g_logLevel) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_write(prio[level], "WMCore", buf);
#endif
    if (s_stderr) {
        static const char *tag = "DIWE";
        fprintf(stderr, "%c %s\n", tag[level], buf);
    }
}

}  // namespace wm
