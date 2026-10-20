include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_tarantool

mod_LTLIBRARIES = mod_tarantool.la
mod_tarantool_la_SOURCES = mod_tarantool.c \
	tnt_client.c tnt_client.h \
	tnt_sql.c tnt_sql.h \
	tnt_rules.c tnt_rules.h \
	tnt_stats.c tnt_stats.h \
	tnt_tcache.c tnt_tcache.h \
	circuit_breaker.c circuit_breaker.h \
	mp_decode.c mp_decode.h \
	switch_uuidv7_pvt.h

# -Wno-error=format-truncation: gcc >= 8 flags snprintf() length-guards as
# format-truncation; FreeSWITCH's -Werror would fail the build (esp. 1.10)
mod_tarantool_la_CFLAGS  = $(AM_CFLAGS) -std=gnu99 -D_GNU_SOURCE -Wno-error=pedantic -Wno-error=format-truncation
mod_tarantool_la_LIBADD  = $(switch_builddir)/libfreeswitch.la
mod_tarantool_la_LDFLAGS = -avoid-version -module -no-undefined -shared

EXTRA_DIST = conf/tarantool.conf.xml README.md README_ru.md switch_uuidv7_pvt.h build.sh

confdir=$(sysconfdir)/freeswitch/autoload_configs

install-data-local:
	$(INSTALL) -d $(DESTDIR)$(confdir)
	$(INSTALL) -m 644 $(srcdir)/conf/tarantool.conf.xml $(DESTDIR)$(confdir)