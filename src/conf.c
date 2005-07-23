/* updatedb configuration.

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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <error.h>
#include <getopt.h>
#include <obstack.h>

#include "conf.h"
#include "lib.h"

/* 1 if locate(1) should check whether files are visible before reporting
   them */
_Bool conf_check_visibility = 1;

/* Filesystems to skip, sorted by name */
char *const *conf_prunefs;
size_t conf_prunefs_len;

/* Paths to skip, sorted by name using dir_path_cmp () */
char *const *conf_prunepaths;
size_t conf_prunepaths_len;

/* Root of the directory tree to store in the database */
const char *conf_scan_root; /* = NULL; */

/* Database path */
const char *conf_output; /* = NULL; */

/* 1 if file names should be written to stdout as they are found */
_Bool conf_verbose; /* = 0; */

/* Configuration representation for the database configuration block */
const char *conf_block;
size_t conf_block_size;

 /* String list handling */

/* A "variable" in progress: a list of whitespace-separated strings */
struct var
{
  struct obstack strings; /* Strings */
  struct obstack pointers;
  void *strings_mark, *pointers_mark;
};

/* PRUNEFS */
static struct var prunefs_var;

/* PRUNEPATHS */
static struct var prunepaths_var;

/* Initialize VAR */
static void
var_init (struct var *var)
{
  obstack_init (&var->strings);
  obstack_alignment_mask (&var->strings) = 0;
  obstack_init (&var->pointers);
  var->strings_mark = obstack_alloc (&var->strings, 0);
  var->pointers_mark = obstack_alloc (&var->pointers, 0);
}

/* Add values from space-separated VAL to VAR */
static void
var_add_values (struct var *var, const char *val)
{
  for (;;)
    {
      const char *start;
      char *p;

      while (isspace ((unsigned char)*val))
	val++;
      if (*val == 0)
	break;
      start = val;
      do
	val++;
      while (*val != 0 && !isspace ((unsigned char)*val));
      p = obstack_copy0 (&var->strings, start, val - start);
      obstack_ptr_grow (&var->pointers, p);
    }
}

/* Clear contents of VAR */
static void
var_clear (struct var *var)
{
  obstack_free (&var->strings, var->strings_mark);
  obstack_free (&var->pointers, var->pointers_mark);
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

/* Finish VAR, sort its contents and remove duplicates;
   return array of strings, set *LEN to number of members */
static char **
var_finish (struct var *var, size_t *len)
{
  char **base;
  size_t num;

  num = OBSTACK_OBJECT_SIZE (&var->pointers) / sizeof (char *);
  base = obstack_finish (&var->pointers);
  qsort (base, num, sizeof (*base), cmp_pointers);
  if (num != 0)
    {
      char **src, **dest;

      dest = base + 1;
      for (src = base + 1; src < base + num; src++)
	{
	  if (strcmp (dest[-1], *src) != 0)
	    {
	      *dest = *src;
	      dest++;
	    }
	}
    }
  *len = num;
  return base;
}

 /* UPDATEDB_CONF parsing */

/* UPDATEDB_CONF (locked) */
static FILE *uc_file;
/* Line number at token start; type matches error_at_line () */
static unsigned uc_line;
/* Current line number; type matches error_at_line () */
static unsigned uc_current_line;
/* Obstack for string data */
static struct obstack uc_obstack;

/* Token types */
enum { UCT_EOF, UCT_EOL, UCT_IDENTIFIER, UCT_EQUAL, UCT_QUOTED, UCT_OTHER };

/* Return next token from uc_file; for UCT_IDENTIFIER or UCT_OTHER store
   pointer to data to *PTR (valid until next call). */
static int
uc_lex (char **ptr)
{
  static void *obstack_mark; /* = NULL; */

  int c;

  if (obstack_mark != NULL)
    {
      obstack_free (&uc_obstack, obstack_mark);
      obstack_mark = NULL;
    }
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
      while ((c = getc_unlocked (uc_file)) != '"')
	{
	  if (c == EOF || c == '\n')
	    {
	      error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			     _("EOF in quoted string"));
	      if (c != EOF)
		ungetc (c, uc_file);
	      break;
	    }
	  obstack_1grow (&uc_obstack, c);
	}
      obstack_1grow (&uc_obstack, 0);
      *ptr = obstack_finish (&uc_obstack);
      obstack_mark = *ptr;
      return UCT_QUOTED;
      
    default:
      if (!isalpha ((unsigned char)c))
	return UCT_OTHER;
      do
	{
	  obstack_1grow (&uc_obstack, c);
	  c = getc_unlocked (uc_file);
	}
      while (c != EOF && isalnum ((unsigned char)c));
      if (c != EOF)
	ungetc (c, uc_file);
      obstack_1grow (&uc_obstack, 0);
      *ptr = obstack_finish (&uc_obstack);
      obstack_mark = *ptr;
      return UCT_IDENTIFIER;
    }
}

