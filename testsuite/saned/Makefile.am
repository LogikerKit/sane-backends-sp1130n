##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2026  Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

SANED     = ../../frontend/saned$(EXEEXT)
SCANIMAGE = ../../frontend/scanimage$(EXEEXT)
DEVICE    = net:localhost:port:test

EXTRA_DIST = README saned.sh

if COMPILE_SANED
check_PROGRAMS = saned_test
TESTS = saned.sh $(check_PROGRAMS)

saned_test_SOURCES = saned_test.c
saned_test_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir) -I$(top_srcdir)/include \
	-I$(top_builddir)/include $(AVAHI_CFLAGS)
saned_test_LDADD = ../../sanei/libsanei.la ../../lib/liblib.la \
	$(SYSLOG_LIBS) $(SYSTEMD_LIBS) $(AVAHI_LIBS)
endif
all: help

help:
	@echo "Use 'make test' to run the tests."

test: test.saned

test.saned:
	@echo "**** Testing $(SCANIMAGE) with device $(DEVICE) via $(SANED)"
	@if $(srcdir)/saned.sh ; \
	then echo ; echo "**** saned test passed" ; \
	else echo ; echo "**** Something failed (maybe saned or the net or test backend not built?)" ; \
	exit 1 ; \
	fi
