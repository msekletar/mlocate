/* Database file format.
   Copyright (C) 2005 FIXME

   You can use this file without restriction.

   This file is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.

   (This file defines a file format, so even the LGPL seems too restrictive.
   Share and enjoy.) */

#ifndef DB_H__
#define DB_H__

#include <stdint.h>

/* FIXME: See mlocate.db(5) */

/* File header */
struct db_header
{
  uint8_t magic[8];		/* See DB_MAGIC below */
  uint8_t version;	       /* File format version, see DB_VERSION* below */
};
/* Followed by an EOF-terminated sequence of directories */

/* Contains a '\0' byte to unambiguously mark the file as a binary file. */
#define DB_MAGIC { '\0', 'm', 'l', 'o', 'c', 'a', 't', 'e' }

#define DB_VERSION_0 0x00

/* Directory header */
struct db_directory
{
  uint64_t ctime;		/* st_ctime of the directory in big endian */
  uint32_t entries;		/* Number of entries in big endian */
  /* Followed by NUL-terminated absolute path of the directory */
};
/* Followed by ENTRIES directory entries, sorted by name using strcmp () */

/* Directory entry */
struct db_entry
{
  uint8_t is_directory;
  /* Followed by NUL-terminated name */
};

#endif
