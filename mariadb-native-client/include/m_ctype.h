/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA */

/*
  A better inplementation of the UNIX ctype(3) library.
  Notes:   my_global.h should be included before ctype.h
*/

#ifndef _m_ctype_h
#define _m_ctype_h

#include <ctype.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define CHARSET_DIR	"charsets/"
#define MY_CS_NAME_SIZE 32


/* we use the mysqlnd implementation */
typedef struct charset_info_st
{
  uint	nr; /* so far only 1 byte for charset */
  uint  state;
  char	*csname;
  char	*name;
  char  *comment;
  char  *dir;
  uint	char_minlen;
  uint	char_maxlen;
  uint 	(*mb_charlen)(uint c);
  uint 	(*mb_valid)(const char *start, const char *end);
} CHARSET_INFO;

extern const CHARSET_INFO  compiled_charsets[];
extern CHARSET_INFO *default_charset_info;

CHARSET_INFO *find_compiled_charset(uint cs_number);
CHARSET_INFO *find_compiled_charset_by_name(const char *name);

size_t mysql_cset_escape_quotes(const CHARSET_INFO *cset, char *newstr,  const char *escapestr, size_t escapestr_len);
size_t mysql_cset_escape_slashes(const CHARSET_INFO *cset, char *newstr, const char *escapestr, size_t escapestr_len);
#endif
