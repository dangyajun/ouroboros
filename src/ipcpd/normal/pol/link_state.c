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
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#define OUROBOROS_PREFIX "link-state-routing"

#include <ouroboros/dev.h>
#include <ouroboros/errno.h>
#include <ouroboros/fqueue.h>
#include <ouroboros/list.h>
#include <ouroboros/logs.h>
#include <ouroboros/notifier.h>
#include <ouroboros/rib.h>
#include <ouroboros/utils.h>

#include "ae.h"
#include "connmgr.h"
#include "graph.h"
#include "ipcp.h"
#include "link_state.h"
#include "pff.h"

#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>

#include "link_state.pb-c.h"
typedef LinkStateMsg link_state_msg_t;

#define RECALC_TIME    4
#define LS_UPDATE_TIME 15
#define LS_TIMEO       60
#define LSM_MAX_LEN    128
#define LSDB           "lsdb"

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif

struct routing_i {
        struct pff * pff;
        pthread_t    calculator;
};

/* TODO: link weight support. */
struct adjacency {
        struct list_head next;

        uint64_t         dst;
        uint64_t         src;

        time_t           stamp;
};

enum nb_type {
        NB_DT = 0,
        NB_MGMT
};

struct nb {
        struct list_head next;

        uint64_t         addr;
        int              fd;
        enum nb_type     type;
};

struct {
        struct list_head nbs;
        fset_t *         mgmt_set;

        struct list_head db;
        size_t           db_len;

        pthread_rwlock_t db_lock;

        struct graph *   graph;

        pthread_t        lsupdate;
        pthread_t        lsreader;
        pthread_t        listener;
} ls;

struct pol_routing_ops link_state_ops = {
        .init              = link_state_init,
        .fini              = link_state_fini,
        .routing_i_create  = link_state_routing_i_create,
        .routing_i_destroy = link_state_routing_i_destroy
};

static int str_adj(struct adjacency * adj,
                   char *             buf,
                   size_t             len)
{
        char        tmbuf[64];
        struct tm * tm;

        if (len < 256)
                return -1;

        tm = localtime(&adj->stamp);
        strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", tm);

        sprintf(buf,
                "src: %" PRIu64 "\n"
                "dst: %" PRIu64 "\n"
                "upd: %s\n",
                adj->src,
                adj->dst,
                tmbuf);

        return strlen(buf);
}

static int lsdb_read(const char * path,
                     char *       buf,
                     size_t       len)
{
        struct list_head * p;
        char               entry[RIB_PATH_LEN + 1];

        pthread_rwlock_rdlock(&ls.db_lock);

        if (ls.db_len == 0) {
                pthread_rwlock_unlock(&ls.db_lock);
                return -EPERM;
        }

        list_for_each(p, &ls.db) {
                struct adjacency * a = list_entry(p, struct adjacency, next);
                sprintf(entry, "%" PRIu64 ".%" PRIu64, a->src, a->dst);
                if (strcmp(entry, path) == 0) {
                        len = str_adj(a, buf, len);
                        pthread_rwlock_unlock(&ls.db_lock);
                        return len;
                }
        }

        pthread_rwlock_unlock(&ls.db_lock);

        return -1;
}

static int lsdb_readdir(char *** buf)
{
        struct list_head * p;
        char               entry[RIB_PATH_LEN + 1];
        ssize_t            idx = 0;

        pthread_rwlock_rdlock(&ls.db_lock);

        if (ls.db_len == 0) {
                pthread_rwlock_unlock(&ls.db_lock);
                return 0;
        }

        *buf = malloc(sizeof(**buf) * ls.db_len);
        if (*buf == NULL) {
                pthread_rwlock_unlock(&ls.db_lock);
                return -ENOMEM;
        }

        list_for_each(p, &ls.db) {
                struct adjacency * a = list_entry(p, struct adjacency, next);
                sprintf(entry, "%" PRIu64 ".%" PRIu64, a->src, a->dst);
                (*buf)[idx] = malloc(strlen(entry) + 1);
                if ((*buf)[idx] == NULL) {
                        ssize_t j;
                        for (j = 0; j < idx; ++j)
                                free(*buf[j]);
                        free(buf);
                        pthread_rwlock_unlock(&ls.db_lock);
                        return -ENOMEM;
                }

                strcpy((*buf)[idx], entry);

                idx++;
        }

        pthread_rwlock_unlock(&ls.db_lock);

        return idx;
}

