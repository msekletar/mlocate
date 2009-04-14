/* locate(1).

Copyright (C) 2005, 2007 Red Hat, Inc. All rights reserved.
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
#include <config.h>

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <getopt.h>
#include "error.h"
#include "fnmatch.h"
#include "fwriteerror.h"
#include "obstack.h"
#include "progname.h"
#include "xalloc.h"

#include "db.h"
#include "lib.h"

/* Check file existence before reporting them */
static bool conf_check_existence; /* = false; */

/* Follow trailing symlinks when checking for existence.  The default (and
   probably the existence of the option) looks like a historical accident. */
static bool conf_check_follow_trailing = true;

/* Databases, "-" is stdin */
static struct string_list conf_dbpath; /* = { 0, }; */

/* Ignore case when matching patterns */
static bool conf_ignore_case; /* = false; */

/* Match only the basename against patterns */
static bool conf_match_basename; /* = false; */

/* Patterns are regexps */
static bool conf_match_regexp; /* = false; */

/* Patterns are BREs */
static bool conf_match_regexp_basic; /* = false; */

/* Output match count only */
static bool conf_output_count; /* = false; */

/* Output limit */
static uintmax_t conf_output_limit;
static bool conf_output_limit_set; /* = false; */

/* Quote nonprintable characters on output */
static bool conf_output_quote; /* = false; */

/* Character for output separation */
static char conf_output_separator = '\n';

/* Patterns to search for */
static struct string_list conf_patterns; /* = { 0, }; */

/* If conf_match_regexp, compiled patterns to search for */
static regex_t *conf_regex_patterns;

/* If !conf_match_regexp, true if the pattern contains no characters recognized
   by fnmatch () as special */
static bool *conf_patterns_simple;

/* If !conf_match_regexp, there is at least one simple pattern */
static bool conf_have_simple_pattern; /* = false; */

/* If conf_have_simple_pattern && conf_ignore_case, patterns to search for, in
   uppercase */
static wchar_t **conf_uppercase_patterns;

/* Don't report errors about databases */
static bool conf_quiet; /* = false; */

/* Output only statistics */
static bool conf_statistics; /* = false; */

 /* String utilities */

/* Convert SRC to upper-case wide string in OBSTACK;
   return result */
static wchar_t *
uppercase_string (struct obstack *obstack, const char *src)
{
  size_t left, wchars;
  wchar_t *res, *p;

  left = strlen (src) + 1;
  /* Optimistically assume the string is OK and will fit.  This may allocate
     a bit more memory than necessary, but the conversion is slow enough that
     computing the exact size is not worth it. */
  res = obstack_alloc (obstack, left * sizeof (*res));
  wchars = mbstowcs (res, src, left);
  if (wchars != (size_t)-1)
    assert (wchars < left);
  else
    {
      mbstate_t state;
      wchar_t wc;

      obstack_free (obstack, res);
      /* The slow path.  obstack design makes it hard to preallocate space for
	 mbsrtowcs (), so we would have to copy the wide string to use
	 an universal loop using mbsrtowcs ().  Using mbstowcs () as a fast
	 path is simpler. */
      memset (&state, 0, sizeof (state));
      do
	{
	  size_t size;

	  size = mbrtowc (&wc, src, left, &state);
	  if (size == 0)
	    size = 1;
	  else if (size >= (size_t)-2)
	    {
	      size = 1;
	      wc = (unsigned char)*src;
	      memset (&state, 0, sizeof (state));
	    }
	  src += size;
	  assert (left >= size);
	  left -= size;
	  obstack_grow (obstack, &wc, sizeof (wc));
	}
      while (wc != 0);
      res = obstack_finish (obstack);
    }
  for (p = res; *p != 0; p++)
    *p = towupper (*p);
  return res;
}