/* Parse /etc/updatedb.conf.  Exit on I/O or syntax error. */
static void
parse_updatedb_conf (void)
{
  int old_error_one_per_line;
  unsigned old_error_message_count;
  _Bool had_prunefs, had_prunepaths;

  uc_file = fopen (UPDATEDB_CONF, "r");
  if (uc_file == NULL)
    {
      if (errno != ENOENT)
	/* Not EXIT_FAILURE */
	error (0, errno, _("can not open `%s'"), UPDATEDB_CONF);
      goto err;
    }
  flockfile (uc_file);
  obstack_init (&uc_obstack);
  obstack_alignment_mask (&uc_obstack) = 0;
  uc_current_line = 1;
  old_error_message_count = error_message_count;
  old_error_one_per_line = error_one_per_line;
  error_one_per_line = 1;
  had_prunefs = 0;
  had_prunepaths = 0;
  for (;;)
    {
      struct var *var;
      _Bool *had_var;
      char *val;
      int token;

      token = uc_lex (&val);
      switch (token)
	{
	case UCT_EOF:
	  goto eof;
	  
	case UCT_EOL:
	  continue;

	case UCT_IDENTIFIER:
	  break;

	default:
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("variable name expected"));
	  goto skip_to_eol;
	}
      if (strcmp (val, "PRUNEFS") == 0)
	{
	  var = &prunefs_var;
	  had_var = &had_prunefs;
	}
      else if (strcmp (val, "PRUNEPATHS") == 0)
	{
	  var = &prunepaths_var;
	  had_var = &had_prunepaths;
	}
      else
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("unknown variable `%s'"), val);
	  goto skip_to_eol;
	}
      if (*had_var != 0)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("variable `%s' was already defined"), val);
	  goto skip_to_eol;
	}
      *had_var = 1;
      token = uc_lex (&val);
      if (token != UCT_EQUAL)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("`=' expected after variable name"));
	  goto skip_to_eol;
	}
      token = uc_lex (&val);
      if (token != UCT_QUOTED)
	{
	  error_at_line (0, 0, UPDATEDB_CONF, uc_line,
			 _("value in quotes expected after `='"));
	  goto skip_to_eol;
	}
      var_add_values (var, val);
      token = uc_lex (&val);
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
	  token = uc_lex (&val);
	}
    }
 eof:
  if (ferror (uc_file))
    {
      read_error (UPDATEDB_CONF, uc_file, 0);
      exit (EXIT_FAILURE);
    }
  error_one_per_line = old_error_one_per_line;
  obstack_free (&uc_obstack, NULL);
  funlockfile (uc_file);
  fclose (uc_file);
  if (error_message_count != old_error_message_count)
    exit (EXIT_FAILURE);
 err:
  ;
}

 /* Command-line argument parsing */

