AM_CFLAGS = $(VARNISHAPI_CFLAGS) -Wall -Werror
AM_LDFLAGS = $(VARNISHAPI_LIBS) $(VMOD_LDFLAGS)

vmod_LTLIBRARIES = libvmod_prometheus.la

libvmod_prometheus_la_SOURCES = \
	vmod_prometheus.c

nodist_libvmod_prometheus_la_SOURCES = \
	vcc_prometheus_if.c \
	vcc_prometheus_if.h

dist_man_MANS = vmod_prometheus.3

@BUILD_VMOD_PROMETHEUS@

EXTRA_DIST = \
	vmod_prometheus.vcc 
