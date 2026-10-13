#ifndef CONSOLE_H
#define CONSOLE_H

void console_printf(const char *str, ...);
void log_info(const char *message);
void log_warning(const char *message);
void log_error(const char *message);
void log_fatal(const char *message);

#endif
