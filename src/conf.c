/* updatedb configuration.

Copyright (C) 2005, 2007, 2008 Red Hat, Inc. All rights reserved.
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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <getopt.h>
#include "canonicalize.h"
#include "error.h"
#include "obstack.h"
#include "xalloc.h"

#include "conf.h"
#include "lib.h"

/* true if locate(1) should check whether files are visible before reporting
   them */
bool conf_check_visibility = true;

/* Filesystems to skip, converted to uppercase and sorted by name */
struct string_list conf_prunefs;

/* Directory names to skip, sorted by name */
struct string_list conf_prunenames;

/* Paths to skip, sorted by name using dir_path_cmp () */
struct string_list conf_prunepaths;

/* true if bind mounts should be skipped */
bool conf_prune_bind_mounts; /* = false; */

/* true if pruning debug output was requested */
bool conf_debug_pruning; /* = false; */

/* Root of the directory tree to store in the database (canonical) */
char *conf_scan_root; /* = NULL; */

/* Absolute (not necessarily canonical) path to the database */
const char *conf_output; /* = NULL; */

/* 1 if file names should be written to stdout as they are found */
bool conf_verbose; /* = false; */

/* Configuration representation for the database configuration block */
const char *conf_block;
size_t conf_block_size;

/* Parse a STR, store the parsed boolean value to DEST;
   return 0 if OK, -1 on error. */
static int
parse_bool (bool *dest, const char *str)
{
  if (strcmp (str, "0") == 0 || strcmp (str, "no") == 0)
    {
      *dest = false;
      return 0;
    }
  if (strcmp (str, "1") == 0 || strcmp (str, "yes") == 0)
    {
      *dest = true;
      return 0;
    }
  return -1;
}

 /* String list handling */

/* A "variable" in progress: a list of whitespace-separated strings, used to
   initialize a struct string_list.  Each "variable" can be used only with one
   string_list, which is initialized to zeroes. */
struct var
{
  struct obstack strings; /* Strings */
  void *strings_mark;
};

/* PRUNEFS */
static struct var prunefs_var;

/* PRUNENAMES */
static struct var prunenames_var;

/* PRUNEPATHS */
static struct var prunepaths_var;

/* Initialize VAR */
static void
var_init (struct var *var)
{
  obstack_init (&var->strings);
  obstack_alignment_mask (&var->strings) = 0;
  var->strings_mark = obstack_alloc (&var->strings, 0);
}

/* Add values from space-separated VAL to VAR and LIST */
static void
var_add_values (struct var *var, struct string_list *list, const char *val)
{
  for (;;)
    {
      const char *start;

      while (isspace ((unsigned char)*val))
	val++;
      if (*val == 0)
	break;
      start = val;
      do
	val++;
      while (*val != 0 && !isspace ((unsigned char)*val));
      string_list_append (list,
			  obstack_copy0 (&var->strings, start, val - start));
    }
}

/* Clear contents of VAR and LIST */
static void
var_clear (struct var *var, struct string_list *list)
{
  obstack_free (&var->strings, var->strings_mark);
  var->strings_mark = obstack_alloc (&var->strings, 0);
  list->len = 0;
}

/* Compare two string pointers */
static int
cmp_pointers (const void *xa, const void *xb)
{
  char *const *a, *const *b;

  a = xa;
  b = xb;
  return strcmp (*a, *b);
}

/* Finish variable LIST, sort its contents, remove duplicates */
static void
var_finish (struct string_list *list)
{
  qsort (list->entries, list->len, sizeof (*list->entries), cmp_pointers);
  if (list->len != 0)
    {
      char **src, **dest;

      dest = list->entries + 1;
      for (src = list->entries + 1; src < list->entries + list->len; src++)
	{
	  if (strcmp (dest[-1], *src) != 0)
	    {
	      *dest = *src;
	      dest++;
	    }
	}
      list->len = dest - list->entries;
    }
  list->entries = xnrealloc (list->entries, list->len, sizeof (*list->entries));
}

 /* UPDATEDB_CONF parsing */

/* UPDATEDB_CONF (locked) */
static FILE *uc_file;
/* Line number at token start; type matches error_at_line () */
static unsigned uc_line;
/* Current line number; type matches error_at_line () */
static unsigned uc_current_line;
/* Last string returned by uc_lex */
static char *uc_lex_buf;
static size_t uc_lex_buf_size;

