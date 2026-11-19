/* Minimal FreeSWITCH switch.h stub for the standalone unit tests.
 * Provides just enough surface for tnt_rules.c / tnt_sql.c to compile
 * outside a FreeSWITCH tree. NOT part of the module itself. */
#ifndef SWITCH_H_STUB
#define SWITCH_H_STUB

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWITCH_DECLARE(type) type
#define SWITCH_DECLARE_NONSTD(type) type

typedef int switch_status_t;
#define SWITCH_STATUS_SUCCESS 0
#define SWITCH_STATUS_FALSE 1
#define SWITCH_STATUS_TERM 2
#define SWITCH_STATUS_GENERR 3

typedef int switch_log_level_t;
#define SWITCH_LOG_DEBUG 0
#define SWITCH_LOG_INFO 1
#define SWITCH_LOG_NOTICE 2
#define SWITCH_LOG_WARNING 3
#define SWITCH_LOG_ERROR 4
#define SWITCH_LOG_CRIT 5
#define SWITCH_CHANNEL_LOG 0
#define SWITCH_CHANNEL_LOG_CLEAN 0
#define SWITCH_CHANNEL_SESSION_LOG(x) 0

/* the tests capture log output to inspect diagnostics */
void tnt_test_log_capture(int level, const char *line);

static inline int switch_log_printf(int channel, int level, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	int rc;

	(void)channel;
	va_start(ap, fmt);
	rc = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	tnt_test_log_capture(level, buf);
	return rc;
}

typedef int switch_bool_t;
typedef int switch_mutex_t;

#endif /* SWITCH_H_STUB */