static struct rib_ops r_ops = {
        .read    = lsdb_read,
        .readdir = lsdb_readdir
};

static int lsdb_add_nb(uint64_t     addr,
                       int          fd,
                       enum nb_type type)
{
        struct list_head * p;
        struct nb *        nb;

        pthread_rwlock_wrlock(&ls.db_lock);

        list_for_each(p, &ls.nbs) {
                struct nb * el = list_entry(p, struct nb, next);
                if (el->addr == addr && el->type == type) {
                        log_dbg("Already know %s neighbor %" PRIu64 ".",
                                type == NB_DT ? "dt" : "mgmt", addr);
                        if (el->fd != fd) {
                                log_warn("Existing neighbor assigned new fd.");
                                el->fd = fd;
                        }
                        pthread_rwlock_unlock(&ls.db_lock);
                        return -EPERM;
                }

                if (addr > el->addr)
                        break;
        }

        nb = malloc(sizeof(*nb));
        if (nb == NULL) {
                pthread_rwlock_unlock(&ls.db_lock);
                return -ENOMEM;
        }

        nb->addr  = addr;
        nb->fd    = fd;
        nb->type  = type;

        list_add_tail(&nb->next, p);

        log_dbg("Type %s neighbor %" PRIu64 " added.",
                nb->type == NB_DT ? "dt" : "mgmt", addr);

        pthread_rwlock_unlock(&ls.db_lock);

        return 0;
}

static int lsdb_del_nb(uint64_t     addr,
                       int          fd)
{
        struct list_head * p;
        struct list_head * h;

        pthread_rwlock_wrlock(&ls.db_lock);

        list_for_each_safe(p, h, &ls.nbs) {
                struct nb * nb = list_entry(p, struct nb, next);
                if (nb->addr == addr && nb->fd == fd) {
                        list_del(&nb->next);
                        pthread_rwlock_unlock(&ls.db_lock);
                        log_dbg("Type %s neighbor %" PRIu64 " deleted.",
                                nb->type == NB_DT ? "dt" : "mgmt", addr);
                        free(nb);
                        return 0;
                }
        }

        pthread_rwlock_unlock(&ls.db_lock);

        return -EPERM;
}

static int lsdb_add_link(uint64_t    src,
                         uint64_t    dst,
                         qosspec_t * qs)
{
        struct list_head * p;
        struct adjacency * adj;
        struct timespec    now;

        clock_gettime(CLOCK_REALTIME_COARSE, &now);

        pthread_rwlock_wrlock(&ls.db_lock);

        list_for_each(p, &ls.db) {
                struct adjacency * a = list_entry(p, struct adjacency, next);
                if (a->dst == dst && a->src == src) {
                        a->stamp = now.tv_sec;
                        pthread_rwlock_unlock(&ls.db_lock);
                        return 0;
                }

                if (a->dst > dst || (a->dst == dst && a->src > src))
                        break;
        }

        adj = malloc(sizeof(*adj));
        if (adj == NULL) {
                pthread_rwlock_unlock(&ls.db_lock);
                return -ENOMEM;
        }

        adj->dst   = dst;
        adj->src   = src;
        adj->stamp = now.tv_sec;

        list_add_tail(&adj->next, p);

        ls.db_len++;

        if (graph_update_edge(ls.graph, src, dst, *qs))
                log_warn("Failed to add edge to graph.");

        pthread_rwlock_unlock(&ls.db_lock);

        return 0;
}