/* Write STRING to stdout, replace unprintable characters with '?' */
static void
write_quoted (const char *string)
{
  mbstate_t state;
  const char *last; /* Start of the current batch of bytes for fwrite () */
  size_t left;

  left = strlen (string);
  memset (&state, 0, sizeof (state));
  last = string;
  while (left != 0)
    {
      size_t size;
      wchar_t wc;
      bool printable;

      size = mbrtowc (&wc, string, left, &state);
      if (size == 0)
	break;
      if (size < (size_t)-2)
	printable = iswprint (wc);
      else if (size == (size_t)-1)
	{
	  size = 1;
	  memset (&state, 0, sizeof (state));
	  printable = false;
	}
      else
	{
	  assert (size == (size_t)-2);
	  size = left;
	  printable = false;
	}
      if (printable == false)
	{
	  if (string != last)
	    fwrite (last, 1, string - last, stdout);
	  putchar ('?');
	}
      string += size;
      assert (left >= size);
      left -= size;
      if (printable == false)
	last = string;
    }
  if (string != last)
    fwrite (last, 1, string - last, stdout);
}

 /* Access permission checking */

/* The cache is a simple stack of paths, each path longer than the previous
   one.  This allows calling access() for each object only once (actually, only
   the R_OK | X_OK acess () calls are cached; the R_OK calls are not). */
struct check_entry
{
  struct check_entry *next;
  size_t len;
  bool allowed;
  char path[];			/* _Not_ NUL-terminated */
};

/* Contains the check_entry stack */
static struct obstack check_stack_obstack;

/* Return (possibly cached) result of access (PATH, R_OK | X_OK).  Note that
   errno is not set. */
static int
cached_access_rx (const char *path)
{
  static struct check_entry *check_stack; /* = NULL; */

  size_t len;
  struct check_entry *e, *to_free, *new;
  
  len = strlen (path);
  to_free = NULL;
  for (e = check_stack; e != NULL; e = e->next)
    {
      if (e->len < len)
	break;
      to_free = e;
      if (e->len == len && memcmp (e->path, path, len) == 0)
	goto found;
    }
  if (to_free != NULL)
    obstack_free (&check_stack_obstack, to_free);
  new = obstack_alloc (&check_stack_obstack,
		       offsetof (struct check_entry, path) + len);
  new->next = e;
  new->len = len;
  new->allowed = access (path, R_OK | X_OK) == 0;
  memcpy (new->path, path, len);
  check_stack = new;
  e = new;
 found:
  return e->allowed != false ? 0 : -1;
}

/* Check permissions of parent directory of PATH; it should be accessible and
   readable.
   Return 0 if OK, -1 on error */
static int
check_directory_perms (const char *path)
{
  static char *copy; /* = NULL; */
  static size_t copy_size; /* = 0; */

  size_t size;
  char *p, *slash, *last_slash;
  int res;

  res = -1;
  size = strlen (path) + 1;
  assert (size > 1);
  while (size > copy_size)
    copy = x2realloc (copy, &copy_size);
  memcpy (copy, path, size);
  last_slash = strrchr (copy, '/');
  assert (last_slash != NULL);
  if (last_slash == copy) /* "/" was checked in main () */
    {
      res = 0;
      goto err;
    }
  for (p = copy + 1; (slash = strchr (p, '/')) != last_slash; p = slash + 1)
    {
      char old;

      old = *slash;
      *slash = 0;
      if (cached_access_rx (copy) != 0)
	goto err;
      *slash = old;
    }
  *last_slash = 0;
  /* r-- directories are probably very uncommon, so we try R_OK | X_OK (which
     pre-populates the cache if PATH has subdirectories) first.  This is a
     heuristic that can in theory almost double the number of access () calls,
     in practice it reduces the number of access () calls by about 25 %.  The
     asymptotical number of calls stays the same ;-) */
  if (cached_access_rx (copy) != 0 && access (copy, R_OK) != 0)
    goto err;
  res = 0;
 err:
  return res;
}

 /* Statistics */

/* Statistics of the current database */
static uintmax_t stats_directories;
static uintmax_t stats_entries;
static uintmax_t stats_bytes;

/* Clear current statistics */
static void
stats_clear (void)
{
  stats_directories = 0;
  stats_entries = 0;
  stats_bytes = 0;
}

