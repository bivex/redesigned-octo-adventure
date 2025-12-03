ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS} -I m4
AM_CFLAGS = -std=gnu11 -g -O3 \
	-I./src -I/usr/include/postgresql -I$(top_srcdir)/../libreactor/src -I./src/reactor_postgres -I/var/www/rads/third_party/criterion-2.4.3/include \
	-Wall -Wextra -Wpedantic -DDEBUG_MODE
AM_LDFLAGS = -L/var/www/rads/third_party/criterion-2.4.3/lib -Wl,-rpath=/var/www/rads/third_party/criterion-2.4.3/lib
AM_LIBS =

EXTRA_DIST = \
  CHANGES \
  LICENSE \
  README.md

SOURCE_FILES = \
src/reactor_postgres/reactor_postgres.c \
src/reactor_postgres/reactor_postgres_client.c

HEADER_FILES = \
src/reactor_postgres/reactor_postgres.h \
src/reactor_postgres/reactor_postgres_client.h

AUTOMAKE_OPTIONS = subdir-objects
lib_LTLIBRARIES = libreactor_postgres.la
libreactor_postgres_la_SOURCES = $(SOURCE_FILES) $(HEADER_FILES)

headerfilesdir = $(includedir)/reactor_postgres
headerfiles_HEADERS = $(HEADER_FILES)

mainheaderdir = $(includedir)
mainheader_HEADERS = src/reactor_postgres.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libreactor_postgres.pc

## examples ###

bin_PROGRAMS = \
  example/query_lowlevel \
  example/queries \
  test/test_client \
  bench/benchmark_writes \
  bench/benchmark_simple \
  bench/test_close

example_query_lowlevel_SOURCES = example/query_lowlevel.c
example_query_lowlevel_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl
example_queries_SOURCES = example/queries.c
example_queries_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl
test_test_client_SOURCES = test/test_client.c
test_test_client_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lcriterion -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl
bench_benchmark_writes_SOURCES = bench/benchmark_writes.c
bench_benchmark_writes_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl
bench_benchmark_simple_SOURCES = bench/benchmark_simple.c
bench_benchmark_simple_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl
bench_test_close_SOURCES = bench/test_close.c
bench_test_close_LDADD = libreactor_postgres.la ../libreactor/libreactor.la -lclo -lpq -lssl -lcrypto -lldap -lgssapi_krb5 -lsasl2 -lpthread -lrt -ldl

MAINTAINERCLEANFILES = aclocal.m4 config.h.in configure Makefile.in docs/Makefile.in libreactor_postgres-?.?.?.tar.gz
maintainer-clean-local:; rm -rf autotools m4 libreactor_postgres-?.?.?

CLEANFILES = {.,src/reactor_postgres}/*.{gcno,gcda,gcov}
