# Process this file with autoconf to produce a configure script.
AC_INIT([mlocate], [0.03], [mitr@redhat.com])
AC_COPYRIGHT(
[Copyright (C) 2005 FIXME])
AC_PREREQ(2.59)
AC_CONFIG_HEADERS([src/config.h])	
AC_CONFIG_SRCDIR([src/locate.c])
AC_CONFIG_AUX_DIR([admin])
AM_INIT_AUTOMAKE([no-define subdir-objects -Wall])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_RANLIB

# Checks for libraries.
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.14.3])

# Checks for header files.

# Checks for types.

# Checks for structures.
AC_CHECK_MEMBERS([struct dirent.d_type], , , [#include <dirent.h>])

# Checks for compiler characteristics.

# Checks for library functions.
AC_FUNC_OBSTACK

# Checks for system services.
AC_SYS_LARGEFILE

# Miscellanous hackery.

# Modify CFLAGS after all tests are run (some of them would fail because
# of the -Werror).
if test "$GCC" = yes ; then
    AC_CACHE_CHECK([for available GCC warning flags],
		   [mitr_cv_prog_gcc_warnings],
    [mitr_cv_prog_gcc_warnings=""
    for i in -Wall -W -Wcast-align -Wcast-qual -Wmissing-noreturn dnl
-Wnested-externs -Wpointer-arith -Wshadow -Wstrict-prototypes -Wundef dnl
-Wwrite-strings; do
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

AC_CONFIG_FILES([Makefile po/Makefile.in])
AC_OUTPUT