/* Token types */
enum
  {
    UCT_EOF, UCT_EOL, UCT_IDENTIFIER, UCT_EQUAL, UCT_QUOTED, UCT_OTHER,
    UCT_PRUNE_BIND_MOUNTS, UCT_PRUNEFS, UCT_PRUNENAMES, UCT_PRUNEPATHS
  };

/* Append C to uc_lex_buf at POS;
   return updated POS */
static size_t
uc_lex_buf_append (size_t pos, char c)
{
  if (pos == uc_lex_buf_size)
    uc_lex_buf = x2realloc (uc_lex_buf, &uc_lex_buf_size);
  uc_lex_buf[pos] = c;
  return pos + 1;
}

/* Return next token from uc_file; for UCT_IDENTIFIER, UCT_QUOTED or keywords,
   store the data to uc_lex_buf (valid until next call). */
static int
uc_lex (void)
{
  int c;

  uc_line = uc_current_line;
  do
    {
      c = getc_unlocked (uc_file);
      if (c == EOF)
	return UCT_EOF;
    }
  while (c != '\n' && isspace ((unsigned char)c));
  switch (c)
    {
    case '#':
      do
	{
	  c = getc_unlocked (uc_file);
	  if (c == EOF)
	    return UCT_EOF;
	}
      while (c != '\n');
      /* Fall through */
    case '\n':
      uc_current_line++;
      if (uc_current_line == 0)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_current_line - 1,
			 _("warning: Line number overflow"));
	  error_message_count--; /* Don't count as an error */
	}
      return UCT_EOL;

    case '=':
      return UCT_EQUAL;

    case '"':
      {
	size_t i;

	i = 0;
	while ((c = getc_unlocked (uc_file)) != '"')
	  {
	    if (c == EOF || c == '\n')
	      {
		error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			       _("missing closing `\"'"));
		ungetc (c, uc_file);
		break;
	      }
	    i = uc_lex_buf_append (i, c);
	  }
	uc_lex_buf_append (i, 0);
	return UCT_QUOTED;
      }

    default:
      {
	size_t i;

	if (!isalpha ((unsigned char)c) && c != '_')
	  return UCT_OTHER;
	i = 0;
	do
	  {
	    i = uc_lex_buf_append (i, c);
	    c = getc_unlocked (uc_file);
	  }
	while (c != EOF && (isalnum ((unsigned char)c) || c == '_'));
	ungetc (c, uc_file);
	uc_lex_buf_append (i, 0);
	if (strcmp (uc_lex_buf, "PRUNE_BIND_MOUNTS") == 0)
	  return UCT_PRUNE_BIND_MOUNTS;
	if (strcmp (uc_lex_buf, "PRUNEFS") == 0)
	  return UCT_PRUNEFS;
	if (strcmp (uc_lex_buf, "PRUNENAMES") == 0)
	  return UCT_PRUNENAMES;
	if (strcmp (uc_lex_buf, "PRUNEPATHS") == 0)
	  return UCT_PRUNEPATHS;
	return UCT_IDENTIFIER;
      }
    }
}