/* Print current statistics for DB */
static void
stats_print (const struct db *db)
{
  uintmax_t sz;
  
  sz = db_bytes_read (db);
  printf (_("Database %s:\n"), db->filename);
  /* The third argument of ngettext () is unsigned long; it is still better
     to have invalid grammar than truncated numbers. */
  printf (ngettext ("\t%'ju directory\n", "\t%'ju directories\n",
		    stats_directories), stats_directories);
  printf (ngettext ("\t%'ju file\n", "\t%'ju files\n", stats_entries),
	  stats_entries);
  printf (ngettext ("\t%'ju byte in file names\n",
		    "\t%'ju bytes in file names\n", stats_bytes), stats_bytes);
  printf (ngettext ("\t%'ju byte used to store database\n",
		    "\t%'ju bytes used to store database\n", sz), sz);
}

 /* Database search */

/* Number of matches so far */
static uintmax_t matches_found; /* = 0; */

/* Contains a single, usually not obstack_finish ()'ed object */
static struct obstack path_obstack;

/* Contains a single object */
static struct obstack uc_obstack;
/* .. after this zero-length marker */
static void *uc_obstack_mark;

/* Does STRING match one of conf_patterns? */
static bool
string_matches_pattern (const char *string)
{
  size_t i;
  wchar_t *wstring;
  bool matched;

  if (conf_match_regexp == false && conf_ignore_case != false
      && conf_have_simple_pattern != false)
    {
      obstack_free (&uc_obstack, uc_obstack_mark);
      wstring = uppercase_string (&uc_obstack, string);
      uc_obstack_mark = wstring;
    }
  else
    wstring = NULL;
  matched = false;
  for (i = 0; i < conf_patterns.len; i++)
    {
      if (conf_match_regexp != false)
	matched = regexec (conf_regex_patterns + i, string, 0, NULL, 0) == 0;
      else
	{
	  if (conf_patterns_simple[i] != false)
	    {
	      if (conf_ignore_case == false)
		matched = mbsstr (string, conf_patterns.entries[i]) != NULL;
	      else
		matched = wcsstr (wstring, conf_uppercase_patterns[i]) != NULL;
	    }
	  else
	    matched = (fnmatch (conf_patterns.entries[i], string,
				conf_ignore_case != false ? FNM_CASEFOLD : 0)
		       == 0);
	}
      if (matched != false)
	break;
    }
  return matched;
}

/* PATH was found, handle it as necessary; maintain *VISIBLE: if it is -1,
   check whether the directory containing PATH is accessible and readable and
   set *VISIBLE accordingly; otherwise just use the value;
   return 0 to continue, -1 if match limit was reached */
static int
handle_path (const char *path, int *visible)
{
  const char *s, *matching;

  /* Statistics */
  if (conf_statistics != false)
    {
      stats_entries++; /* Overflow is too unlikely */
      stats_bytes += strlen (path);
      goto done;
    }
  /* Matches pattern? */
  if (conf_match_basename != false && (s = strrchr (path, '/')) != NULL)
    matching = s + 1;
  else
    matching = path;
  if (!string_matches_pattern (matching))
    goto done;
  /* Visible? */
  if (*visible == -1)
    *visible = check_directory_perms (path) == 0;
  if (*visible != 1)
    goto done;
  if (conf_check_existence != false)
    {
      struct stat st;

      if ((conf_check_follow_trailing != false ? stat : lstat) (path, &st) != 0)
	goto done;
    }
  /* Output */
  if (conf_output_count == false)
    {
      if (conf_output_quote != false)
	write_quoted (path);
      else
	fputs (path, stdout);
      putchar (conf_output_separator);
    }
  matches_found++; /* Overflow is too unlikely */
  if (conf_output_limit_set != false && matches_found == conf_output_limit)
    return -1;
 done:
  return 0;
}

/* Read and handle a directory in DB with HEADER (read just past struct
   db_directory);
   return 0 if OK, -1 on error or reached conf_output_limit

   path_obstack may contain a partial object if this function returns -1. */
