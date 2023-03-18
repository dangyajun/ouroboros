/*
 * Ouroboros - Copyright (C) 2016 - 2023
 *
 * The IPC Resource Manager - Flows
 *
 *    Dimitri Staessens <dimitri@ouroboros.rocks>
 *    Sander Vrijders   <sander@ouroboros.rocks>
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

#ifndef OUROBOROS_IRMD_IRM_FLOW_H
#define OUROBOROS_IRMD_IRM_FLOW_H

#include <ouroboros/list.h>
#include <ouroboros/qos.h>
#include <ouroboros/shm_rbuff.h>
#include <ouroboros/utils.h>

#include <sys/types.h>
#include <pthread.h>
#include <time.h>

enum flow_state {
        FLOW_NULL = 0,
        FLOW_ALLOC_PENDING,
        FLOW_ALLOC_REQ_PENDING,
        FLOW_ALLOCATED,
        FLOW_DEALLOC_PENDING,
        FLOW_DESTROY
};

struct irm_flow {
        struct list_head   next;

        int                flow_id;

        pid_t              n_pid;
        pid_t              n_1_pid;

        qosspec_t          qs;
        time_t             mpl;
        buffer_t           data;

        struct shm_rbuff * n_rb;
        struct shm_rbuff * n_1_rb;

        struct timespec    t0;

        enum flow_state    state;
        pthread_cond_t     state_cond;
        pthread_mutex_t    state_lock;
};

struct irm_flow * irm_flow_create(pid_t     n_pid,
                                  pid_t     n_1_pid,
                                  int       flow_id,
                                  qosspec_t qs);

void              irm_flow_destroy(struct irm_flow * f);

enum flow_state   irm_flow_get_state(struct irm_flow * f);


void              irm_flow_set_state(struct irm_flow * f,
                                     enum flow_state   state);

int               irm_flow_wait_state(struct irm_flow * f,
                                      enum flow_state   state,
                                      struct timespec * timeo);

#endif /* OUROBOROS_IRMD_IRM_FLOW_H */