/* Parse ARGC, ARGV.  Exit on error or --help, --version. */
static void
parse_arguments (int argc, char *argv[])
{
  static const struct option options[] =
    {
      { "add-prunefs", required_argument, NULL, 'f' },
      { "add-prunepaths", required_argument, NULL, 'e' },
      { "database-root", required_argument, NULL, 'R' },
      { "help", no_argument, NULL, 'h' },
      { "output", required_argument, NULL, 'o' },
      { "prunefs", required_argument, NULL, 'F' },
      { "prunepaths", required_argument, NULL, 'P' },
      { "require-visibility", required_argument, NULL, 'l' },
      { "verbose", no_argument, NULL, 'v' },
      { "version", no_argument, NULL, 'V' },
      { NULL, 0, NULL, 0 }
    };

  _Bool prunefs_changed, prunepaths_changed, had_visibility;

  prunefs_changed = 0;
  prunepaths_changed = 0;
  had_visibility = 0;
  for (;;)
    {
      int opt, idx;

      opt = getopt_long (argc, argv, "U:Ve:f:hl:o:v", options, &idx);
      switch (opt)
	{
	case -1:
	  goto options_done;

	case '?':
	  exit (EXIT_FAILURE);

	case 'F':
	  if (prunefs_changed != 0)
	    error (EXIT_FAILURE, 0, _("--prunefs would override earlier "
				      "command-line argument"));
	  prunefs_changed = 1;
	  var_clear (&prunefs_var);
	  var_add_values (&prunefs_var, optarg);
	  break;
	  
	case 'P':
	  if (prunepaths_changed != 0)
	    error (EXIT_FAILURE, 0, _("--prunepaths would override earlier "
				      "command-line argument"));
	  prunepaths_changed = 1;
	  var_clear (&prunepaths_var);
	  var_add_values (&prunepaths_var, optarg);
	  break;
	  
	case 'U':
	  if (conf_scan_root != NULL)
	    error (EXIT_FAILURE, 0, _("--database-root specified twice"));
	  conf_scan_root = optarg;
	  if (*conf_scan_root != '/')
	    /* Not necessarily the canonical path name */
	    error (EXIT_FAILURE, 0, _("the argument to --database-root must "
				      "be an absolute path name"));
	  break;

	case 'V':
	  printf (_("%s %s \n"
		    "Copyright (C) 2005 Red Hat, Inc. All rights reserved.\n"
		    "This software is distributed under the GPL v.2.\n"
		    "\n"
		    "This program is provided with NO WARRANTY, to the extent "
		    "permitted by law.\n"), PACKAGE_NAME, PACKAGE_VERSION);
	  exit (EXIT_SUCCESS);
	  
	case 'e':
	  prunepaths_changed = 1;
	  var_add_values (&prunepaths_var, optarg);
	  break;
	  
	case 'f':
	  prunefs_changed = 1;
	  var_add_values (&prunefs_var, optarg);
	  break;
	  
	case 'h':
	  printf (_("Usage: updatedb [OPTION]...\n"
		    "Update a mlocate database.\n"
		    "\n"
		    "  -h, --help                     print this help\n"
		    "  -V, --version                  print version "
		    "information\n"
		    "      --prunepaths PATHS         paths to omit from "
		    "database\n"
		    "      --prunefs FS               filesystems to omit "
		    "from database\n"
		    "  -o, --output FILE              database to update "
		    "(default\n"
		    "                                 `%s'\n"
		    "  -U, --database-root PATH       the subtree to store in "
		    "database (default \"/\")\n"
		    "  -e, --add-prunepaths PATHS     omit also PATHS\n"
		    "  -f, --add-prunefs FS           omit also FS\n"
		    "  -l, --require-visibility FLAG  check visibility "
		    "before reporting files\n"
		    "                                 (default \"true\")\n"
		    "  -v, --verbose                  print paths of files as "
		    "they are found\n"
		    "\n"
		    "The lists of paths and filesystems to omit default to "
		    "values read from\n"
		    "`%s'.\n"
		    "\n"
		    "Report bugs to %s.\n"), DBFILE, UPDATEDB_CONF,
		  PACKAGE_BUGREPORT);
	  exit (EXIT_SUCCESS);

	case 'l':
	  if (had_visibility != 0)
	    error (EXIT_FAILURE, 0, _("--require-visibility specified twice"));
	  had_visibility = 1;
	  if (strcmp (optarg, "0") == 0 || strcmp (optarg, "no") == 0
	      || strcmp (optarg, "false") == 0)
	    conf_check_visibility = 0;
	  else if (strcmp (optarg, "1") == 0 || strcmp (optarg, "yes") == 0
	      || strcmp (optarg, "true") == 0)
	    conf_check_visibility = 1;
	  else
	    error (EXIT_FAILURE, 0, _("unknown value `%s' of "
				      "--require-visibility"), optarg);
	  break;
	  
	case 'o':
	  if (conf_output != NULL)
	    error (EXIT_FAILURE, 0, _("--output specified twice"));
	  conf_output = optarg;
	  break;
	  
	case 'v':
	  conf_verbose = 1;
	  break;

	default:
	  abort ();
	}
    }
 options_done:
  if (optind != argc)
    error (EXIT_FAILURE, 0, _("unexpected operand on command line"));
  if (conf_scan_root == NULL)
    conf_scan_root = "/";
  if (conf_output == NULL)
    conf_output = DBFILE;
}

 /* Conversion of configuration for main code */

