/*
 * Ouroboros - Copyright (C) 2016 - 2023
 *
 * Lockfile for Ouroboros
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

#ifndef OUROBOROS_LOCKFILE_H
#define OUROBOROS_LOCKFILE_H

#include <sys/types.h>

struct lockfile;

struct lockfile * lockfile_create(void);

struct lockfile * lockfile_open(void);

void              lockfile_close(struct lockfile * lf);

void              lockfile_destroy(struct lockfile * lf);

pid_t             lockfile_owner(struct lockfile * lf);

#endif /* OUROBOROS_LOCKFILE_H */
