/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * The API to instruct the global Namespace Manager
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
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

#include <ouroboros/nsm.h>

int nsm_reg(char * name,
            char ** dafs,
            size_t dafs_size)
{
        (void) name;
        (void) dafs;
        (void) dafs_size;

        return -1;
}

int nsm_unreg(char * name,
              char ** dafs,
              size_t dafs_size)
{
        (void) name;
        (void) dafs;
        (void) dafs_size;


        return -1;
}

ssize_t nsm_resolve(char * name,
                    char ** dafs)
{
        (void) name;
        (void) dafs;

        return -1;
}
