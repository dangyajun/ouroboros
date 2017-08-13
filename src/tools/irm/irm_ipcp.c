/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * A tool to instruct the IRM daemon
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

#include <stdio.h>

#include "irm_ops.h"
#include "irm_utils.h"

static void usage(void)
{
        printf("Usage: irm ipcp [OPERATION]\n\n"
               "where OPERATION = {create destroy\n"
               "                   bootstrap enroll help}\n");
}

static int do_help(int argc, char **argv)
{
        (void) argc;
        (void) argv;

        usage();
        return 0;
}

static const struct cmd {
        const char * cmd;
        int (* func)(int argc, char ** argv);
} cmds[] = {
        { "create",    do_create_ipcp },
        { "destroy",   do_destroy_ipcp },
        { "bootstrap", do_bootstrap_ipcp },
        { "enroll",    do_enroll_ipcp },
        { "help",      do_help },
        { NULL,        NULL }
};

static int do_cmd(const char * argv0,
                  int argc,
                  char ** argv)
{
        const struct cmd * c;

        for (c = cmds; c->cmd; ++c) {
                if (matches(argv0, c->cmd) == 0)
                        return c->func(argc - 1, argv + 1);
        }

        fprintf(stderr, "\"%s\" is unknown, try \"irm ipcp help\".\n", argv0);

        return -1;
}

int ipcp_cmd(int argc, char ** argv)
{
        if (argc < 1) {
                usage();
                return -1;
        }

        return do_cmd(argv[0], argc, argv);
}
