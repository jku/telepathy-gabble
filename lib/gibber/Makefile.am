noinst_LTLIBRARIES = libgibber.la

BUILT_SOURCES = \
  gibber-signals-marshal.list \
  gibber-signals-marshal.h \
  gibber-signals-marshal.c

OUR_SOURCES =                     \
  gibber-debug.c                  \
  gibber-debug.h                  \
  gibber-transport.c              \
  gibber-transport.h              \
  gibber-fd-transport.c           \
  gibber-fd-transport.h           \
  gibber-tcp-transport.c          \
  gibber-tcp-transport.h          \
  gibber-unix-transport.c         \
  gibber-unix-transport.h         \
  gibber-linklocal-transport.c    \
  gibber-linklocal-transport.h    \
  gibber-listener.c               \
  gibber-listener.h               \
  gibber-linklocal-transport.c    \
  gibber-linklocal-transport.h    \
  gibber-sockets.c                \
  gibber-sockets.h                \
  gibber-sockets-unix.h           \
  gibber-sockets-win32.h          \
  gibber-util.h                   \
  gibber-util.c

libgibber_la_SOURCES = $(OUR_SOURCES) $(BUILT_SOURCES)

# Coding style checks
check_c_sources = \
    $(OUR_SOURCES)

include $(top_srcdir)/tools/check-coding-style.mk
check-local: check-coding-style

CLEANFILES=$(BUILT_SOURCES)
dist-hook:
	$(shell for x in $(BUILT_SOURCES); do rm -f $(distdir)/$$x ; done)

gibber-signals-marshal.list: $(OUR_SOURCES) Makefile.am
	@( cd $(srcdir) && \
	sed -n -e 's/.*_gibber_signals_marshal_\([[:upper:][:digit:]]*__[[:upper:][:digit:]_]*\).*/\1/p' \
	$(OUR_SOURCES) ) \
	| sed -e 's/__/:/' -e 'y/_/,/' | sort -u > $@.tmp
	@if cmp -s $@.tmp $@; then \
		rm $@.tmp; \
		touch $@; \
	else \
		mv $@.tmp $@; \
	fi

%-signals-marshal.h: %-signals-marshal.list Makefile.am
	$(AM_V_GEN)glib-genmarshal --header --prefix=_$(subst -,_,$*)_signals_marshal $< > $@

%-signals-marshal.c: %-signals-marshal.list Makefile.am
	$(AM_V_GEN){ echo '#include "$*-signals-marshal.h"' && \
	glib-genmarshal --body --prefix=_$(subst -,_,$*)_signals_marshal $< ; \
	} > $@


AM_CFLAGS = $(ERROR_CFLAGS) $(GCOV_CFLAGS) @GLIB_CFLAGS@ -fno-strict-aliasing

AM_LDFLAGS = $(GCOV_LIBS) @GLIB_LIBS@