/* Parse /etc/updatedb.conf.  Exit on I/O or syntax error. */
static void
parse_updatedb_conf (void)
{
  int old_error_one_per_line;
  unsigned old_error_message_count;
  bool had_prune_bind_mounts, had_prunefs, had_prunenames, had_prunepaths;

  uc_file = fopen (UPDATEDB_CONF, "r");
  if (uc_file == NULL)
    {
      if (errno != ENOENT)
	error (EXIT_FAILURE, errno, _("can not open `%s'"), UPDATEDB_CONF);
      goto err;
    }
  flockfile (uc_file);
  uc_current_line = 1;
  old_error_message_count = error_message_count;
  old_error_one_per_line = error_one_per_line;
  error_one_per_line = 1;
  had_prune_bind_mounts = false;
  had_prunefs = false;
  had_prunenames = false;
  had_prunepaths = false;
  for (;;)
    {
      bool *had_var;
      int var_token, token;

      token = uc_lex ();
      switch (token)
	{
	case UCT_EOF:
	  goto eof;

	case UCT_EOL:
	  continue;

	case UCT_PRUNE_BIND_MOUNTS:
	  had_var = &had_prune_bind_mounts;
	  break;

	case UCT_PRUNEFS:
	  had_var = &had_prunefs;
	  break;

	case UCT_PRUNENAMES:
	  had_var = &had_prunenames;
	  break;

	case UCT_PRUNEPATHS:
	  had_var = &had_prunepaths;
	  break;

	case UCT_IDENTIFIER:
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("unknown variable `%s'"), uc_lex_buf);
	  goto skip_to_eol;

	default:
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("variable name expected"));
	  goto skip_to_eol;
	}
      if (*had_var != false)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("variable `%s' was already defined"), uc_lex_buf);
	  goto skip_to_eol;
	}
      *had_var = true;
      var_token = token;
      token = uc_lex ();
      if (token != UCT_EQUAL)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("`=' expected after variable name"));
	  goto skip_to_eol;
	}
      token = uc_lex ();
      if (token != UCT_QUOTED)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("value in quotes expected after `='"));
	  goto skip_to_eol;
	}
      if (var_token == UCT_PRUNE_BIND_MOUNTS)
	{
	  if (parse_bool (&conf_prune_bind_mounts, uc_lex_buf) != 0)
	    {
	      error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			     _("invalid value `%s' of PRUNE_BIND_MOUNTS"),
			     uc_lex_buf);
	      goto skip_to_eol;
	    }
	}
      else if (var_token == UCT_PRUNEFS)
	var_add_values (&prunefs_var, &conf_prunefs, uc_lex_buf);
      else if (var_token == UCT_PRUNENAMES)
	var_add_values (&prunenames_var, &conf_prunenames, uc_lex_buf);
      else if (var_token == UCT_PRUNEPATHS)
	var_add_values (&prunepaths_var, &conf_prunepaths, uc_lex_buf);
      else
	abort ();
      token = uc_lex ();
      if (token != UCT_EOL && token != UCT_EOF)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("unexpected data after variable value"));
	  goto skip_to_eol;
	}
      /* Fall through */
    skip_to_eol:
      while (token != UCT_EOL)
	{
	  if (token == UCT_EOF)
	    goto eof;
	  token = uc_lex ();
	}
    }
 eof:
  if (ferror (uc_file))
    error (EXIT_FAILURE, 0, _("I/O error reading `%s'"), UPDATEDB_CONF);
  error_one_per_line = old_error_one_per_line;
  free (uc_lex_buf);
  uc_lex_buf_size = 0;
  funlockfile (uc_file);
  fclose (uc_file);
  if (error_message_count != old_error_message_count)
    exit (EXIT_FAILURE);
 err:
  ;
}

 /* Command-line argument parsing */

/* Output --help text */
static void
help (void)
{
  printf (_("Usage: updatedb [OPTION]...\n"
	    "Update a mlocate database.\n"
	    "\n"
	    "  -f, --add-prunefs FS           omit also FS\n"
	    "  -n, --add-prunenames NAMES     omit also NAMES\n"
	    "  -e, --add-prunepaths PATHS     omit also PATHS\n"
	    "  -U, --database-root PATH       the subtree to store in "
	    "database (default \"/\")\n"
	    "  -h, --help                     print this help\n"
	    "  -o, --output FILE              database to update (default\n"
	    "                                 `%s')\n"
	    "      --prune-bind-mounts FLAG   omit bind mounts (default "
	    "\"no\")\n"
	    "      --prunefs FS               filesystems to omit from "
	    "database\n"
	    "      --prunenames NAMES         directory names to omit from "
	    "database\n"
	    "      --prunepaths PATHS         paths to omit from database\n"
	    "  -l, --require-visibility FLAG  check visibility before "
	    "reporting files\n"
	    "                                 (default \"yes\")\n"
	    "  -v, --verbose                  print paths of files as they "
	    "are found\n"
	    "  -V, --version                  print version information\n"
	    "\n"
	    "The configuration defaults to values read from\n"
	    "`%s'.\n"), DBFILE, UPDATEDB_CONF);
  printf (_("\n"
	    "Report bugs to %s.\n"), PACKAGE_BUGREPORT);
}

/* Prepend current working directory to PATH;
   return resulting pathm, for free () */
static char *
prepend_cwd (const char *path)
{
  char *buf, *res;
  size_t size, len1, size2;

  buf = NULL;
  size = BUFSIZ; /* Not PATH_MAX because it is not defined on some platforms. */
  do
    buf = x2realloc (buf, &size);
  while ((res = getcwd (buf, size)) == NULL && errno == ERANGE);
  if (res == NULL)
    error (EXIT_FAILURE, errno, _("can not get current working directory"));
  len1 = strlen (buf);
  size2 = strlen (path) + 1;
  buf = xrealloc (buf, len1 + 1 + size2);
  buf[len1] = '/';
  memcpy (buf + len1 + 1, path, size2);
  return buf;
}