static int
handle_directory (struct db *db, const struct db_header *hdr)
{
  size_t size, dir_name_len;
  int visible;
  void *p;
  
  stats_directories++;
  if (db_read_name (db, &path_obstack) != 0)
    goto err;
  size = OBSTACK_OBJECT_SIZE (&path_obstack);
  if (size == 0)
    {
      if (conf_quiet == false)
	error (0, 0, _("invalid empty directory name in `%s'"), db->filename);
      goto err;
    }
  if (size != 1 || *(char *)obstack_base (&path_obstack) != '/')
    obstack_1grow (&path_obstack, '/');
  dir_name_len = OBSTACK_OBJECT_SIZE (&path_obstack);
  visible = hdr->check_visibility ? -1 : 1;
  for (;;)
    {
      struct db_entry entry;
      
      if (db_read (db, &entry, sizeof (entry)) != 0)
	{
	  db_report_error (db);
	  goto err;
	}
      if (entry.type == DBE_END)
	break;
      if (db_read_name (db, &path_obstack) != 0)
	goto err;
      obstack_1grow (&path_obstack, 0);
      if (handle_path (obstack_base (&path_obstack), &visible) != 0)
	goto err;
      size = OBSTACK_OBJECT_SIZE (&path_obstack) - dir_name_len;
      if (size > OBSTACK_SIZE_MAX) /* No surprises, please */
	{
	  if (conf_quiet == false)
	    error (0, 0, _("file name length %zu in `%s' is too large"), size,
		   db->filename);
	  goto err;
	}
      obstack_blank (&path_obstack, -(ssize_t)size);
    }
  p = obstack_finish (&path_obstack);
  obstack_free (&path_obstack, p);
  return 0;

 err:
  return -1;
}

/* Read and handle DATABASE, opened as FD;
   PRIVILEGED is non-zero if db_is_privileged() */
static void
handle_db (int fd, const char *database, bool privileged)
{
  struct db db;
  struct db_header hdr;
  struct db_directory dir;
  void *p;
  int visible;

  if (db_open (&db, &hdr, fd, database, conf_quiet) != 0)
    {
      close(fd);
      goto err;
    }
  stats_clear ();
  if (db_read_name (&db, &path_obstack) != 0)
    goto err_path;
  obstack_1grow (&path_obstack, 0);
  if (privileged == false)
    hdr.check_visibility = 0;
  visible = hdr.check_visibility ? -1 : 1;
  p = obstack_finish (&path_obstack);
  if (handle_path (p, &visible) != 0)
    goto err_free;
  obstack_free (&path_obstack, p);
  if (db_skip (&db, ntohl (hdr.conf_size)) != 0)
    goto err_path;
  while (db_read (&db, &dir, sizeof (dir)) == 0)
    {
      if (handle_directory (&db, &hdr) != 0)
	goto err_path;
    }
  if (db.err != 0)
    {
      db_report_error (&db);
      goto err_path;
    }
  if (conf_statistics != false)
    stats_print (&db);
  /* Fall through */
 err_path:
  p = obstack_finish (&path_obstack);
 err_free:
  obstack_free (&path_obstack, p);
  db_close (&db);
 err:
  ;
}

 /* Main program */

/* GID of GROUPNAME, or (gid_t)-1 if unknown */
static gid_t privileged_gid;

/* STDIN_FILENO was already used as a database */
static bool stdin_used; /* = false; */

/* Parse DBPATH, add its entries to db_obstack */
static void
parse_dbpath (const char *dbpath)
{
  for (;;)
    {
      const char *end;
      char *entry;
      size_t len;

      end = strchrnul (dbpath, ':');
      len = end - dbpath;
      if (len == 0)
	entry = xstrdup (DBFILE);
      else
	{
	  char *copy, *p;

	  copy = xmalloc (len + 1);
	  entry = copy;
	  p = mempcpy (copy, dbpath, len);
	  *p = 0;
	}
      string_list_append (&conf_dbpath, entry);
      if (*end == 0)
	break;
      dbpath = end + 1;
    }
}

