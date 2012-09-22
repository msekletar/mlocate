# Process this file with autoconf to produce a configure script.
AC_INIT([mlocate], [0.26], [mitr@redhat.com], ,
        [https://fedorahosted.org/mlocate/])
AC_COPYRIGHT(
[Copyright (C) 2005, 2006, 2007, 2009, 2010, 2012 Red Hat, Inc. All rights
reserved.
This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmac <mitr@redhat.com>])
AC_PREREQ([2.63b])
AC_CONFIG_HEADERS([src/config.h])
AC_CONFIG_SRCDIR([src/locate.c])
AC_CONFIG_AUX_DIR([admin])
AC_CONFIG_TESTDIR([.], [src:tests])
AM_INIT_AUTOMAKE([no-dist-gzip dist-xz no-define subdir-objects -Wall])

# Checks for programs.
AC_PROG_CC
gl_EARLY
AM_PROG_CC_C_O
AC_PROG_RANLIB
# This interface seems to be undocumented, but bison uses it ;-)
AM_MISSING_PROG([AUTOM4TE], [autom4te])

gl_INIT

# Checks for libraries.
AM_GNU_GETTEXT([external], [need-ngettext])
AM_GNU_GETTEXT_VERSION([0.17])

# Checks for header files.

# Checks for types.

# Checks for structures.

# Checks for compiler characteristics.

# Checks for library functions.
## getopt_long () availability should be checked here
AC_CHECK_FUNCS_ONCE([fdopendir])
AC_FUNC_GETMNTENT

# Checks for system services.
AC_SYS_LARGEFILE

AC_CACHE_CHECK([for mounted filesystem table in /proc],
	       [mitr_cv_sys_proc_mounts],
[mitr_cv_sys_proc_mounts=no
if test "$cross_compiling" != yes; then
    for i in /proc/self/mounts /proc/mounts; do
	if test -f "$i"; then
	    mitr_cv_sys_proc_mounts=$i
	    break
	fi
    done
fi])
if test "$mitr_cv_sys_proc_mounts" != no; then
    AC_DEFINE_UNQUOTED([PROC_MOUNTS_PATH], ["$mitr_cv_sys_proc_mounts"],
		       [Define to the path to the mounted filesystem table.])
fi

# Miscellanous hackery.

# Modify CFLAGS after all tests are run (some of them would fail because
# of the -Werror).
if test "$GCC" = yes ; then
    AC_CACHE_CHECK([for available GCC warning flags],
		   [mitr_cv_prog_gcc_warnings],
    [mitr_cv_prog_gcc_warnings=""
    for i in -Wall -W -Wcast-align -Wmissing-noreturn -Wpointer-arith dnl
 -Wshadow -Wstrict-prototypes -Wwrite-strings; do
        oldcflags=$CFLAGS
        CFLAGS="$CFLAGS $i"
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
		[mitr_cv_prog_gcc_warnings="$mitr_cv_prog_gcc_warnings $i"])
	CFLAGS=$oldcflags
     done])
     CFLAGS="$CFLAGS $mitr_cv_prog_gcc_warnings"
     AC_MSG_CHECKING([whether we should use -Werror])
     AC_ARG_ENABLE([Werror],
		   AS_HELP_STRING([--enable-Werror],
			[use -Werror if compiling with gcc (default is NO)]),
		   [], enable_Werror=no)
     if test "x$enable_Werror" != xno; then
	CFLAGS="$CFLAGS -Werror"
     fi
     AC_MSG_RESULT([$enable_Werror])
fi

AC_CONFIG_FILES([Makefile gnulib/lib/Makefile po/Makefile.in])
AC_OUTPUT