static int lsdb_del_link(uint64_t src,
                         uint64_t dst)
{
        struct list_head * p;
        struct list_head * h;

        pthread_rwlock_wrlock(&ls.db_lock);

        list_for_each_safe(p, h, &ls.db) {
                struct adjacency * a = list_entry(p, struct adjacency, next);
                if (a->dst == dst && a->src == src) {
                        list_del(&a->next);
                        if (graph_del_edge(ls.graph, src, dst))
                                log_warn("Failed to delete edge from graph.");

                        ls.db_len--;

                        pthread_rwlock_unlock(&ls.db_lock);
                        free(a);
                        return 0;
                }
        }

        pthread_rwlock_unlock(&ls.db_lock);

        return -EPERM;
}

static int nbr_to_fd(uint64_t addr)
{
        struct list_head * p;

        pthread_rwlock_rdlock(&ls.db_lock);

        list_for_each(p, &ls.nbs) {
                struct nb * nb = list_entry(p, struct nb, next);
                if (nb->addr == addr && nb->type == NB_DT) {
                        pthread_rwlock_unlock(&ls.db_lock);
                        return nb->fd;
                }
        }

        pthread_rwlock_unlock(&ls.db_lock);

        return -1;
}

static void * calculate_pff(void * o)
{
        struct routing_i *      instance;
        struct routing_table ** table;
        ssize_t                 n_table;
        int                     i;
        int                     fd;

        instance = (struct routing_i *) o;

        while (true) {
                table = NULL;
                n_table = graph_routing_table(ls.graph,
                                              ipcpi.dt_addr, &table);
                if (n_table < 0) {
                        sleep(RECALC_TIME);
                        continue;
                }

                pff_lock(instance->pff);

                pff_flush(instance->pff);

                for (i = 0; i < n_table; i++) {
                        fd = nbr_to_fd(table[i]->nhop);
                        if (fd == -1)
                                continue;

                        pff_add(instance->pff, table[i]->dst, fd);
                }

                pff_unlock(instance->pff);

                freepp(struct routing_table, table, n_table);
                sleep(RECALC_TIME);
        }

        return (void *) 0;
}

static void send_lsm(uint64_t src,
                     uint64_t dst)
{
        uint8_t            buf[LSM_MAX_LEN];
        link_state_msg_t   lsm = LINK_STATE_MSG__INIT;
        size_t             len;
        struct list_head * p;

        lsm.d_addr = dst;
        lsm.s_addr = src;

        len = link_state_msg__get_packed_size(&lsm);

        assert(len <= LSM_MAX_LEN);

        link_state_msg__pack(&lsm, buf);

        list_for_each(p, &ls.nbs) {
                struct nb * nb = list_entry(p, struct nb, next);
                if (nb->type == NB_MGMT)
                        flow_write(nb->fd, buf, len);
        }
}

static void * lsupdate(void * o)
{
        struct list_head * p;
        struct list_head * h;
        struct timespec    now;

        (void) o;

        while (true) {
                clock_gettime(CLOCK_REALTIME_COARSE, &now);

                pthread_rwlock_rdlock(&ls.db_lock);

                pthread_cleanup_push((void (*) (void *)) pthread_rwlock_unlock,
                                     (void *) &ls.db_lock);

                list_for_each_safe(p, h, &ls.db) {
                        struct adjacency * adj;
                        adj = list_entry(p, struct adjacency, next);
                        if (now.tv_sec - adj->stamp > LS_TIMEO) {
                                list_del(&adj->next);
                                log_dbg("%" PRIu64 " - %" PRIu64" timed out.",
                                        adj->src, adj->dst);
                                if (graph_del_edge(ls.graph, adj->src,
                                                   adj->dst))
                                        log_dbg("Failed to delete edge.");
                                free(adj);
                                continue;
                        }

                        if (adj->src == ipcpi.dt_addr) {
                                send_lsm(adj->src, adj->dst);
                                adj->stamp = now.tv_sec;
                        }
                }

                pthread_cleanup_pop(true);

                sleep(LS_UPDATE_TIME);
        }

        return (void *) 0;
}

