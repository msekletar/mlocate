/* locate(1).

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
#include <config.h>

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <error.h>
#include <getopt.h>
#include <obstack.h>

#include "db.h"
#include "lib.h"

/* Check file existence before reporting them */
static _Bool conf_check_existence; /* = 0; */

/* Follow trailing symlinks when checking for existence.  The default (and
   probably the existence of the option) look like a historical accident. */
static _Bool conf_check_follow_trailing = 1;

/* Databases, "-" is stdin */
static char *const *conf_dbpath;
static size_t conf_dbpath_len;

/* Number of databases that need GROUPNAME privileges */
static size_t conf_dbpath_privileged;

/* Ignore case when matching patterns */
static _Bool conf_ignore_case; /* = 0; */

/* Match only the basename against patterns */
static _Bool conf_match_basename; /* = 0; */

/* Patterns are regexps */
static _Bool conf_match_regexp; /* = 0; */

/* Patterns are BREs */
static _Bool conf_match_regexp_basic; /* = 0; */

/* Output match count only */
static _Bool conf_output_count; /* = 0; */

/* Output limit */
static uintmax_t conf_output_limit;
static _Bool conf_output_limit_set; /* = 0; */

/* Character for output separation */
static char conf_output_separator = '\n';

/* Patterns to search for: regex_t * if conf_match_regexp, char * otherwise */
static void **conf_patterns;
static size_t conf_num_patterns;

/* If !conf_match_regexp, 1 if the pattern contains no characters recognized
   by fnmatch () as special */
static _Bool *conf_patterns_simple;

/* Don't report errors about databases */
static _Bool conf_quiet; /* = 0; */

/* Output only statistics */
static _Bool conf_statistics; /* = 0; */

/* Convert SRC to upper-case in OBSTACK */
static void
uppercase_string (struct obstack *obstack, const char *src)
{
  mbstate_t src_state, dest_state;
  size_t src_left;
  wchar_t wc;

  memset (&src_state, 0, sizeof (src_state));
  memset (&dest_state, 0, sizeof (dest_state));
  src_left = strlen (src) + 1;
  do
    {
      char buf[MB_LEN_MAX];
      size_t size;

      size = mbrtowc (&wc, src, src_left, &src_state);
      if (size == 0)
	size = 1;
      else if (size >= (size_t)-2)
	{
	  size = 1;
	  wc = (unsigned char)*src;
	  memset (&src_state, 0, sizeof (src_state));
	}
      src += size;
      assert (src_left >= size);
      src_left -= size;
      wc = towupper (wc);
      size = wcrtomb (buf, wc, &dest_state);
      if (size == (size_t)-1)
	{
	  size = 1;
	  buf[0] = (unsigned char)wc;
	  memset (&dest_state, 0, sizeof (dest_state));
	}
      obstack_grow (obstack, buf, size);
    }
  while (wc != 0);
}

 /* Access permission checking */

/* The cache is a simple stack of paths, each path longer than the previous
   one.  This allows using calling access() for the object only once (actually,
   only the R_OK | X_OK acess () calls are cached; the R_OK calls are not). */
struct check_entry
{
  struct check_entry *next;
  size_t len;
  _Bool allowed;
  char path[];			/* _Not_ NUL-terminated */
};

/* Stack top (the longest path) */
static struct check_entry *check_stack; /* = NULL; */

/* Contains the check_entry stack */
static struct obstack check_stack_obstack;

/* Contains a single object, for temporary use in check_directory_perms () */
static struct obstack check_obstack;

/* Return (possibly cached) result of access (PATH, R_OK | X_OK).  Note that
   errno is not set. */
static int
cached_access_rx (const char *path)
{
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
  return e->allowed ? 0 : -1;
}

/* Check permissions of parent directory of PATH; it should be accessible and
   readable.
   Return 0 if OK, -1 on error */