/* Output --help text */
static void
help (void)
{
  printf (_("Usage: locate [OPTION]... [PATTERN]...\n"
	    "Search for entries in a mlocate database.\n"
	    "\n"
	    "  -b, --basename         match only the base name of path names\n"
	    "  -c, --count            only print number of found entries\n"
	    "  -d, --database DBPATH  use DBPATH instead of default database "
	    "(which is\n"
	    "                         %s)\n"
	    "  -e, --existing         only print entries for currently "
	    "existing files\n"
	    "  -L, --follow           follow trailing symbolic links when "
	    "checking file\n"
	    "                         existence (default)\n"
	    "  -h, --help             print this help\n"
	    "  -i, --ignore-case      ignore case distinctions when matching "
	    "patterns\n"
	    "  -l, --limit, -n LIMIT  limit output (or counting) to LIMIT "
	    "entries\n"
	    "  -m, --mmap             ignored, for backward compatibility\n"
	    "  -P, --nofollow, -H     don't follow trailing symbolic links "
	    "when checking file\n"
	    "                         existence\n"
	    "  -0, --null             separate entries with NUL on output\n"
	    "  -S, --statistics       don't search for entries, print "
	    "statistics about each\n"
	    "                         used database\n"
	    "  -q, --quiet            report no error messages about reading "
	    "databases\n"
	    "  -r, --regexp REGEXP    search for basic regexp REGEXP instead "
	    "of patterns\n"
	    "      --regex            patterns are extended regexps\n"
	    "  -s, --stdio            ignored, for backward compatibility\n"
	    "  -V, --version          print version information\n"
	    "  -w, --wholename        match whole path name "
	    "(default)\n"), DBFILE);
  printf (_("\n"
	    "Report bugs to %s.\n"), PACKAGE_BUGREPORT);
}

/* Parse options in ARGC, ARGV.  Exit on error or --help, --version. */
static void
parse_options (int argc, char *argv[])
{
  static const struct option options[] =
    {
      { "basename", no_argument, NULL, 'b' },
      { "count", no_argument, NULL, 'c' },
      { "database", required_argument, NULL, 'd' },
      { "existing", no_argument, NULL, 'e' },
      { "follow", no_argument, NULL, 'L' },
      { "help", no_argument, NULL, 'h' },
      { "ignore-case", no_argument, NULL, 'i' },
      { "limit", required_argument, NULL, 'l' },
      { "mmap", no_argument, NULL, 'm' },
      { "quiet", no_argument, NULL, 'q' },
      { "nofollow", no_argument, NULL, 'P' },
      { "null", no_argument, NULL, '0' },
      { "regexp", required_argument, NULL, 'r' },
      { "regex", no_argument, NULL, 'R' },
      { "statistics", no_argument, NULL, 'S' },
      { "stdio", no_argument, NULL, 's' },
      { "version", no_argument, NULL, 'V' },
      { "wholename", no_argument, NULL, 'w' },
      { NULL, 0, NULL, 0 }
    };

  bool got_basename, got_follow;

  got_basename = false;
  got_follow = false;
  for (;;)
    {
      int opt, idx;

      opt = getopt_long (argc, argv, "0HPLSVbcd:ehil:mn:qr:sw", options, &idx);
      switch (opt)
	{
	case -1:
	  goto options_done;

	case '?':
	  exit (EXIT_FAILURE);

	case '0':
	  conf_output_separator = 0;
	  break;

	case 'H': case 'P':
	  if (got_follow != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "nofollow");
	  got_follow = true;
	  conf_check_follow_trailing = false;
	  break;

	case 'L':
	  if (got_follow != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "follow");
	  got_follow = true;
	  conf_check_follow_trailing = true;
	  break;

	case 'R':
	  conf_match_regexp = true;
	  break;

	case 'S':
	  conf_statistics = true;
	  break;

	case 'V':
	  puts (PACKAGE_NAME " " PACKAGE_VERSION);
	  puts (_("Copyright (C) 2007 Red Hat, Inc. All rights reserved.\n"
		  "This software is distributed under the GPL v.2.\n"
		  "\n"
		  "This program is provided with NO WARRANTY, to the extent "
		  "permitted by law."));
	  exit (EXIT_SUCCESS);

	case 'b':
	  if (got_basename != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "basename");
	  got_basename = true;
	  conf_match_basename = true;
	  break;

	case 'c':
	  conf_output_count = true;
	  break;

	case 'd':
	  parse_dbpath (optarg);
	  break;

	case 'e':
	  conf_check_existence = true;
	  break;

	case 'h':
	  help ();
	  exit (EXIT_SUCCESS);

	case 'i':
	  conf_ignore_case = true;
	  break;

	case 'l': case 'n':
	  {
	    char *end;

	    if (conf_output_limit_set != false)
	      error (EXIT_FAILURE, 0, _("--%s specified twice"), "limit");
	    conf_output_limit_set = true;
	    errno = 0;
	    conf_output_limit = strtoumax (optarg, &end, 10);
	    if (errno != 0 || *end != 0 || end == optarg
		|| isspace ((unsigned char)*optarg))
	      error (EXIT_FAILURE, 0, _("invalid value `%s' of --%s"), optarg,
		     "limit");
	    break;
	  }

	case 'm': case 's':
	  break; /* Ignore */

	case 'q':
	  conf_quiet = true;
	  break;

	case 'r':
	  conf_match_regexp = true;
	  conf_match_regexp_basic = true;
	  string_list_append (&conf_patterns, optarg);
	  break;

	case 'w':
	  if (got_basename != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "wholename");
	  got_basename = true;
	  conf_match_basename = false;
	  break;

	default:
	  abort ();
	}
    }
 options_done:
  if (conf_output_separator != 0 && isatty(STDOUT_FILENO))
    conf_output_quote = true;
  if ((conf_statistics != false || conf_match_regexp_basic != false)
      && optind != argc)
    error (EXIT_FAILURE, 0,
	   _("non-option arguments are not allowed with --%s"),
	   conf_statistics != false ? "statistics" : "regexp");
}

