/* Copyright (C) 1995,1996 Robert de Bath <rdebath@cix.compulink.co.uk>
 * This file is part of the Linux-8086 C library and is distributed
 * under the GNU Library General Public License.
 */

#include <string.h>
#include <ctype.h>

int strncasecmp(register const char *s, register const char *d, size_t l)
{
   /* For length 0 return 0. This is an undefined case but our internal
      users depend on it returning 0 */
   while(l>0)
   {
      if( *s != *d )
      {
	 if( tolower(*s) != tolower(*d) )
	    return *s - *d;
      }
      else
	 if( *s == '\0' ) return 0;
      s++; d++; l--;
   }
   return 0;
}

