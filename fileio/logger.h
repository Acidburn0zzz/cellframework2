/*  -- Cellframework Mk.II -  Open framework to abstract the common tasks related to
 *                            PS3 application development.
 *
 *  Copyright (C) 2010-2012
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _LOGGER_H_
#define _LOGGER_H_

#define BUFSIZE	(64 * 1024)
#define TCPDUMP_FILE	(SYS_APP_HOME "/tcpdump.dump")
#define TCPDUMP_STACKSIZE	(16 * 1024)
#define TCPDUMP_PRIO	(2048)

void logger_init (void);
void logger_shutdown (void);
void net_send(const char *__format,...);

#endif
