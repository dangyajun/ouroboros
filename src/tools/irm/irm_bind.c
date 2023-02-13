/*
 * Ouroboros - Copyright (C) 2016 - 2023
 *
 * Bind names in the processing system
 *
 *    Dimitri Staessens <dimitri@ouroboros.rocks>
 *    Sander Vrijders   <sander@ouroboros.rocks>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>

#include <ouroboros/irm.h>

#include "irm_ops.h"
#include "irm_utils.h"

static void usage(void)
{
        printf("Usage: irm bind [OPERATION]\n"
               "\n"
               "where OPERATION = {program process ipcp help}\n");
}

static int do_help(int    argc,
                   char **argv)
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
        { "program", do_bind_program },
        { "process", do_bind_process },
        { "ipcp",    do_bind_ipcp },
        { "help",    do_help },
        { NULL,      NULL }
};

static int do_cmd(const char * argv0,
                  int          argc,
                  char **      argv)
{
        const struct cmd * c;

        for (c = cmds; c->cmd; ++c) {
                if (!matches(argv0, c->cmd))
                        return c->func(argc, argv);
        }

        fprintf(stderr, "\"%s\" is unknown, try \"irm bind help\".\n", argv0);

        return -1;
}

int bind_cmd(int     argc,
             char ** argv)
{
        if (argc < 1) {
                usage();
                return -1;
        }

        return do_cmd(argv[0], argc, argv);
}
