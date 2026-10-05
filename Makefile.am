include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_tarantool

mod_LTLIBRARIES = mod_tarantool.la
mod_tarantool_la_SOURCES = mod_tarantool.c \
	tnt_client.c tnt_client.h \
	tnt_sql.c tnt_sql.h \
	tnt_rules.c tnt_rules.h \
	mp_decode.c mp_decode.h

mod_tarantool_la_CFLAGS  = $(AM_CFLAGS) -std=gnu99 -D_GNU_SOURCE -Wno-error=pedantic
mod_tarantool_la_LIBADD  = $(switch_builddir)/libfreeswitch.la
mod_tarantool_la_LDFLAGS = -avoid-version -module -no-undefined -shared

EXTRA_DIST = conf/tarantool.conf.xml README.md

confdir=$(sysconfdir)/freeswitch/autoload_configs

install-data-local:
	$(INSTALL) -d $(DESTDIR)$(confdir)
	$(INSTALL) -m 644 $(srcdir)/conf/tarantool.conf.xml $(DESTDIR)$(confdir)