static void * ls_conn_handle(void * o)
{
        struct conn conn;

        (void) o;

        while (true) {
                if (connmgr_wait(AEID_MGMT, &conn)) {
                        log_err("Failed to get next MGMT connection.");
                        continue;
                }

                /* NOTE: connection acceptance policy could be here. */

                notifier_event(NOTIFY_MGMT_CONN_ADD, &conn);
        }

        return 0;
}


static void forward_lsm(uint8_t * buf,
                        size_t    len,
                        int       in_fd)
{
        struct list_head * p;

        pthread_rwlock_rdlock(&ls.db_lock);

        list_for_each(p, &ls.nbs) {
                struct nb * nb = list_entry(p, struct nb, next);
                if (nb->type == NB_MGMT && nb->fd != in_fd)
                        flow_write(nb->fd, buf, len);
        }

        pthread_rwlock_unlock(&ls.db_lock);
}

static void * lsreader(void * o)
{
        fqueue_t * fq;
        int        ret;
        uint8_t    buf[LSM_MAX_LEN];
        size_t     len;
        int        fd;
        qosspec_t  qs;

        (void) o;

        memset(&qs, 0, sizeof(qs));

        fq = fqueue_create();
        if (fq == NULL)
                return (void *) -1;

        pthread_cleanup_push((void (*) (void *)) fqueue_destroy,
                             (void *) fq);

        while (true) {
                ret = fevent(ls.mgmt_set, fq, NULL);
                if (ret < 0) {
                        log_warn("Event error: %d.", ret);
                        continue;
                }

                while ((fd = fqueue_next(fq)) >= 0) {
                        link_state_msg_t * msg;
                        len = flow_read(fd, buf, LSM_MAX_LEN);
                        if (len <= 0)
                                continue;

                        msg = link_state_msg__unpack(NULL, len, buf);
                        if (msg == NULL) {
                                log_dbg("Failed to unpack link state message.");
                                continue;
                        }

                        lsdb_add_link(msg->s_addr, msg->d_addr, &qs);

                        link_state_msg__free_unpacked(msg, NULL);

                        forward_lsm(buf, len, fd);
                }
        }

        pthread_cleanup_pop(true);

        return (void *) 0;
}

static void handle_event(void *       self,
                         int          event,
                         const void * o)
{
        /* FIXME: Apply correct QoS on graph */
        struct conn * c;
        qosspec_t     qs;

        (void) self;

        c = (struct conn *) o;

        memset(&qs, 0, sizeof(qs));

        switch (event) {
        case NOTIFY_DT_CONN_ADD:
                if (lsdb_add_nb(c->conn_info.addr, c->flow_info.fd, NB_DT))
                        log_dbg("Failed to add neighbor to LSDB.");

                if (lsdb_add_link(ipcpi.dt_addr, c->conn_info.addr, &qs))
                        log_dbg("Failed to add adjacency to LSDB.");
                send_lsm(ipcpi.dt_addr, c->conn_info.addr);
                break;
        case NOTIFY_DT_CONN_DEL:
                if (lsdb_del_nb(c->conn_info.addr, c->flow_info.fd))
                        log_dbg("Failed to delete neighbor from LSDB.");

                if (lsdb_del_link(ipcpi.dt_addr, c->conn_info.addr))
                        log_dbg("Local link was not in LSDB.");
                break;
        case NOTIFY_DT_CONN_QOS:
                log_dbg("QoS changes currently unsupported.");
                break;
        case NOTIFY_MGMT_CONN_ADD:
                fset_add(ls.mgmt_set, c->flow_info.fd);
                if (lsdb_add_nb(c->conn_info.addr, c->flow_info.fd, NB_MGMT))
                        log_warn("Failed to add mgmt neighbor to LSDB.");
                break;
        case NOTIFY_MGMT_CONN_DEL:
                fset_del(ls.mgmt_set, c->flow_info.fd);
                if (lsdb_del_nb(c->conn_info.addr, c->flow_info.fd))
                        log_warn("Failed to add mgmt neighbor to LSDB.");
                break;
        default:
                log_info("Unknown routing event.");
                break;
        }
}