static struct obstack conf_block_obstack;
  
/* Generate conf_block */
static void
gen_conf_block (void)
{
  static const char nul; /* = 0; */

  size_t i;

  obstack_init (&conf_block_obstack);
  obstack_alignment_mask (&conf_block_obstack) = 0;
#define CONST(S) obstack_grow (&conf_block_obstack, S, sizeof (S))
  /* conf_check_visibility value is stored in the header */
  CONST ("prunefs");
  for (i = 0; i < conf_prunefs_len; i++)
    obstack_grow (&conf_block_obstack, conf_prunefs[i],
		  strlen (conf_prunefs[i]) + 1);
  obstack_grow (&conf_block_obstack, &nul, 1);
  CONST ("prunepaths");
  for (i = 0; i < conf_prunepaths_len; i++)
    obstack_grow (&conf_block_obstack, conf_prunepaths[i],
		  strlen (conf_prunepaths[i]) + 1);
  obstack_grow (&conf_block_obstack, &nul, 1);
  /* scan_root is contained directly in the header */
  /* conf_output, conf_verbose are not relevant */
#undef CONST
  conf_block_size = OBSTACK_OBJECT_SIZE (&conf_block_obstack);
  conf_block = obstack_finish (&conf_block_obstack);
}
  
/* Compare two string pointers using dir_path_cmp () */
static int
cmp_dir_path_pointers (const void *xa, const void *xb)
{
  char *const *a, *const *b;

  a = xa;
  b = xb;
  return dir_path_cmp (*a, *b);
}

/* Parse /etc/updatedb.conf and command-line arguments ARGC, ARGV.
   Exit on error or --help, --version. */
void
conf_prepare (int argc, char *argv[])
{
  char **paths;
  size_t i;

  var_init (&prunefs_var);
  var_init (&prunepaths_var);
  parse_updatedb_conf ();
  parse_arguments (argc, argv);
  conf_prunefs = var_finish (&prunefs_var, &conf_prunefs_len);
  for (i = 0; i < conf_prunefs_len; i++)
    {
      char *p;

      for (p = conf_prunefs[i]; *p != 0; p++)
	*p = toupper((unsigned char)*p);
    }
  paths = var_finish (&prunepaths_var, &conf_prunepaths_len);
  conf_prunepaths = paths;
  gen_conf_block ();
  qsort (paths, conf_prunepaths_len, sizeof (*conf_prunepaths),
	 cmp_dir_path_pointers);
}
