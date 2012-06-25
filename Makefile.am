# Copyright (C) 2005, 2006, 2007 Red Hat, Inc. All rights reserved.
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
updatedb_conf = $(sysconfdir)/updatedb.conf
# autopoint puts its m4 files to the first directory
ACLOCAL_AMFLAGS = -I m4 -I gnulib/m4
AM_CPPFLAGS = -I$(top_srcdir)/gnulib/lib -I$(top_builddir)/gnulib/lib
COMMON_CPPFLAGS = -DDBFILE='"$(dbfile)"' -DLOCALEDIR='"$(localedir)"' \
	-DGROUPNAME='"$(groupname)"'
CONF_CPPFLAGS = -DUPDATEDB_CONF='"$(updatedb_conf)"'

## Targets
SUBDIRS = gnulib/lib po

bin_PROGRAMS = src/locate src/updatedb
dist_man_MANS = doc/mlocate.db.5
man_MANS = doc/locate.1 doc/updatedb.conf.5 doc/updatedb.8

noinst_LIBRARIES = src/liblib.a

check_PROGRAMS = tests/bind-mount-helper

## Rules
CLEANFILES = $(man_MANS)
DISTCLEANFILES = atconfig
EXTRA_DIST = doc/locate.1.in doc/updatedb.conf.5.in doc/updatedb.8.in \
	tests/testsuite tests/package.m4 tests/testsuite.at $(TESTFILES)
TESTFILES = tests/bind-mount.at tests/config.at tests/locate.at \
	tests/updatedb.at

src_liblib_a_SOURCES = src/bind-mount.c src/bind-mount.h src/db.h \
	src/lib.c src/lib.h

src_locate_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS)
src_locate_LDADD = src/liblib.a gnulib/lib/libgnu.a $(LIBINTL)

src_updatedb_SOURCES = src/conf.c src/conf.h src/updatedb.c
src_updatedb_CPPFLAGS = $(AM_CPPFLAGS) $(COMMON_CPPFLAGS) $(CONF_CPPFLAGS)
src_updatedb_LDADD = src/liblib.a gnulib/lib/libgnu.a $(LIBINTL)

tests_bind_mount_helper_LDADD = src/liblib.a gnulib/lib/libgnu.a $(LIBINTL)

doc/locate.1: $(srcdir)/doc/locate.1.in Makefile
	$(MKDIR_P) doc
	sed 's,@dbfile@,$(dbfile),g' < $(srcdir)/doc/locate.1.in > $@

doc/updatedb.conf.5: $(srcdir)/doc/updatedb.conf.5.in Makefile
	$(MKDIR_P) doc
	sed 's,@updatedb_conf@,$(updatedb_conf),g' \
		< $(srcdir)/doc/updatedb.conf.5.in > $@

doc/updatedb.8: $(srcdir)/doc/updatedb.8.in Makefile
	$(MKDIR_P) doc
	sed -e 's,@dbfile@,$(dbfile),g; s,@groupname@,$(groupname),g' \
		-e 's,@updatedb_conf@,$(updatedb_conf),g' \
		< $(srcdir)/doc/updatedb.8.in > $@

clean-local:
	$(SHELL) $(top_srcdir)/tests/testsuite --clean

check-local: atconfig $(top_srcdir)/tests/testsuite
	$(SHELL) $(top_srcdir)/tests/testsuite

installcheck-local: atconfig $(top_srcdir)/tests/testsuite
	$(SHELL) $(top_srcdir)/tests/testsuite AUTOTEST_PATH=$(bindir)

install-exec-local:
	$(MKDIR_P) "$(DESTDIR)$(dbdir)"
	-chgrp $(groupname) "$(DESTDIR)$(dbdir)" 2>/dev/null \
		&& chmod g=rx,o= "$(DESTDIR)$(dbdir)"

install-exec-hook:
	-chgrp $(groupname) "$(DESTDIR)$(bindir)/locate" 2>/dev/null \
		&& chmod g+s,go-w "$(DESTDIR)$(bindir)/locate"

$(top_srcdir)/tests/package.m4: $(top_srcdir)/configure.ac
	{								   \
	  echo '# Signature of the current package.';			   \
	  echo 'm4_define([AT_PACKAGE_NAME],      [@PACKAGE_NAME@])';	   \
	  echo 'm4_define([AT_PACKAGE_TARNAME],   [@PACKAGE_TARNAME@])';   \
	  echo 'm4_define([AT_PACKAGE_VERSION],   [@PACKAGE_VERSION@])';   \
	  echo 'm4_define([AT_PACKAGE_STRING],    [@PACKAGE_STRING@])';	   \
	  echo 'm4_define([AT_PACKAGE_BUGREPORT], [@PACKAGE_BUGREPORT@])'; \
	  echo 'm4_define([AT_PACKAGE_URL],       [@PACKAGE_URL@])';	   \
	} >$(top_srcdir)/tests/package.m4

$(top_srcdir)/tests/testsuite: tests/package.m4 tests/testsuite.at $(TESTFILES)
	$(AUTOM4TE) --language=autotest -I $(top_srcdir)/tests \
		$(top_srcdir)/tests/testsuite.at -o $@.tmp
	mv $@.tmp $@

atconfig: $(top_builddir)/config.status
	cd $(top_builddir) && $(SHELL) ./config.status ./atconfig
