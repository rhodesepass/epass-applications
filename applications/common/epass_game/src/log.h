#ifndef EPASS_GAME_LOG_H
#define EPASS_GAME_LOG_H

void epass_log(const char *level, const char *file, int line,
               const char *format, ...);

#define log_info(...) epass_log("I", __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) epass_log("W", __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) epass_log("E", __FILE__, __LINE__, __VA_ARGS__)

#endif
