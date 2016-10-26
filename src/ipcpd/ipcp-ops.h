/*
 * Ouroboros - Copyright (C) 2016
 *
 * IPC process ops
 *
 *    Dimitri Staessens <dimitri.staessens@intec.ugent.be>
 *    Sander Vrijders   <sander.vrijders@intec.ugent.be>
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

#ifndef IPCPD_IPCP_OPS_H
#define IPCPD_IPCP_OPS_H

#include <ouroboros/irm_config.h>
#include <ouroboros/shared.h>

struct ipcp_ops {
        int   (* ipcp_bootstrap)(struct dif_config * conf);

        int   (* ipcp_enroll)(char * dif_name);

        int   (* ipcp_name_reg)(char * name);

        int   (* ipcp_name_unreg)(char * name);

        int   (* ipcp_name_query)(char * name);

        int   (* ipcp_flow_alloc)(int           fd,
                                  char *        dst_ap_name,
                                  char *        src_ae_name,
                                  enum qos_cube qos);

        int   (* ipcp_flow_alloc_resp)(int fd,
                                       int response);

        int   (* ipcp_flow_dealloc)(int fd);
};

#endif /* IPCPD_IPCP_OPS_H */
