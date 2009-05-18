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
#include <sys/param.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>

#include <lxc/lxc.h>
#include "confile.h"
#include "arguments.h"

static int my_parser(struct lxc_arguments* args, int c, char* arg)
{
	switch (c) {
	case 'f': args->rcfile = arg; break;
	}
	return 0;
}

static const struct option my_longopts[] = {
	{"rcfile", required_argument, 0, 'f'},
	LXC_COMMON_OPTIONS
};

static struct lxc_arguments my_args = {
	.progname = "lxc-create",
	.help     = "\
--name=NAME\n\
\n\
lxc-create creates a container with the identifier NAME\n\
\n\
Options :\n\
  -n, --name=NAME      NAME for name of the container\n\
  -f, --rcfile=FILE    Load configuration file FILE\n",
	.options  = my_longopts,
	.parser   = my_parser,
	.checker  = NULL,
};

int main(int argc, char *argv[])
{
	struct lxc_conf lxc_conf;
	int ret;

	ret = lxc_arguments_parse(&my_args, argc, argv);
	if (ret)
		return -1;

	if (lxc_log_init(my_args.log_file, my_args.log_priority,
			 my_args.progname, my_args.quiet))
		return -1;

	if (lxc_conf_init(&lxc_conf))
		return -1;

	if (my_args.rcfile && lxc_config_read(my_args.rcfile, &lxc_conf))
		return -1;

	return lxc_create(my_args.name, &lxc_conf);
}

