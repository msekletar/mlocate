/* Common functions.

Copyright (C) 2005 Red Hat, Inc. All rights reserved.
This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmac <mitr@redhat.com> */

#ifndef LIB_H__
#define LIB_H__

#include <config.h>

#include <stdio.h>
#include <stdint.h>

#include <obstack.h>

#ifdef ENABLE_NLS
#include <libintl.h>
#else
#define gettext(X) (X)
#endif

#define _(X) gettext(X)

/* Number of entries in ARRAY */
#define ARRAY_SIZE(array) (sizeof (array) / sizeof (*array))

/* Convert VAL to big endian */
extern uint64_t htonll (uint64_t val);

/* Convert VAL from big endian */
extern uint64_t ntohll (uint64_t val);

/* Initialize dir_path_cmp_table */
extern void dir_path_cmp_init (void);

/* Compare two path names using the database directory order. This is not
   exactly strcmp () order: "a" < "a.b", so "a/z" < "a.b". */
extern int dir_path_cmp (const char *a, const char *b);

/* Report read error or unexpected EOF STREAM for FILENAME, using ERR */
extern void read_error (const char *filename, FILE *stream, int err);

/* Allocate SIZE bytes, terminate on failure */
extern void *xmalloc (size_t size);

/* Functions used by obstack code */
extern struct _obstack_chunk *obstack_chunk_alloc (long size);

#define obstack_chunk_free free

/* The obstack code is too portable to be perfect :(. So we need some
   better substitutes to avoid 32-bit overflows. */
#define OBSTACK_OBJECT_SIZE(H) \
  (size_t)((char *)obstack_next_free (H) - (char *)obstack_base (H))

/* To avoid obstack integer overflows, limit large obstack allocations to this
   value. */
enum { OBSTACK_SIZE_MAX = 1024 * 1024 };

/* Open FILENAME, report error on failure if not QUIET.  Store database
   visibility check flag to *CHECK_VISIBLITY;
   Return open database or NULL on error. */
extern FILE *open_db (const char *filename, _Bool *check_visibility,
		      _Bool quiet);

/* Read a NUL-terminated string from DATABASE FILE to current object
   in OBSTACK (without the terminating NULL), report error on failure if not
   QUIET.
   Return 0 if OK, or -1 on I/O error. */
extern int read_name (struct obstack *h, const char *database, FILE *file,
		      _Bool quiet);

#endif
