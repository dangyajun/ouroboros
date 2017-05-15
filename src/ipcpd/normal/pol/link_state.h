/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Link state routing policy
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef OUROBOROS_IPCPD_NORMAL_POL_LINK_STATE_H
#define OUROBOROS_IPCPD_NORMAL_POL_LINK_STATE_H

#include "pol-routing-ops.h"

int                link_state_init(struct nbs * nbs);

void               link_state_fini(void);

struct routing_i * link_state_routing_i_create(struct pff * pff);

void               link_state_routing_i_destroy(struct routing_i * instance);

struct pol_routing_ops link_state_ops = {
        .init              = link_state_init,
        .fini              = link_state_fini,
        .routing_i_create  = link_state_routing_i_create,
        .routing_i_destroy = link_state_routing_i_destroy
};

#endif /* OUROBOROS_IPCPD_NORMAL_POL_LINK_STATE_H */