/* Parse arguments in ARGC, ARGV.  Exit on error. */
static void
parse_arguments (int argc, char *argv[])
{
  size_t i;

  for (i = optind; i < (size_t)argc; i++)
    string_list_append (&conf_patterns, argv[i]);
  if (conf_statistics == false && conf_patterns.len == 0)
    error (EXIT_FAILURE, 0, _("no pattern to search for specified"));
  conf_patterns.entries = xnrealloc (conf_patterns.entries, conf_patterns.len,
				     sizeof (*conf_patterns.entries));
  if (conf_match_regexp != false)
    {
      int cflags;

      conf_regex_patterns = XNMALLOC (conf_patterns.len, regex_t);
      cflags = REG_NOSUB;
      if (conf_match_regexp_basic == false) /* GNU-style */
	cflags |= REG_EXTENDED;
      if (conf_ignore_case != false)
	cflags |= REG_ICASE;
      for (i = 0; i < conf_patterns.len; i++)
	{
	  int err;

	  err = regcomp (conf_regex_patterns + i, conf_patterns.entries[i],
			 cflags);
	  if (err != 0)
	    {
	      size_t size;
	      char *msg;

	      size = regerror (err, conf_regex_patterns + i, NULL, 0);
	      msg = xmalloc (size);
	      regerror (err, conf_regex_patterns + i, msg, size);
	      error (EXIT_FAILURE, 0, _("invalid regexp `%s': %s"),
		     conf_patterns.entries[i], msg);
	    }
	}
    }
  else
    {
      conf_patterns_simple = XNMALLOC (conf_patterns.len, bool);
      for (i = 0; i < conf_patterns.len; i++)
	{
	  conf_patterns_simple[i] = strpbrk (conf_patterns.entries[i],
					     "*?[\\]") == NULL;
	  if (conf_patterns_simple[i] != false)
	    conf_have_simple_pattern = true;
	}
      if (conf_ignore_case != false && conf_have_simple_pattern != false)
	{
	  struct obstack obstack;

	  conf_uppercase_patterns = XNMALLOC (conf_patterns.len, wchar_t *);
	  obstack_init (&obstack);
	  for (i = 0; i < conf_patterns.len; i++)
	    conf_uppercase_patterns[i]
	      = uppercase_string (&obstack, conf_patterns.entries[i]);
	  /* leave the obstack allocated */
	}
    }
}

/* Does a database with ST require GROUPNAME privileges? */
static bool
db_is_privileged (const struct stat *st)
{
  return (privileged_gid != (gid_t)-1 && st->st_gid == privileged_gid
	  && (st->st_mode & (S_IRGRP | S_IROTH)) == S_IRGRP);
}

