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

#ifndef CONF_H__
#define CONF_H__

#include <config.h>

#include <stddef.h>

/* 1 if locate(1) should check whether files are visible before reporting
   them */
extern _Bool conf_check_visibility;

/* Filesystems to skip, converted to uppercase and sorted by name */
extern char *const *conf_prunefs;
extern size_t conf_prunefs_len;

/* Paths to skip, sorted by name using dir_path_cmp () */
extern char *const *conf_prunepaths;
extern size_t conf_prunepaths_len;

/* Root of the directory tree to store in the database */
extern const char *conf_scan_root;

/* Absolute (not necessarily canonical) path to the database */
extern const char *conf_output;

/* 1 if file names should be written to stdout as they are found */
extern _Bool conf_verbose;

/* Configuration representation for the database configuration block */
extern const char *conf_block;
extern size_t conf_block_size;

/* Parse /etc/updatedb.conf and command-line arguments ARGC, ARGV.
   Exit on error or --help, --version. */
extern void conf_prepare (int argc, char *argv[]);

#endif
