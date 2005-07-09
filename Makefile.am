# Copyright (C) 2005 FIXME
#

## Process this file with automake to produce Makefile.in

## Settings
dbdir = $(localstatedir)/mlocate
localedir = $(datadir)/locale
ACLOCAL_AMFLAGS = -I m4
DB_CPPFLAGS = -DDBFILE='"$(dbdir)/mlocate.db"'
GETTEXT_CPPFLAGS = -DLOCALEDIR='"$(localedir)"'

## Targets
SUBDIRS = po

bin_PROGRAMS = src/locate
libexec_PROGRAMS = src/updatedb

noinst_LIBRARIES = src/liblib.a

## Rules
src_locate_CPPFLAGS = $(AM_CPPFLAGS) $(DB_CPPFLAGS) $(GETTEXT_CPPFLAGS)
src_locate_LDADD = src/liblib.a

src_updatedb_CPPFLAGS = $(AM_CPPFLAGS) $(DB_CPPFLAGS) $(GETTEXT_CPPFLAGS)
src_updatedb_LDADD = src/liblib.a

## Dependency data
src_liblib_a_SOURCES = src/db.h src/lib.c src/lib.h
