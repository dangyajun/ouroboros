/*
 * Ouroboros - Copyright (C) 2016
 *
 * The Relaying and Multiplexing task
 *
 *    Sander Vrijders <sander.vrijders@intec.ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#ifndef OUROBOROS_IPCP_RMT_H
#define OUROBOROS_IPCP_RMT_H

#include <ouroboros/shm_rdrbuff.h>
#include <ouroboros/utils.h>

#include "dt_const.h"
#include "shm_pci.h"

int rmt_init(uint32_t address);
int rmt_fini();

int rmt_dt_flow(int           fd,
                enum qos_cube qos);

/* Hand PDU to RMT, SDU from N+1 */
int rmt_frct_write_sdu(struct pci *         pci,
                       struct shm_du_buff * sdb);

/* Hand PDU to RMT, SDU from N */
int rmt_frct_write_buf(struct pci * pci,
                       buffer_t *   buf);

#endif