static int
check_directory_perms (const char *path)
{
  size_t size;
  char *copy, *p, *slash, *last_slash;
  int res;

  res = -1;
  size = strlen (path) + 1;
  assert (size > 1);
  if (size > OBSTACK_SIZE_MAX)
    goto err; /* The error was already reported */
  copy = obstack_copy (&check_obstack, path, size);
  last_slash = strrchr (copy, '/');
  p = copy + 1; /* "/" was checked in main () */
  while ((slash = strchr (p, '/')) != NULL && slash != last_slash)
    {
      char old;

      old = *slash;
      *slash = 0;
      if (cached_access_rx (copy) != 0)
	goto err_copy;
      *slash = old;
      p = slash + 1;
    }
  if (slash != NULL)
    {
      assert (slash == last_slash);
      *slash = 0;
      /* r-- directories are probably very uncommon, so we try R_OK | X_OK
	 (which pre-populates the cache if PATH has subdirectories) first.
	 This is a heuristic that can in theory almost double the number of
	 access () calls, in practice it reduces the number of access () calls
	 by about 25 %.  The asymptotical number of calls stays the same ;-) */
      if (cached_access_rx (copy) != 0 && access (copy, R_OK) != 0)
	goto err_copy;
    }
  res = 0;
 err_copy:
  obstack_free (&check_obstack, copy);
 err:
  return res;
}

 /* Database search */

/* Number of matches so far */
static uintmax_t matches_found; /* = 0; */

/* Contains a single, never-obstack_finish ()'ed object */
static struct obstack path_obstack;
/* ... after this zero-length marker */
static void *path_obstack_mark;

/* Contains a single object */
static struct obstack uc_obstack;
/* .. after this zero-length marker */
static void *uc_obstack_mark;

/* Statistics of the current database */
static uintmax_t stats_directories;
static uintmax_t stats_entries;
static uintmax_t stats_bytes;

/* PATH was found, handle it as necessary; maintain *VISIBLE: if it is -1,
   check whether the directory containing PATH is accessible and readable and
   set *VISIBLE accordingly; otherwise just use the value;
   return 0 to continue, -1 if match limit was reached */
static int
handle_path (const char *path, int *visible)
{
  const char *s, *matching;
  size_t i;

  if (conf_statistics != 0)
    {
      /* Ignore overflow */
      stats_entries++;
      stats_bytes += strlen (path);
      goto done;
    }
  if (conf_match_basename != 0 && (s = strrchr (path, '/')) != NULL)
    matching = s + 1;
  else
    matching = path;
  for (i = 0; i < conf_num_patterns; i++)
    {
      if (conf_match_regexp != 0)
	{
	  if (regexec (conf_patterns[i], matching, 0, NULL, 0) == 0)
	    goto matched;
	}
      else
	{
	  int flags;

	  flags = 0;
	  if (conf_ignore_case != 0)
	    {
	      obstack_free (&uc_obstack, uc_obstack_mark);
	      uppercase_string (&uc_obstack, matching);
	      matching = obstack_finish (&uc_obstack);
	    }
	  if (conf_patterns_simple[i] != 0)
	    {
	      if (strstr (matching, conf_patterns[i]) != NULL)
		goto matched;
	    }
	  else if (fnmatch (conf_patterns[i], matching, flags) == 0)
	    goto matched;
	}
    }
  goto done;

 matched:
  if (*visible == -1)
    *visible = check_directory_perms (path) == 0;
  if (*visible != 1)
    goto done;
  if (conf_check_existence != 0)
    {
      struct stat st;

      if ((conf_check_follow_trailing ? stat : lstat) (path, &st) != 0)
	goto done;
    }
  /* Overflow (>= 2^64 entries) is just too unlikely to warrant bothering
     about. */
  if (conf_output_count == 0)
    {
      fputs (path, stdout);
      putchar (conf_output_separator);
    }
  matches_found++;
  if (conf_output_limit_set != 0 && matches_found == conf_output_limit)
    return -1;
 done:
  return 0;
}