/* Set up conf_dbpath, after first entries possibly added by "-d" */
static void
finish_dbpath (void)
{
  const char *locate_path;
  char **dest_path;
  size_t src, dest;

  if (conf_dbpath.len == 0)
    string_list_append (&conf_dbpath, xstrdup (DBFILE));
  locate_path = getenv ("LOCATE_PATH");
  if (locate_path != NULL)
    parse_dbpath (locate_path);
  dest_path = XNMALLOC (conf_dbpath.len, char *);
  dest = 0;
  /* Sort databases requiring GROUPNAME privileges before the others.  This
     check is inherenly racy, but we recheck before deciding to drop
     privileges. */
  for (src = 0; src < conf_dbpath.len; src++)
    {
      struct stat st;

      if (strcmp (conf_dbpath.entries[src], "-") != 0
	  && stat (conf_dbpath.entries[src], &st) == 0
	  && db_is_privileged (&st))
	{
	  dest_path[dest] = conf_dbpath.entries[src];
	  dest++;
	  conf_dbpath.entries[src] = NULL;
	}
    }
  for (src = 0; src < conf_dbpath.len; src++)
    {
      if (conf_dbpath.entries[src] != NULL)
	{
	  dest_path[dest] = conf_dbpath.entries[src];
	  dest++;
	}
    }
  assert (dest == conf_dbpath.len);
  free (conf_dbpath.entries);
  conf_dbpath.entries = dest_path;
}

/* Drop set-group-ID privileges, if any. */
static void
drop_setgid (void)
{
  if (setgid (getgid ()) != 0)
    error (EXIT_FAILURE, errno, _("can not drop privileges"));
}

/* Handle a conf_dbpath ENTRY, drop privileges when they are no longer
   necessary. */
static void
handle_dbpath_entry (const char *entry)
{
  int fd;
  bool privileged_db;

  if (strcmp (entry, "-") == 0)
    {
      if (stdin_used != false)
	error (EXIT_FAILURE, 0,
	       _("can not read two databases from standard input"));
      stdin_used = true;
      fd = STDIN_FILENO;
      privileged_db = false;
    }
  else
    {
      struct stat st;

      if (stat (entry, &st) != 0)
	{
	  if (conf_quiet == false)
	    error (0, errno, _("can not stat () `%s'"), entry);
	  goto err;
	}
      if (!db_is_privileged (&st))
	drop_setgid();
      fd = open (entry, O_RDONLY);
      if (fd == -1)
	{
	  if (conf_quiet == false)
	    error (0, errno, _("can not open `%s'"), entry);
	  goto err;
	}
      if (fstat (fd, &st) != 0)
	{
	  if (conf_quiet == false)
	    error (0, errno, _("can not stat () `%s'"), entry);
	  close (fd);
	  goto err;
	}
      privileged_db = db_is_privileged (&st);
    }
  if (privileged_db == false)
    drop_setgid();
  handle_db (fd, entry, privileged_db); /* Closes fd */
 err:
  ;
}

int
main (int argc, char *argv[])
{
  struct group *grp;
  size_t i;
  int res;

  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE_NAME, LOCALEDIR);
  textdomain (PACKAGE_NAME);
  grp = getgrnam (GROUPNAME);
  if (grp != NULL)
    privileged_gid = grp->gr_gid;
  else
    privileged_gid = (gid_t)-1;
  parse_options (argc, argv);
  parse_arguments (argc, argv);
  finish_dbpath ();
  obstack_init (&path_obstack);
  obstack_alignment_mask (&path_obstack) = 0;
  obstack_init (&uc_obstack);
  uc_obstack_mark = obstack_alloc (&uc_obstack, 0);
  obstack_init (&check_stack_obstack);
  res = EXIT_FAILURE;
  /* Don't call access ("/", R_OK | X_OK) all the time.  This is too strict,
     it is possible to have "/" --x and have a database describing a
     subdirectory, but that is just too improbable. */
  if (conf_statistics == false && access ("/", R_OK | X_OK) != 0)
    goto done;
  for (i = 0; i < conf_dbpath.len; i++)
    {
      if (conf_output_limit_set != false && matches_found >= conf_output_limit)
	{
	  res = EXIT_SUCCESS;
	  goto done;
	}
      /* Drops privileges when possible */
      handle_dbpath_entry (conf_dbpath.entries[i]);
    }
 done:
  if (conf_output_count != false)
    printf ("%ju\n", matches_found);
  if (conf_statistics != false || matches_found != 0)
    res = EXIT_SUCCESS;
  if (fwriteerror (stdout))
    error (EXIT_FAILURE, errno,
	   _("I/O error while writing to standard output"));
  return res;
}
