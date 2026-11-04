/* log_msg for a standalone host build of upstream's tools/languagetool (rg35xx/build.sh assets): upstream's
 * src/utils/log.c needs SDL for its mutex, and the tool only ever logs to stderr. */
#include <stdarg.h>
#include <stdio.h>
#include "utils/log.h"

void log_msg(log_level level, const char *fmt, ...) {
    va_list a;
    (void)level;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