/* Read and handle DATABASE, open as FILE */
static void
handle_db (FILE *file, const char *database)
{
  struct db db;
  struct db_header hdr;
  struct db_directory dir;
  size_t size;
  int err, visible;

  if (db_open (&db, &hdr, file, database, conf_quiet) != 0)
    goto err;
  stats_directories = 0;
  stats_entries = 0;
  stats_bytes = 0;
  if (db_read_name (&db, &path_obstack, conf_quiet) != 0)
    goto err_path;
  obstack_1grow (&path_obstack, 0);
  visible = hdr.check_visibility ? -1 : 1;
  if (handle_path (obstack_finish (&path_obstack), &visible) != 0)
    goto err_path;
  obstack_free (&path_obstack, path_obstack_mark);
  if (db_skip (&db, ntohl (hdr.conf_size), conf_quiet) != 0)
    goto err_path;
  while (db_read (&db, &dir, sizeof (dir)) == 0)
    {
      size_t dir_name_len;

      stats_directories++;
      if (db_read_name (&db, &path_obstack, conf_quiet) != 0)
	goto err_path;
      size = OBSTACK_OBJECT_SIZE (&path_obstack);
      if (size == 0)
	{
	  if (conf_quiet == 0)
	    error (0, 0, _("invalid empty directory name in `%s'"), database);
	  goto err_path;
	}
      if (size != 1 || *(char *)obstack_base (&path_obstack) != '/')
	obstack_1grow (&path_obstack, '/');
      dir_name_len = OBSTACK_OBJECT_SIZE (&path_obstack);
      visible = hdr.check_visibility ? -1 : 1;
      for (;;)
	{
	  struct db_entry entry;

	  if (db_read (&db, &entry, sizeof (entry)) != 0)
	    {
	      if (conf_quiet == 0)
		read_error (database, db.file, errno);
	      goto err_path;
	    }
	  if (entry.type == DBE_END)
	    break;
	  if (db_read_name (&db, &path_obstack, conf_quiet) != 0)
	    goto err_path;
	  obstack_1grow (&path_obstack, 0);
	  if (handle_path (obstack_base (&path_obstack), &visible) != 0)
	    goto err_path;
	  size = OBSTACK_OBJECT_SIZE (&path_obstack) - dir_name_len;
	  if (size > OBSTACK_SIZE_MAX) /* No surprises, please */
	    {
	      if (conf_quiet == 0)	      
		error (0, 0, _("file name length %zu in `%s' is too large"),
		       size, database);
	      goto err_path;
	    }
	  obstack_blank (&path_obstack, -(ssize_t)size);
	}
      obstack_finish (&path_obstack);
      obstack_free (&path_obstack, path_obstack_mark);
    }
  err = errno;
  /* fread () returned 0 */
  if (ferror (db.file))
    {
      if (conf_quiet == 0)
	read_error (database, db.file, errno);
      goto err_path;
    }
  if (conf_statistics != 0)
    {
      uintmax_t sz;

      sz = db_bytes_read (&db);
      printf (_("Database %s:\n"), database);
      /* The third argument of ngettext () is unsigned long; it is still better
	 to have invalid grammar than truncated numbers. */
      printf (ngettext ("\t%ju directory\n", "\t%ju directories\n",
		       stats_directories), stats_directories);
      printf (ngettext ("\t%ju file\n", "\t%ju files\n", stats_entries),
	      stats_entries);
      printf (ngettext ("\t%ju byte in file names\n",
			"\t%ju bytes in file names\n", stats_bytes),
	      stats_bytes);
      printf (ngettext ("\t%ju byte used to store database\n",
			"\t%ju bytes used to store database\n", sz), sz);
    }
  /* Fall through */
 err_path:
  obstack_finish (&path_obstack);
  obstack_free (&path_obstack, path_obstack_mark);
  db_close (&db);
 err:
  ;
}

 /* Main program */

/* Pointers to database paths, valid between parse_options () and
   finish_dbpath (). */
static struct obstack db_obstack;

/* Pointers to patterns to search for, valid between parse_options () and
   parse_arguments (). */
static struct obstack pattern_obstack;

/* GID of GROUPNAME, or (gid_t)-1 if unknown */
static gid_t privileged_gid;