/* Parse ARGC, ARGV.  Exit on error or --help, --version. */
static void
parse_arguments (int argc, char *argv[])
{
  enum { OPT_DEBUG_PRUNING = CHAR_MAX + 1 };

  static const struct option options[] =
    {
      { "add-prunefs", required_argument, NULL, 'f' },
      { "add-prunenames", required_argument, NULL, 'n' },
      { "add-prunepaths", required_argument, NULL, 'e' },
      { "database-root", required_argument, NULL, 'U' },
      { "debug-pruning", no_argument, NULL, OPT_DEBUG_PRUNING },
      { "help", no_argument, NULL, 'h' },
      { "output", required_argument, NULL, 'o' },
      { "prune-bind-mounts", required_argument, NULL, 'B' },
      { "prunefs", required_argument, NULL, 'F' },
      { "prunenames", required_argument, NULL, 'N' },
      { "prunepaths", required_argument, NULL, 'P' },
      { "require-visibility", required_argument, NULL, 'l' },
      { "verbose", no_argument, NULL, 'v' },
      { "version", no_argument, NULL, 'V' },
      { NULL, 0, NULL, 0 }
    };

  bool prunefs_changed, prunenames_changed, prunepaths_changed;
  bool got_prune_bind_mounts, got_visibility;

  prunefs_changed = false;
  prunenames_changed = false;
  prunepaths_changed = false;
  got_prune_bind_mounts = false;
  got_visibility = false;
  for (;;)
    {
      int opt, idx;

      opt = getopt_long (argc, argv, "U:Ve:f:hl:n:o:v", options, &idx);
      switch (opt)
	{
	case -1:
	  goto options_done;

	case '?':
	  exit (EXIT_FAILURE);

	case 'B':
	  if (got_prune_bind_mounts != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "prune-bind-mounts");
	  got_prune_bind_mounts = true;
	  if (parse_bool (&conf_prune_bind_mounts, optarg) != 0)
	    error (EXIT_FAILURE, 0, _("invalid value `%s' of --%s"), optarg,
		   "prune-bind-mounts");
	  break;

	case 'F':
	  if (prunefs_changed != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "prunefs");
	  prunefs_changed = true;
	  var_clear (&prunefs_var, &conf_prunefs);
	  var_add_values (&prunefs_var, &conf_prunefs, optarg);
	  break;

	case 'N':
	  if (prunenames_changed != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "prunenames");
	  prunenames_changed = true;
	  var_clear (&prunenames_var, &conf_prunenames);
	  var_add_values (&prunenames_var, &conf_prunenames, optarg);
	  break;

	case 'P':
	  if (prunepaths_changed != false)
	    error (EXIT_FAILURE, 0,
		   _("--%s would override earlier command-line argument"),
		   "prunepaths");
	  prunepaths_changed = true;
	  var_clear (&prunepaths_var, &conf_prunepaths);
	  var_add_values (&prunepaths_var, &conf_prunepaths, optarg);
	  break;

	case 'U':
	  if (conf_scan_root != NULL)
	    error (EXIT_FAILURE, 0, _("--%s specified twice"),
		   "database-root");
	  conf_scan_root = canonicalize_file_name (optarg);
	  if (conf_scan_root == NULL)
	    error (EXIT_FAILURE, errno, _("invalid value `%s' of --%s"), optarg,
		   "database-root");
	  break;

	case 'V':
	  puts ("updatedb (" PACKAGE_NAME ") " PACKAGE_VERSION);
	  puts (_("Copyright (C) 2007 Red Hat, Inc. All rights reserved.\n"
		  "This software is distributed under the GPL v.2.\n"
		  "\n"
		  "This program is provided with NO WARRANTY, to the extent "
		  "permitted by law."));
	  exit (EXIT_SUCCESS);

	case 'e':
	  prunepaths_changed = true;
	  var_add_values (&prunepaths_var, &conf_prunepaths, optarg);
	  break;

	case 'f':
	  prunefs_changed = true;
	  var_add_values (&prunefs_var, &conf_prunefs, optarg);
	  break;

	case 'h':
	  help ();
	  exit (EXIT_SUCCESS);

	case 'l':
	  if (got_visibility != false)
	    error (EXIT_FAILURE, 0, _("--%s specified twice"),
		   "require-visibility");
	  got_visibility = true;
	  if (parse_bool (&conf_check_visibility, optarg) != 0)
	    error (EXIT_FAILURE, 0, _("invalid value `%s' of --%s"), optarg,
		   "require-visibility");
	  break;

	case 'n':
	  prunenames_changed = true;
	  var_add_values (&prunenames_var, &conf_prunenames, optarg);
	  break;

	case 'o':
	  if (conf_output != NULL)
	    error (EXIT_FAILURE, 0, _("--%s specified twice"), "output");
	  conf_output = optarg;
	  break;

	case 'v':
	  conf_verbose = true;
	  break;

	case OPT_DEBUG_PRUNING:
	  conf_debug_pruning = true;
	  break;

	default:
	  abort ();
	}
    }
 options_done:
  if (optind != argc)
    error (EXIT_FAILURE, 0, _("unexpected operand on command line"));
  if (conf_scan_root == NULL)
    {
      static char root[] = "/";

      conf_scan_root = root;
    }
  if (conf_output == NULL)
    conf_output = DBFILE;
  if (*conf_output != '/')
    conf_output = prepend_cwd (conf_output);
}

 /* Conversion of configuration for main code */

