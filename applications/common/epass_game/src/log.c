#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void epass_log(const char *level, const char *file, int line,
               const char *format, ...)
{
    va_list args;
    fprintf(stderr, "[%s] %s:%d: ", level, file, line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}