/* Parse DBPATH, add its entries to db_obstack */
static void
parse_dbpath (const char *dbpath)
{
  /* There is no need to define "path = default path" explicitly */
  if (*dbpath == 0)
    return;
  do
    {
      const char *end;
      size_t len;

      end = strchr (dbpath, ':');
      if (end != NULL)
	len = end - dbpath;
      else
	{
	  len = strlen (dbpath);
	  end = dbpath + len;
	}
      if (len == 0)
	obstack_ptr_grow (&db_obstack, DBFILE);
      else
	{
	  char *copy;

	  copy = xmalloc (len + 1);
	  memcpy (copy, dbpath, len);
	  copy[len] = 0;
	  obstack_ptr_grow (&db_obstack, copy);
	}
      if (*end == ':')
	end++;
      dbpath = end;
    }
  while (*dbpath != 0);
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

  _Bool got_basename, got_follow;

  obstack_init (&db_obstack);
  obstack_init (&pattern_obstack);
  got_basename = 0;
  got_follow = 0;
  for (;;)
    {
      int opt, idx;

      opt = getopt_long (argc, argv, "0HPLSVbcd:ehil:mnqr:sw", options, &idx);
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
	  if (got_follow != 0)
	    error (EXIT_FAILURE, 0, _("--%s would override earlier "
				      "command-line argument"), "nofollow");
	  conf_check_follow_trailing = 0;
	  break;

	case 'L': 
	  if (got_follow != 0)
	    error (EXIT_FAILURE, 0, _("--%s would override earlier "
				      "command-line argument"), "follow");
	  conf_check_follow_trailing = 1;
	  break;

	case 'R':
	  conf_match_regexp = 1;
	  break;

	case 'S':
	  conf_statistics = 1;
	  break;

	case 'V':
	  printf (_("%s %s \n"
		    "Copyright (C) 2005 Red Hat, Inc. All rights reserved.\n"
		    "This software is distributed under the GPL v.2.\n"
		    "\n"
		    "This program is provided with NO WARRANTY, to the extent "
		    "permitted by law.\n"), PACKAGE_NAME, PACKAGE_VERSION);
	  exit (EXIT_SUCCESS);
	  
	case 'b':
	  if (got_basename != 0)
	    error (EXIT_FAILURE, 0, _("--%s would override earlier "
				      "command-line argument"), "basename");
	  got_basename = 1;
	  conf_match_basename = 1;
	  break;

	case 'c':
	  conf_output_count = 1;
	  break;

	case 'd':
	  parse_dbpath (optarg);
	  break;

	case 'e':
	  conf_check_existence = 1;
	  break;
	  
	case 'h':
	  printf (_("Usage: locate [OPTION]... [PATTERN]...\n"
		    "Search for entries in a mlocate database.\n"
		    "\n"
		    "  -b, --basename         match only the base name of "
		    "path names\n"
		    "  -c, --count            only print number of found "
		    "entries\n"
		    "  -d, --database DBPATH  use DBPATH instead of default "
		    "database (which is\n"
		    "                         %s)\n"
		    "  -e, --existing         only print entries for "
		    "currently existing files\n"
		    "  -L, --follow           follow trailing symbolic links "
		    "when checking file\n"
		    "                         existence (default)\n"
		    "  -h, --help             print this help\n"
		    "  -i, --ignore-case      ignore case distinctions when "
		    "matching patterns\n"
		    "  -l, --limit, -n LIMIT  limit output (or counting) to "
		    "LIMIT entries\n"
		    "  -m, --mmap             ignored, for backward "
		    "compatibility\n"
		    "  -P, --nofollow, -H     don't follow trailing symbolic "
		    "links when checking file\n"
		    "                         existence\n"
		    "  -0, --null             separate entries with NUL on "
		    "output\n"
		    "  -S, --statistics       don't search for entries, print "
		    "statistics about each\n"
		    "                         used database\n"
		    "  -q, --quiet            report no error messages about "
		    "reading databases\n"
		    "  -r, --regexp REGEXP    search for basic regexp REGEXP "
		    "instead of patterns\n"
		    "      --regex            patterns are extended regexps\n"
		    "  -s, --stdio            ignored, for backward "
		    "compatibility\n"
		    "  -V, --version          print version information\n"
		    "  -w, --wholename        match whole path name "
		    "(default)\n"), DBFILE);
	  printf (_("\n"
		    "Report bugs to %s.\n"), PACKAGE_BUGREPORT);
	  exit (EXIT_SUCCESS);

	case 'i':
	  conf_ignore_case = 1;
	  break;

	case 'l': case 'n':
	  {
	    char *end;
	    
	    if (conf_output_limit_set != 0)
	      error (EXIT_FAILURE, 0, _("--%s specified twice"), "limit");
	    conf_output_limit_set = 1;
	    errno = 0;
	    conf_output_limit = strtoumax (optarg, &end, 10);
	    if (errno != 0 || *end != 0 || end == optarg
		|| isspace ((unsigned char)*optarg))
	      error (EXIT_FAILURE, 0, _("invalid value `%s' of --limit"),
		     optarg);
	    break;
	  }

	case 'm': case 's':
	  break; /* Ignore */

	case 'q':
	  conf_quiet = 1;
	  break;
	  
	case 'r':
	  conf_match_regexp = 1;
	  conf_match_regexp_basic = 1;
	  obstack_ptr_grow (&pattern_obstack, optarg);
	  break;

	case 'w':
	  if (got_basename != 0)
	    error (EXIT_FAILURE, 0, _("--%s would override earlier "
				      "command-line argument"), "wholename");
	  got_basename = 1;
	  conf_match_basename = 0;
	  break;

	default:
	  abort ();
	}
    }
 options_done:
  if (conf_statistics != 0 || conf_match_regexp_basic != 0)
    {
      if (optind != argc)
	error (EXIT_FAILURE, 0, _("non-option arguments are not allowed with "
				  "--%s"),
	       conf_statistics != 0 ? "statistics" : "regexp");
    }
}

