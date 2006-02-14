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
dbfile = $(dbdir)/mlocate.db
groupname = mlocate
localedir = $(datadir)/locale
updatedb_conf = $(sysconfdir)/updatedb.conf
ACLOCAL_AMFLAGS = -I m4
COMMON_CPPFLAGS = -DDBFILE='"$(dbfile)"' -DLOCALEDIR='"$(localedir)"' \
	-DGROUPNAME='"$(groupname)"'
CONF_CPPFLAGS = -DUPDATEDB_CONF='"$(updatedb_conf)"'

## Targets
SUBDIRS = po

bin_PROGRAMS = src/locate src/updatedb
dist_man_MANS = doc/mlocate.db.5
man_MANS = doc/locate.1 doc/updatedb.conf.5 doc/updatedb.8

noinst_LIBRARIES = src/liblib.a

## Rules
CLEANFILES = $(man_MANS)
EXTRA_DIST = doc/locate.1.in doc/updatedb.conf.5.in doc/updatedb.8.in

src_liblib_a_SOURCES = src/db.h src/lib.c src/lib.h

src_locate_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS)
src_locate_LDADD = src/liblib.a $(LIBINTL)

src_updatedb_SOURCES = src/conf.c src/conf.h src/updatedb.c
src_updatedb_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS) $(CONF_CPPFLAGS)
src_updatedb_LDADD = src/liblib.a $(LIBINTL)

doc/locate.1: $(srcdir)/doc/locate.1.in Makefile
	$(mkdir_p) doc
	sed 's,@dbfile@,$(dbfile),g' < $(srcdir)/doc/locate.1.in > $@

doc/updatedb.conf.5: $(srcdir)/doc/updatedb.conf.5.in Makefile
	$(mkdir_p) doc
	sed 's,@updatedb_conf@,$(updatedb_conf),g' \
		< $(srcdir)/doc/updatedb.conf.5.in > $@

doc/updatedb.8: $(srcdir)/doc/updatedb.8.in Makefile
	$(mkdir_p) doc
	sed -e 's,@dbfile@,$(dbfile),g; s,@groupname@,$(groupname),g' \
		-e 's,@updatedb_conf@,$(updatedb_conf),g' \
		< $(srcdir)/doc/updatedb.8.in > $@

install-exec-local:
	$(mkdir_p) "$(DESTDIR)$(dbdir)"
	-chgrp $(groupname) "$(DESTDIR)$(dbdir)" 2>/dev/null \
		&& chmod g=rx,o= "$(DESTDIR)$(dbdir)"

install-exec-hook:
	-chgrp $(groupname) "$(DESTDIR)$(bindir)/locate" 2>/dev/null \
		&& chmod g+s,go-w "$(DESTDIR)$(bindir)/locate"
