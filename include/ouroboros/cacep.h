/*
 * Ouroboros - Copyright (C) 2016 - 2021
 *
 * The Common Application Connection Establishment Protocol
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

#ifndef OUROBOROS_CACEP_H
#define OUROBOROS_CACEP_H

#include <ouroboros/cdefs.h>
#include <ouroboros/proto.h>

#include <stdint.h>
#include <sys/types.h>

#define CACEP_BUF_STRLEN 64

struct conn_info {
        char                       comp_name[CACEP_BUF_STRLEN + 1];
        char                       protocol[CACEP_BUF_STRLEN + 1];
        uint32_t                   pref_version;
        enum proto_concrete_syntax pref_syntax;
        struct proto_field         fixed_conc_syntax[PROTO_MAX_FIELDS];
        size_t                     num_fields;
        uint64_t                   addr;
};

__BEGIN_DECLS

int cacep_snd(int                      fd,
              const struct conn_info * in);

int cacep_rcv(int                fd,
              struct conn_info * out);

__END_DECLS

#endif /* OUROBOROS_CACEP_H */