/* Store a string list to OBSTACK */
static void
gen_conf_block_string_list (struct obstack *obstack,
			    const struct string_list *strings)
{
  static const char nul; /* = 0; */

  size_t i;

  for (i = 0; i < strings->len; i++)
    obstack_grow (obstack, strings->entries[i],
		  strlen (strings->entries[i]) + 1);
  obstack_grow (obstack, &nul, 1);
}

/* Generate conf_block */
static void
gen_conf_block (void)
{
  struct obstack obstack;

  obstack_init (&obstack);
  obstack_alignment_mask (&obstack) = 0;
#define CONST(S) obstack_grow (&obstack, S, sizeof (S))
  /* conf_check_visibility value is stored in the header */
  CONST ("prune_bind_mounts");
  /* Add two NUL bytes after the value */
  obstack_grow (&obstack, conf_prune_bind_mounts != false ? "1\0" : "0\0", 3);
  CONST ("prunefs");
  gen_conf_block_string_list (&obstack, &conf_prunefs);
  CONST ("prunenames");
  gen_conf_block_string_list (&obstack, &conf_prunenames);
  CONST ("prunepaths");
  gen_conf_block_string_list (&obstack, &conf_prunepaths);
  /* scan_root is contained directly in the header */
  /* conf_output, conf_verbose are not relevant */
#undef CONST
  conf_block_size = OBSTACK_OBJECT_SIZE (&obstack);
  conf_block = obstack_finish (&obstack);
}

/* Parse /etc/updatedb.conf and command-line arguments ARGC, ARGV.
   Exit on error or --help, --version. */
void
conf_prepare (int argc, char *argv[])
{
  size_t i;

  var_init (&prunefs_var);
  var_init (&prunenames_var);
  var_init (&prunepaths_var);
  parse_updatedb_conf ();
  parse_arguments (argc, argv);
  for (i = 0; i < conf_prunefs.len; i++)
    {
      char *p;

      /* Assuming filesystem names are ASCII-only */
      for (p = conf_prunefs.entries[i]; *p != 0; p++)
	*p = toupper((unsigned char)*p);
    }
  /* Finish the variable only after converting filesystem names to upper case
     to avoid keeping duplicates that originally differed in case and to sort
     them correctly. */
  var_finish (&conf_prunefs);
  var_finish (&conf_prunenames);
  var_finish (&conf_prunepaths);
  gen_conf_block ();
  string_list_dir_path_sort (&conf_prunepaths);
  if (conf_debug_pruning != false)
    {
      const char *p;

      /* This is debuging output, don't mark anything for translation */
      fprintf (stderr, "conf_block:\n");
      for (p = conf_block; p < conf_block + conf_block_size; p++)
	{
	  char c;

	  c = *p;
	  if (isascii ((unsigned char)c) && isprint ((unsigned char)c)
	      && c != '\\')
	    putc (c, stderr);
	  else
	    {
	      fprintf (stderr, "\\%03o", (unsigned)(unsigned char)c);
	      if (c == 0)
		putc ('\n', stderr);
	    }
	}
      fprintf (stderr, "\n-----------------------\n");
    }
}