struct routing_i * link_state_routing_i_create(struct pff * pff)
{
        struct routing_i * tmp;

        assert(pff);

        tmp = malloc(sizeof(*tmp));
        if (tmp == NULL)
                return NULL;

        tmp->pff = pff;

        if (pthread_create(&tmp->calculator, NULL, calculate_pff, tmp)) {
                free(tmp);
                return NULL;
        }

        return tmp;
}

void link_state_routing_i_destroy(struct routing_i * instance)
{
        assert(instance);

        pthread_cancel(instance->calculator);

        pthread_join(instance->calculator, NULL);

        free(instance);
}

int link_state_init(void)
{
        struct conn_info info;

        memset(&info, 0, sizeof(info));

        strcpy(info.ae_name, LS_AE);
        strcpy(info.protocol, LS_PROTO);
        info.pref_version = 1;
        info.pref_syntax  = PROTO_GPB;
        info.addr         = ipcpi.dt_addr;

        ls.graph = graph_create();
        if (ls.graph == NULL)
                goto fail_graph;

        if (notifier_reg(handle_event, NULL))
                goto fail_notifier_reg;

        if (pthread_rwlock_init(&ls.db_lock, NULL))
                goto fail_db_lock_init;

        if (connmgr_ae_init(AEID_MGMT, &info))
                goto fail_connmgr_ae_init;

        ls.mgmt_set = fset_create();
        if (ls.mgmt_set == NULL)
                goto fail_fset_create;

        list_head_init(&ls.db);
        list_head_init(&ls.nbs);

        if (pthread_create(&ls.lsupdate, NULL, lsupdate, NULL))
                goto fail_pthread_create_lsupdate;

        if (pthread_create(&ls.lsreader, NULL, lsreader, NULL))
                goto fail_pthread_create_lsreader;

        if (pthread_create(&ls.listener, NULL, ls_conn_handle, NULL))
                goto fail_pthread_create_listener;

        ls.db_len = 0;

        rib_reg(LSDB, &r_ops);

        return 0;

 fail_pthread_create_listener:
        pthread_cancel(ls.lsreader);
        pthread_join(ls.lsreader, NULL);
 fail_pthread_create_lsreader:
        pthread_cancel(ls.lsupdate);
        pthread_join(ls.lsupdate, NULL);
 fail_pthread_create_lsupdate:
        fset_destroy(ls.mgmt_set);
 fail_fset_create:
        connmgr_ae_fini(AEID_MGMT);
 fail_connmgr_ae_init:
        pthread_rwlock_destroy(&ls.db_lock);
 fail_db_lock_init:
        notifier_unreg(handle_event);
 fail_notifier_reg:
        graph_destroy(ls.graph);
 fail_graph:
        return -1;
}

void link_state_fini(void)
{
        struct list_head * p;
        struct list_head * h;

        rib_unreg(LSDB);

        pthread_cancel(ls.listener);
        pthread_join(ls.listener, NULL);

        pthread_cancel(ls.lsreader);
        pthread_join(ls.lsreader, NULL);

        pthread_cancel(ls.lsupdate);
        pthread_join(ls.lsupdate, NULL);

        fset_destroy(ls.mgmt_set);

        connmgr_ae_fini(AEID_MGMT);

        graph_destroy(ls.graph);

        pthread_rwlock_wrlock(&ls.db_lock);

        list_for_each_safe(p, h, &ls.db) {
                struct adjacency * a = list_entry(p, struct adjacency, next);
                list_del(&a->next);
                free(a);
        }

        pthread_rwlock_unlock(&ls.db_lock);

        pthread_rwlock_destroy(&ls.db_lock);

        notifier_unreg(handle_event);
}
