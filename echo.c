/* vi: set sw=4 ts=4: */
/*
 * echo implementation for busybox
 *
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Original copyright notice is retained at the end of this file.
 */

#include "internal.h"
#include <stdio.h>

extern int 
echo_main(int argc, char** argv)
{
	register char **ap;
	register char *p;
	register char c;
	int nflag = 0;
	int eflag = 0;

	ap = argv;
	if (argc)
		ap++;
	while ((p = *ap) != NULL && *p == '-') {
		if (strcmp(p, "-n")==0) {
			nflag = 1;
		} else if (strcmp(p, "-e")==0) {
			eflag = 1;
		} else if (strcmp(p, "-E")==0) {
			eflag = 0;
		}
		else break;
		ap++;
	}
	while ((p = *ap++) != NULL) {
		while ((c = *p++) != '\0') {
			if (c == '\\' && eflag) {
				switch (c = *p++) {
				case 'a':  c = '\007'; break;
				case 'b':  c = '\b';  break;
				case 'c':  exit( 0);		/* exit */
				case 'f':  c = '\f';  break;
				case 'n':  c = '\n';  break;
				case 'r':  c = '\r';  break;
				case 't':  c = '\t';  break;
				case 'v':  c = '\v';  break;
				case '\\':  break;		/* c = '\\' */
				case '0': case '1': case '2': case '3':
				case '4': case '5': case '6': case '7':
					c -= '0';
					if (*p >= '0' && *p <= '7')
						c = c * 8 + (*p++ - '0');
					if (*p >= '0' && *p <= '7')
					c = c * 8 + (*p++ - '0');
					break;
				default:
					p--;
					break;
				}
			}
			putchar(c);
		}
		if (*ap)
			putchar(' ');
	}
	if (! nflag)
		putchar('\n');
	fflush(stdout);
	exit( 0);
}

/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)echo.c	8.1 (Berkeley) 5/31/93
 */