/* Parse arguments in ARGC, ARGV.  Exit on error. */
static void
parse_arguments (int argc, char *argv[])
{
  void **strings;
  size_t i;
      
  for (i = optind; i < (size_t)argc; i++)
    obstack_ptr_grow (&pattern_obstack, argv[i]);
  if (conf_statistics == 0 && OBSTACK_OBJECT_SIZE (&pattern_obstack) == 0)
    error (EXIT_FAILURE, 0, _("no pattern to search for specified"));
  conf_num_patterns = OBSTACK_OBJECT_SIZE (&pattern_obstack) / sizeof (void *);
  conf_patterns = xmalloc (conf_num_patterns * sizeof (*conf_patterns));
  strings = obstack_finish (&pattern_obstack);
  if (conf_match_regexp != 0)
    {
      regex_t *compiled;
      int cflags;
	
      compiled = xmalloc (conf_num_patterns * sizeof (*compiled));
      cflags = REG_NOSUB;
      if (conf_match_regexp_basic == 0) /* GNU-style */
	cflags |= REG_EXTENDED;
      if (conf_ignore_case != 0)
	cflags |= REG_ICASE;
      for (i = 0; i < conf_num_patterns; i++)
	{
	  regex_t *r;
	  int err;

	  r = compiled + i;
	  err = regcomp (r, strings[i], cflags);
	  if (err != 0)
	    {
	      size_t size;
	      char *msg;

	      size = regerror (err, r, NULL, 0);
	      msg = xmalloc (size);
	      regerror (err, r, msg, size);
	      error (EXIT_FAILURE, 0, _("invalid regexp `%s': %s"),
		     (char *)strings[i], msg);
	    }
	  conf_patterns[i] = r;
	}
    }
  else
    {
      struct obstack obstack;

      conf_patterns_simple = xmalloc (conf_num_patterns
				      * sizeof (*conf_patterns_simple));
      if (conf_ignore_case != 0)
	{
	  obstack_init (&obstack);
	  obstack_alignment_mask (&obstack) = 0;
	}
      for (i = 0; i < conf_num_patterns; i++)
	{
	  char *pattern;
	  
	  if (conf_ignore_case == 0)
	    pattern = strings[i];
	  else
	    {
	      uppercase_string (&obstack, strings[i]);
	      pattern = obstack_finish (&obstack);
	    }
	  conf_patterns[i] = pattern;
	  conf_patterns_simple[i] = strpbrk (pattern, "*?[\\]") == NULL;
	}
      /* leave the obstack allocated if (conf_ignore_case != 0) */
    }
  obstack_free (&pattern_obstack, NULL);
}

