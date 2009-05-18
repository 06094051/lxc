/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <dlezcano at fr.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <stdio.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/types.h>

#include <lxc.h>

lxc_log_define(lxc_restart, lxc);

void usage(char *cmd)
{
	fprintf(stderr, "%s <statefile>\n", basename(cmd));
	fprintf(stderr, "\t -n <name>   : name of the container\n");
	fprintf(stderr, "\t[-o <logfile>]    : path of the log file\n");
	fprintf(stderr, "\t[-l <logpriority>]: log level priority\n");
	fprintf(stderr, "\t[-q ]             : be quiet\n");
	_exit(1);
}

int main(int argc, char *argv[])
{
	char *name = NULL;
	const char *log_file = NULL, *log_priority = NULL;
	int opt, nbargs = 0;
	int quiet = 0;

	while ((opt = getopt(argc, argv, "n:o:l:")) != -1) {
		switch (opt) {
		case 'n':
			name = optarg;
			break;
		case 'o':
			log_file = optarg;
			break;
		case 'l':
			log_priority = optarg;
			break;
		case 'q':
			quiet = 1;
			break;
		}

		nbargs++;
	}

	if (!name)
		usage(argv[0]);

	if (!argv[optind])
		usage(argv[0]);

	if (lxc_log_init(log_file, log_priority, basename(argv[0]), quiet))
		return 1;

	if (lxc_restart(name, argv[1], 0)) {
		ERROR("failed to restart %s", name);
		return 1;
	}

	return 0;
}
