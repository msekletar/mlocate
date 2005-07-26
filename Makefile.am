# Copyright (C) 2005 Red Hat, Inc. All rights reserved.
# This copyrighted material is made available to anyone wishing to use, modify,
# copy, or redistribute it subject to the terms and conditions of the GNU
# General Public License v.2.

# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.

# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

# Author: Miloslav Trmac <mitr@redhat.com>

## Process this file with automake to produce Makefile.in

## Settings
dbdir = $(localstatedir)/mlocate
localedir = $(datadir)/locale
ACLOCAL_AMFLAGS = -I m4
COMMON_CPPFLAGS = -DDBFILE='"$(dbdir)/mlocate.db"' \
	-DLOCALEDIR='"$(localedir)"' -DGROUPNAME='"mlocate"'
CONF_CPPFLAGS = -DUPDATEDB_CONF='"$(sysconfdir)/updatedb.conf"'

## Targets
SUBDIRS = po

bin_PROGRAMS = src/locate
libexec_PROGRAMS = src/updatedb

noinst_LIBRARIES = src/liblib.a

## Rules
src_locate_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS)
src_locate_LDADD = src/liblib.a $(LIBINTL)

src_updatedb_SOURCES = src/conf.c src/conf.h src/updatedb.c
src_updatedb_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS) $(CONF_CPPFLAGS)
src_updatedb_LDADD = src/liblib.a $(LIBINTL)

## Dependency data
src_liblib_a_SOURCES = src/db.h src/lib.c src/lib.h