/* Does a database with ST require GROUPNAME privileges? */
static _Bool
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
  void **src_path;
  char **dest_path;
  size_t src, dest;

  if (OBSTACK_OBJECT_SIZE (&db_obstack) == 0)
    obstack_ptr_grow (&db_obstack, DBFILE);
  locate_path = getenv ("LOCATE_PATH");
  if (locate_path != NULL)
    parse_dbpath (locate_path);
  conf_dbpath_len = OBSTACK_OBJECT_SIZE (&db_obstack) / sizeof (void *);
  src_path = obstack_finish (&db_obstack);
  dest_path = xmalloc (conf_dbpath_len * sizeof (*dest_path));
  dest = 0;
  /* This check is inherenly racy, but we recheck before deciding to drop
     privileges. */
  for (src = 0; src < conf_dbpath_len; src++)
    {
      struct stat st;

      if (strcmp (src_path[src], "-") != 0 && stat (src_path[src], &st) == 0
	  && db_is_privileged (&st))
	{
	  dest_path[dest] = src_path[src];
	  dest++;
	  src_path[src] = NULL;
	}
    }
  conf_dbpath_privileged = dest;
  for (src = 0; src < conf_dbpath_len; src++)
    {
      if (src_path[src] != NULL)
	{
	  dest_path[dest] = src_path[src];
	  dest++;
	}
    }
  assert (dest == conf_dbpath_len);
  obstack_free (&db_obstack, NULL);
  conf_dbpath = dest_path;
}

int
main (int argc, char *argv[])
{
  struct group *grp;
  int res;
  
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
  path_obstack_mark = obstack_alloc (&path_obstack, 0);
  obstack_init (&uc_obstack);
  obstack_alignment_mask (&uc_obstack) = 0;
  uc_obstack_mark = obstack_alloc (&uc_obstack, 0);
  obstack_init (&check_stack_obstack);
  obstack_init (&check_obstack);
  obstack_alignment_mask (&check_obstack) = 0;
  res = EXIT_FAILURE;
  /* Don't call access ("/", R_OK | X_OK) all the time.  This is too strict,
     it is possible to have "/" --x and have a database describing a
     subdirectory, but that is just too improbable. */
  if (access ("/", R_OK | X_OK) == 0)
    {
      _Bool used_stdin;
      size_t i;

      used_stdin = 0;
      for (i = 0; i < conf_dbpath_len; i++)
	{
	  FILE *f;
	  struct stat st;
	  
	  if (conf_output_limit_set && matches_found >= conf_output_limit)
	    {
	      res = EXIT_SUCCESS;
	      goto done;
	    }
	  if (strcmp (conf_dbpath[i], "-") == 0)
	    {
	      if (used_stdin != 0)
		error (EXIT_FAILURE, 0,
		       _("can not read two databases from standard input"));
	      used_stdin = 1;
	      f = stdin;
	    }
	  else
	    {
	      f = fopen (conf_dbpath[i], "rb");
	      if (f == NULL)
		{
		  if (conf_quiet == 0)
		    error (0, errno, _("can not open `%s'"), conf_dbpath[i]);
		  continue;	      
		}
	      if (fstat (fileno (f), &st) != 0)
		{
		  if (conf_quiet == 0)
		    error (0, errno, _("can not stat () `%s'"),
			   conf_dbpath[i]);
		  fclose (f);
		  continue;
		}
	    }
	  if (!db_is_privileged (&st))
	    {
	      if (setgid (getgid ()) != 0)
		error (EXIT_FAILURE, errno, _("can not drop privileges"));
	    }
	  handle_db (f, conf_dbpath[i]);
	}
    }
 done:
  if (conf_output_count != 0)
    printf ("%ju\n", matches_found);
  if (conf_statistics != 0 || matches_found != 0)
    res = EXIT_SUCCESS;
  fflush (stdout);
  if (ferror (stdout))
    error (EXIT_FAILURE, 0, _("I/O error while writing to standard output"));
  return res;
}
