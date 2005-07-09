/* Common functions.
   Copyright (C) 2005 FIXME */

#ifndef LIB_H__
#define LIB_H__

#include <config.h>

#include <stdio.h>
#include <stdint.h>

#ifdef ENABLE_NLS
#include <libintl.h>
#else
#define gettext(X) (X)
#endif

#define _(X) gettext(X)

#ifdef __GNUC__
#define attribute__(X) __attribute__(X)
#else
#define attribute__(X)
#endif

/* Convert VAL to big endian */
extern uint64_t htonll(uint64_t val);

/* Report message FMT and potentially ERR if non-zero */
extern void error (int err, const char *fmt, ...);

/* Report message FMT and potentially ERR if non-zero and
   exit (EXIT_FAILURE) */
extern void fatal (int err, const char *fmt, ...) attribute__ ((noreturn));

/* Report read error or unexpected EOF STREAM for FILENAME, using ERR */
extern void read_error (const char *filename, FILE *stream, int err);

/* Allocate SIZE bytes, terminate on failure */
extern void *xmalloc (size_t size);

/* A buffer which grows on demand */
struct growbuf
{
  void *p;
  size_t len;
};

/* Make sure GB is at least LEN bytes large */
extern void gb_alloc (struct growbuf *gb, size_t len);

/* Open FILENAME, report error on failure.
   Return open database or NULL on error. */
extern FILE *open_db (const char *filename);

#endif
