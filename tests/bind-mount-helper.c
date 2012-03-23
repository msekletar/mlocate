/* Bind mount test suite helper.

Copyright (C) 2012 Red Hat, Inc. All rights reserved.
This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmač <mitr@redhat.com> */
#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#undef NDEBUG
#include <assert.h>

#include "../src/bind-mount.h"
#include "../src/conf.h"
#include "../src/lib.h"

bool conf_debug_pruning; /* = false; */

int
main (int argc, char *argv[])
{
  struct string_list paths;
  int i;
  size_t j;

  assert (argc >= 2);
  i = 1;
  if (strcmp (argv[i], "-d") == 0)
    {
      conf_debug_pruning = true;
      i++;
    }
  assert (argc > i);

  bind_mount_init (argv[i]);

  memset (&paths, 0, sizeof (paths));
  for (i++; i < argc; i++)
    string_list_append (&paths, argv[i]);
  dir_path_cmp_init ();
  string_list_dir_path_sort (&paths);
  for (j = 0; j < paths.len; j++)
    {
      const char *p;

      p = paths.entries[j];
      printf ("%s %s\n", is_bind_mount (p) ? "yes" : "no ", p);
    }
  return EXIT_SUCCESS;
}
