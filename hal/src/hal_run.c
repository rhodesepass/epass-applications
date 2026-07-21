#define _POSIX_C_SOURCE 200809L

#include "hal_run.h"

#include <time.h>

void hal_run(hal_tick_fn tick, void *userdata)
{
    while(tick(userdata)) {}
}

void hal_idle(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
