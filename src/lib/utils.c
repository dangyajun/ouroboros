/*
 * Ouroboros - Copyright (C) 2016 - 2024
 *
 * Handy utilities
 *
 *    Dimitri Staessens <dimitri@ouroboros.rocks>
 *    Sander Vrijders   <sander@ouroboros.rocks>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

#define _POSIX_C_SOURCE 200809L

#include <ouroboros/utils.h>

#include <stdlib.h>
#include <string.h>

int n_digits(unsigned i)
{
    int n = 1;

    while (i > 9) {
        ++n;
        i /= 10;
    }

    return n;
}

char * path_strip(const char * src)
{
        char * dst;

        if (src == NULL)
                return NULL;

        dst = (char *) src + strlen(src);

        while (dst > src && *dst != '/')
                --dst;

        if (*dst == '/')
                ++dst;

        return dst;
}

size_t argvlen(const char ** argv)
{
        size_t argc   = 0;

        if (argv == NULL)
                return 0;

        while (*argv++ != NULL)
                argc++;

        return argc;
}

void argvfree(char ** argv)
{
        char ** argv_dup;

        if (argv == NULL)
                return;

        argv_dup = argv;
        while (*argv_dup != NULL)
                free(*(argv_dup++));

        free(argv);
}

char ** argvdup(char ** argv)
{
        int     argc = 0;
        char ** argv_dup = argv;
        int     i;

        if (argv == NULL)
                return NULL;

        while (*(argv_dup++) != NULL)
                argc++;

        argv_dup = malloc((argc + 1) * sizeof(*argv_dup));
        if (argv_dup == NULL)
                return NULL;

        for (i = 0; i < argc; ++i) {
                argv_dup[i] = strdup(argv[i]);
                if (argv_dup[i] == NULL) {
                        argvfree(argv_dup);
                        return NULL;
                }
        }

        argv_dup[argc] = NULL;
        return argv_dup;
}
