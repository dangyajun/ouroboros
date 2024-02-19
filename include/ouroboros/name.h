/*
 * Ouroboros - Copyright (C) 2016 - 2024
 *
 * Names
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

#ifndef OUROBOROS_NAME_H
#define OUROBOROS_NAME_H

#define NAME_SIZE 255
#define BIND_AUTO 0x01

enum pol_balance {
        LB_RR = 0,
        LB_SPILL,
        LB_INVALID
};

struct name_info {
        char             name[NAME_SIZE + 1];
        enum pol_balance pol_lb;
};

#endif /* OUROBOROS_NAME_H */
