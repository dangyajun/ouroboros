/*
 * Ouroboros - Copyright (C) 2016 - 2021
 *
 * Distributed Hash Table based on Kademlia
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

#if defined(__linux__) || defined(__CYGWIN__)
#define _DEFAULT_SOURCE
#else
#define _POSIX_C_SOURCE 200112L
#endif

#include "config.h"

#define DHT              "dht"
#define OUROBOROS_PREFIX DHT

#include <ouroboros/hash.h>
#include <ouroboros/ipcp-dev.h>
#include <ouroboros/bitmap.h>
#include <ouroboros/errno.h>
#include <ouroboros/logs.h>
#include <ouroboros/list.h>
#include <ouroboros/notifier.h>
#include <ouroboros/random.h>
#include <ouroboros/time_utils.h>
#include <ouroboros/tpm.h>
#include <ouroboros/utils.h>
#include <ouroboros/pthread.h>

#include "common/connmgr.h"
#include "dht.h"
#include "dt.h"
#include "ipcp.h"
#include "ops.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <limits.h>

#include "kademlia.pb-c.h"
typedef KadMsg kad_msg_t;
typedef KadContactMsg kad_contact_msg_t;

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif

#define DHT_MAX_REQS  2048 /* KAD recommends rnd(), bmp can be changed.    */
#define KAD_ALPHA     3    /* Parallel factor, proven optimal value.       */
#define KAD_K         8    /* Replication factor, MDHT value.              */
#define KAD_T_REPL    900  /* Replication time, tied to k. MDHT value.     */
#define KAD_T_REFR    900  /* Refresh time stale bucket, MDHT value.       */
#define KAD_T_JOIN    8    /* Response time to wait for a join.            */
#define KAD_T_RESP    5    /* Response time to wait for a response.        */
#define KAD_R_PING    2    /* Ping retries before declaring peer dead.     */
#define KAD_QUEER     15   /* Time to declare peer questionable.           */
#define KAD_BETA      8    /* Bucket split factor, must be 1, 2, 4 or 8.   */
#define KAD_RESP_RETR 6    /* Number of retries on sending a response.     */
#define KAD_JOIN_RETR 8    /* Number of retries sending a join.            */
#define KAD_JOIN_INTV 1    /* Time (seconds) between join retries.         */
#define HANDLE_TIMEO  1000 /* Timeout for dht_handle_packet tpm check (ms) */
#define DHT_RETR_ADDR 1    /* Number of addresses to return on retrieve    */

enum dht_state {
        DHT_INIT = 0,
        DHT_SHUTDOWN,
        DHT_JOINING,
        DHT_RUNNING,
};

enum kad_code {
        KAD_JOIN = 0,
        KAD_FIND_NODE,
        KAD_FIND_VALUE,
        /* Messages without a response below. */
        KAD_STORE,
        KAD_RESPONSE
};

enum kad_req_state {
        REQ_NULL = 0,
        REQ_INIT,
        REQ_PENDING,
        REQ_RESPONSE,
        REQ_DONE,
        REQ_DESTROY
};

enum lookup_state {
        LU_NULL = 0,
        LU_INIT,
        LU_PENDING,
        LU_UPDATE,
        LU_COMPLETE,
        LU_DESTROY
};

struct kad_req {
        struct list_head   next;

        uint32_t           cookie;
        enum kad_code      code;
        uint8_t *          key;
        uint64_t           addr;

        enum kad_req_state state;
        pthread_cond_t     cond;
        pthread_mutex_t    lock;

        time_t             t_exp;
};

struct cookie_el {
        struct list_head next;

        uint32_t         cookie;
};

struct lookup {
        struct list_head  next;

        struct list_head  cookies;

        uint8_t *         key;

        struct list_head  contacts;
        size_t            n_contacts;

        uint64_t *        addrs;
        size_t            n_addrs;

        enum lookup_state state;
        pthread_cond_t    cond;
        pthread_mutex_t   lock;
};

struct val {
        struct list_head next;

        uint64_t         addr;

        time_t           t_exp;
        time_t           t_rep;
};

struct ref_entry {
        struct list_head next;

        uint8_t *        key;

        time_t           t_rep;
};

struct dht_entry {
        struct list_head next;

        uint8_t *        key;
        size_t           n_vals;
        struct list_head vals;
};

struct contact {
        struct list_head next;

        uint8_t *        id;
        uint64_t         addr;

        size_t           fails;
        time_t           t_seen;
};

struct bucket {
        struct list_head contacts;
        size_t           n_contacts;

        struct list_head alts;
        size_t           n_alts;

        time_t           t_refr;

        size_t           depth;
        uint8_t          mask;

        struct bucket *  parent;
        struct bucket *  children[1L << KAD_BETA];
};

struct cmd {
        struct list_head     next;

        struct shm_du_buff * sdb;
};

struct dir_ops dht_dir_ops = {
        .create       = dht_create,
        .destroy      = dht_destroy,
        .bootstrap    = dht_bootstrap,
        .reg          = dht_reg,
        .unreg        = dht_unreg,
        .query        = dht_query,
        .wait_running = dht_wait_running
};

struct dht {
        size_t           alpha;
        size_t           b;
        size_t           k;

        time_t           t_expire;
        time_t           t_refresh;
        time_t           t_replic;
        time_t           t_repub;

        uint8_t *        id;
        uint64_t         addr;

        struct bucket *  buckets;

        struct list_head entries;

        struct list_head refs;

        struct list_head lookups;

        struct list_head requests;
        struct bmp *     cookies;

        enum dht_state   state;
        struct list_head cmds;
        pthread_cond_t   cond;
        pthread_mutex_t  mtx;

        pthread_rwlock_t lock;

        uint64_t         eid;

        struct tpm *     tpm;

        pthread_t        worker;
};

struct join_info {
        struct dht * dht;
        uint64_t     addr;
};

struct packet_info {
        struct dht *         dht;
        struct shm_du_buff * sdb;
};

static uint8_t * dht_dup_key(const uint8_t * key,
                             size_t          len)
{
        uint8_t * dup;

        dup = malloc(sizeof(*dup) * len);
        if (dup == NULL)
                return NULL;

        memcpy(dup, key, len);

        return dup;
}

static enum dht_state dht_get_state(struct dht * dht)
{
        enum dht_state state;

        pthread_mutex_lock(&dht->mtx);

        state = dht->state;

        pthread_mutex_unlock(&dht->mtx);

        return state;
}

static int dht_set_state(struct dht *   dht,
                         enum dht_state state)
{
        pthread_mutex_lock(&dht->mtx);

        if (state == DHT_JOINING && dht->state != DHT_INIT) {
                 pthread_mutex_unlock(&dht->mtx);
                 return -1;
        }

        dht->state = state;

        pthread_cond_broadcast(&dht->cond);

        pthread_mutex_unlock(&dht->mtx);

        return 0;
}

int dht_wait_running(void * dir)
{
        struct dht * dht;
        int          ret = 0;

        dht = (struct dht *) dir;

        pthread_mutex_lock(&dht->mtx);

        pthread_cleanup_push(__cleanup_mutex_unlock, &dht->mtx);

        while (dht->state == DHT_JOINING)
                pthread_cond_wait(&dht->cond, &dht->mtx);

        if (dht->state != DHT_RUNNING)
                ret = -1;

        pthread_cleanup_pop(true);

        return ret;
}

static uint8_t * create_id(size_t len)
{
        uint8_t * id;

        id = malloc(len);
        if (id == NULL)
                return NULL;

        if (random_buffer(id, len) < 0) {
                free(id);
                return NULL;
        }

        return id;
}

static void kad_req_create(struct dht * dht,
                           kad_msg_t *  msg,
                           uint64_t     addr)
{
        struct kad_req *   req;
        pthread_condattr_t cattr;
        struct timespec    t;
        size_t             b;

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        req = malloc(sizeof(*req));
        if (req == NULL)
                goto fail_malloc;

        list_head_init(&req->next);

        req->t_exp  = t.tv_sec + KAD_T_RESP;
        req->addr   = addr;
        req->state  = REQ_INIT;
        req->cookie = msg->cookie;
        req->code   = msg->code;
        req->key    = NULL;

        pthread_rwlock_rdlock(&dht->lock);
        b = dht->b;
        pthread_rwlock_unlock(&dht->lock);

        if (msg->has_key) {
                req->key = dht_dup_key(msg->key.data, b);
                if (req->key == NULL)
                        goto fail_dup_key;
        }

        if (pthread_mutex_init(&req->lock, NULL))
                goto fail_mutex;


        if (pthread_condattr_init(&cattr))
                goto fail_condattr;
#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif

        if (pthread_cond_init(&req->cond, &cattr))
                goto fail_cond_init;

        pthread_condattr_destroy(&cattr);

        pthread_rwlock_wrlock(&dht->lock);

        list_add(&req->next, &dht->requests);

        pthread_rwlock_unlock(&dht->lock);

        return;

 fail_cond_init:
        pthread_condattr_destroy(&cattr);
 fail_condattr:
        pthread_mutex_destroy(&req->lock);
 fail_mutex:
        free(req->key);
 fail_dup_key:
        free(req);
 fail_malloc:
        return;
}

static void cancel_req_destroy(void * o)
{
        struct kad_req * req = (struct kad_req *) o;

        pthread_mutex_unlock(&req->lock);

        pthread_cond_destroy(&req->cond);
        pthread_mutex_destroy(&req->lock);

        if (req->key != NULL)
                free(req->key);

        free(req);
}

static void kad_req_destroy(struct kad_req * req)
{
        assert(req);

        pthread_mutex_lock(&req->lock);

        switch (req->state) {
        case REQ_DESTROY:
                pthread_mutex_unlock(&req->lock);
                return;
        case REQ_PENDING:
                req->state = REQ_DESTROY;
                pthread_cond_signal(&req->cond);
                break;
        case REQ_INIT:
        case REQ_DONE:
                req->state = REQ_NULL;
                break;
        case REQ_RESPONSE:
        case REQ_NULL:
        default:
                break;
        }

        pthread_cleanup_push(cancel_req_destroy, req);

        while (req->state != REQ_NULL && req->state != REQ_DONE)
                pthread_cond_wait(&req->cond, &req->lock);

        pthread_cleanup_pop(true);
}

static int kad_req_wait(struct kad_req * req,
                        time_t           t)
{
        struct timespec timeo = {t, 0};
        struct timespec abs;
        int ret = 0;

        assert(req);

        clock_gettime(PTHREAD_COND_CLOCK, &abs);

        ts_add(&abs, &timeo, &abs);

        pthread_mutex_lock(&req->lock);

        req->state = REQ_PENDING;

        pthread_cleanup_push(__cleanup_mutex_unlock, &req->lock);

        while (req->state == REQ_PENDING && ret != -ETIMEDOUT)
                ret = -pthread_cond_timedwait(&req->cond, &req->lock, &abs);

        switch(req->state) {
        case REQ_DESTROY:
                ret = -1;
                req->state = REQ_NULL;
                pthread_cond_signal(&req->cond);
                break;
        case REQ_PENDING: /* ETIMEDOUT */
        case REQ_RESPONSE:
                req->state = REQ_DONE;
                pthread_cond_broadcast(&req->cond);
                break;
        default:
                break;
        }

        pthread_cleanup_pop(true);

        return ret;
}

static void kad_req_respond(struct kad_req * req)
{
        pthread_mutex_lock(&req->lock);

        req->state = REQ_RESPONSE;
        pthread_cond_signal(&req->cond);

        pthread_mutex_unlock(&req->lock);
}

static struct contact * contact_create(const uint8_t * id,
                                       size_t          len,
                                       uint64_t        addr)
{
        struct contact * c;
        struct timespec  t;

        c = malloc(sizeof(*c));
        if (c == NULL)
                return NULL;

        list_head_init(&c->next);

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        c->addr   = addr;
        c->fails  = 0;
        c->t_seen = t.tv_sec;
        c->id     = dht_dup_key(id, len);
        if (c->id == NULL) {
                free(c);
                return NULL;
        }

        return c;
}

static void contact_destroy(struct contact * c)
{
        if (c != NULL)
                free(c->id);

        free(c);
}

static struct bucket * iter_bucket(struct bucket * b,
                                   const uint8_t * id)
{
        uint8_t byte;
        uint8_t mask;

        assert(b);

        if (b->children[0] == NULL)
                return b;

        byte = id[(b->depth * KAD_BETA) / CHAR_BIT];

        mask = ((1L << KAD_BETA) - 1) & 0xFF;

        byte >>= (CHAR_BIT - KAD_BETA) -
                (((b->depth) * KAD_BETA) & (CHAR_BIT - 1));

        return iter_bucket(b->children[(byte & mask)], id);
}

static struct bucket * dht_get_bucket(struct dht *    dht,
                                      const uint8_t * id)
{
        assert(dht->buckets);

        return iter_bucket(dht->buckets, id);
}

/*
 * If someone builds a network where the n (n > k) closest nodes all
 * have IDs starting with the same 64 bits: by all means, change this.
 */
static uint64_t dist(const uint8_t * src,
                     const uint8_t * dst)
{
        return betoh64(*((uint64_t *) src) ^ *((uint64_t *) dst));
}

static size_t list_add_sorted(struct list_head * l,
                              struct contact *   c,
                              const uint8_t *    key)
{
        struct list_head * p;

        assert(l);
        assert(c);
        assert(key);
        assert(c->id);

        list_for_each(p, l) {
                struct contact * e = list_entry(p, struct contact, next);
                if (dist(c->id, key) > dist(e->id, key))
                        break;
        }

        list_add_tail(&c->next, p);

        return 1;
}

static size_t dht_contact_list(struct dht *       dht,
                               struct list_head * l,
                               const uint8_t *    key)
{
        struct list_head * p;
        struct bucket *    b;
        size_t             len = 0;
        size_t             i;
        struct timespec    t;

        assert(l);
        assert(dht);
        assert(key);
        assert(list_is_empty(l));

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        b = dht_get_bucket(dht, key);
        if (b == NULL)
                return 0;

        b->t_refr = t.tv_sec + KAD_T_REFR;

        if (b->n_contacts == dht->k || b->parent == NULL) {
                list_for_each(p, &b->contacts) {
                        struct contact * c;
                        c = list_entry(p, struct contact, next);
                        c = contact_create(c->id, dht->b, c->addr);
                        if (list_add_sorted(l, c, key) == 1)
                                if (++len == dht->k)
                                        break;
                }
        } else {
                struct bucket * d = b->parent;
                for (i = 0; i < (1L << KAD_BETA) && len < dht->k; ++i) {
                        list_for_each(p, &d->children[i]->contacts) {
                                struct contact * c;
                                c = list_entry(p, struct contact, next);
                                c = contact_create(c->id, dht->b, c->addr);
                                if (c == NULL)
                                        continue;
                                if (list_add_sorted(l, c, key) == 1)
                                        if (++len == dht->k)
                                                break;
                        }
                }
        }

        assert(len == dht->k || b->parent == NULL);

        return len;
}

static struct lookup * lookup_create(struct dht *    dht,
                                     const uint8_t * id)
{
        struct lookup *    lu;
        pthread_condattr_t cattr;

        assert(dht);
        assert(id);

        lu = malloc(sizeof(*lu));
        if (lu == NULL)
                goto fail_malloc;

        list_head_init(&lu->contacts);
        list_head_init(&lu->cookies);

        lu->state   = LU_INIT;
        lu->addrs   = NULL;
        lu->n_addrs = 0;
        lu->key     = dht_dup_key(id, dht->b);
        if (lu->key == NULL)
                goto fail_id;

        if (pthread_mutex_init(&lu->lock, NULL))
                goto fail_mutex;

        pthread_condattr_init(&cattr);
#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif

        if (pthread_cond_init(&lu->cond, &cattr))
                goto fail_cond;

        pthread_condattr_destroy(&cattr);

        pthread_rwlock_wrlock(&dht->lock);

        list_add(&lu->next, &dht->lookups);

        lu->n_contacts = dht_contact_list(dht, &lu->contacts, id);

        pthread_rwlock_unlock(&dht->lock);

        return lu;

 fail_cond:
        pthread_condattr_destroy(&cattr);
        pthread_mutex_destroy(&lu->lock);
 fail_mutex:
        free(lu->key);
 fail_id:
        free(lu);
 fail_malloc:
        return NULL;
}

static void cancel_lookup_destroy(void * o)
{
        struct lookup *    lu;
        struct list_head * p;
        struct list_head * h;

        lu = (struct lookup *) o;

        if (lu->key != NULL)
                free(lu->key);
        if (lu->addrs != NULL)
                free(lu->addrs);

        list_for_each_safe(p, h, &lu->contacts) {
                struct contact * c = list_entry(p, struct contact, next);
                list_del(&c->next);
                contact_destroy(c);
        }

        list_for_each_safe(p, h, &lu->cookies) {
                struct cookie_el * c = list_entry(p, struct cookie_el, next);
                list_del(&c->next);
                free(c);
        }

        pthread_mutex_unlock(&lu->lock);

        pthread_mutex_destroy(&lu->lock);

        free(lu);
}

static void lookup_destroy(struct lookup * lu)
{
        assert(lu);

        pthread_mutex_lock(&lu->lock);

        switch (lu->state) {
        case LU_DESTROY:
                pthread_mutex_unlock(&lu->lock);
                return;
        case LU_PENDING:
                lu->state = LU_DESTROY;
                pthread_cond_broadcast(&lu->cond);
                break;
        case LU_INIT:
        case LU_UPDATE:
        case LU_COMPLETE:
                lu->state = LU_NULL;
                break;
        case LU_NULL:
        default:
                break;
        }

        pthread_cleanup_push(cancel_lookup_destroy, lu);

        while (lu->state != LU_NULL)
                pthread_cond_wait(&lu->cond, &lu->lock);

        pthread_cleanup_pop(true);
}

static void lookup_update(struct dht *    dht,
                          struct lookup * lu,
                          kad_msg_t *     msg)
{
        struct list_head * p = NULL;
        struct list_head * h;
        struct contact *   c = NULL;
        size_t             n;
        size_t             pos = 0;
        bool               mod = false;

        assert(lu);
        assert(msg);

        if (dht_get_state(dht) != DHT_RUNNING)
                return;

        pthread_mutex_lock(&lu->lock);

        list_for_each_safe(p, h, &lu->cookies) {
                struct cookie_el * e = list_entry(p, struct cookie_el, next);
                if (e->cookie == msg->cookie) {
                        list_del(&e->next);
                        free(e);
                        break;
                }
        }

        if (lu->state == LU_COMPLETE) {
                pthread_mutex_unlock(&lu->lock);
                return;
        }

        if (msg->n_addrs > 0) {
                if (lu->addrs == NULL) {
                        lu->addrs = malloc(sizeof(*lu->addrs) * msg->n_addrs);
                        for (n = 0; n < msg->n_addrs; ++n)
                                lu->addrs[n] = msg->addrs[n];
                        lu->n_addrs = msg->n_addrs;
                }

                lu->state = LU_COMPLETE;
                pthread_cond_broadcast(&lu->cond);
                pthread_mutex_unlock(&lu->lock);
                return;
        }

        pthread_cleanup_push(__cleanup_mutex_unlock, &lu->lock);

        while (lu->state == LU_INIT) {
                pthread_rwlock_unlock(&dht->lock);
                pthread_cond_wait(&lu->cond, &lu->lock);
                pthread_rwlock_rdlock(&dht->lock);
        }

        pthread_cleanup_pop(false);

        for (n = 0; n < msg->n_contacts; ++n) {
                c = contact_create(msg->contacts[n]->id.data,
                                   dht->b, msg->contacts[n]->addr);
                if (c == NULL)
                        continue;

                pos = 0;

                list_for_each(p, &lu->contacts) {
                        struct contact * e;
                        e = list_entry(p, struct contact, next);
                        if (!memcmp(e->id, c->id, dht->b)) {
                                contact_destroy(c);
                                c = NULL;
                                break;
                        }

                        if (dist(c->id, lu->key) > dist(e->id, lu->key))
                                break;

                        pos++;
                }

                if (c == NULL)
                        continue;

                if (lu->n_contacts < dht->k) {
                        list_add_tail(&c->next, p);
                        ++lu->n_contacts;
                        mod = true;
                } else if (pos == dht->k) {
                        contact_destroy(c);
                } else {
                        struct contact * d;
                        list_add_tail(&c->next, p);
                        d = list_last_entry(&lu->contacts,
                                            struct contact, next);
                        list_del(&d->next);
                        assert(lu->contacts.prv != &d->next);
                        contact_destroy(d);
                        mod = true;
                }
        }

        if (list_is_empty(&lu->cookies) && !mod)
                lu->state = LU_COMPLETE;
        else
                lu->state = LU_UPDATE;

        pthread_cond_broadcast(&lu->cond);
        pthread_mutex_unlock(&lu->lock);
        return;
}

static ssize_t lookup_get_addrs(struct lookup * lu,
                                uint64_t *      addrs)
{
        ssize_t n;

        assert(lu);

        pthread_mutex_lock(&lu->lock);

        for (n = 0; (size_t) n < lu->n_addrs; ++n)
                addrs[n] = lu->addrs[n];

        assert((size_t) n == lu->n_addrs);

        pthread_mutex_unlock(&lu->lock);

        return n;
}

static ssize_t lookup_contact_addrs(struct lookup * lu,
                                    uint64_t *      addrs)
{
        struct list_head * p;
        ssize_t            n = 0;

        assert(lu);
        assert(addrs);

        pthread_mutex_lock(&lu->lock);

        list_for_each(p, &lu->contacts) {
                struct contact * c = list_entry(p, struct contact, next);
                addrs[n] = c->addr;
                n++;
        }

        pthread_mutex_unlock(&lu->lock);

        return n;
}

static void lookup_new_addrs(struct lookup * lu,
                             uint64_t *      addrs)
{
        struct list_head * p;
        size_t             n = 0;

        assert(lu);
        assert(addrs);

        pthread_mutex_lock(&lu->lock);

        /* Uses fails to check if the contact has been contacted. */
        list_for_each(p, &lu->contacts) {
                struct contact * c = list_entry(p, struct contact, next);
                if (c->fails == 0) {
                        c->fails = 1;
                        addrs[n] = c->addr;
                        n++;
                }

                if (n == KAD_ALPHA)
                        break;
        }

        assert(n <= KAD_ALPHA);

        addrs[n] = 0;

        pthread_mutex_unlock(&lu->lock);
}

static void lookup_set_state(struct lookup *   lu,
                             enum lookup_state state)
{
        pthread_mutex_lock(&lu->lock);

        lu->state = state;
        pthread_cond_broadcast(&lu->cond);

        pthread_mutex_unlock(&lu->lock);
}

static void cancel_lookup_wait(void * o)
{
        struct lookup * lu = (struct lookup *) o;
        lu->state = LU_NULL;
        pthread_mutex_unlock(&lu->lock);
        lookup_destroy(lu);
}

static enum lookup_state lookup_wait(struct lookup * lu)
{
        struct timespec   timeo = {KAD_T_RESP, 0};
        struct timespec   abs;
        enum lookup_state state;
        int               ret = 0;

        clock_gettime(PTHREAD_COND_CLOCK, &abs);

        ts_add(&abs, &timeo, &abs);

        pthread_mutex_lock(&lu->lock);

        if (lu->state == LU_INIT || lu->state == LU_UPDATE)
                lu->state = LU_PENDING;

        pthread_cleanup_push(cancel_lookup_wait, lu);

        while (lu->state == LU_PENDING && ret != -ETIMEDOUT)
                ret = -pthread_cond_timedwait(&lu->cond, &lu->lock, &abs);

        pthread_cleanup_pop(false);

        if (ret == -ETIMEDOUT)
                lu->state = LU_COMPLETE;

        state = lu->state;

        pthread_mutex_unlock(&lu->lock);

        return state;
}

static struct kad_req * dht_find_request(struct dht * dht,
                                         kad_msg_t *  msg)
{
        struct list_head * p;

        assert(dht);
        assert(msg);

        list_for_each(p, &dht->requests) {
                struct kad_req * r = list_entry(p, struct kad_req, next);
                if (r->cookie == msg->cookie)
                        return r;
        }

        return NULL;
}

static struct lookup * dht_find_lookup(struct dht *    dht,
                                       uint32_t        cookie)
{
        struct list_head * p;
        struct list_head * p2;
        struct list_head * h2;

        assert(dht);
        assert(cookie > 0);

        list_for_each(p, &dht->lookups) {
                struct lookup * l = list_entry(p, struct lookup, next);
                pthread_mutex_lock(&l->lock);
                list_for_each_safe(p2, h2, &l->cookies) {
                        struct cookie_el * e;
                        e = list_entry(p2, struct cookie_el, next);
                        if (e->cookie == cookie) {
                                list_del(&e->next);
                                free(e);
                                pthread_mutex_unlock(&l->lock);
                                return l;
                        }
                }
                pthread_mutex_unlock(&l->lock);
        }

        return NULL;
}

static struct val * val_create(uint64_t addr,
                               time_t   exp)
{
        struct val *    v;
        struct timespec t;

        v = malloc(sizeof(*v));
        if (v == NULL)
                return NULL;

        list_head_init(&v->next);
        v->addr = addr;

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        v->t_exp = t.tv_sec + exp;
        v->t_rep = t.tv_sec + KAD_T_REPL;

        return v;
}

static void val_destroy(struct val * v)
{
        assert(v);

        free(v);
}

static struct ref_entry * ref_entry_create(struct dht *    dht,
                                           const uint8_t * key)
{
        struct ref_entry * e;
        struct timespec    t;

        assert(dht);
        assert(key);

        e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        e->key = dht_dup_key(key, dht->b);
        if (e->key == NULL) {
                free(e);
                return NULL;
        }

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        e->t_rep = t.tv_sec + dht->t_repub;

        return e;
}

static void ref_entry_destroy(struct ref_entry * e)
{
        free(e->key);
        free(e);
}

static struct dht_entry * dht_entry_create(struct dht *    dht,
                                           const uint8_t * key)
{
        struct dht_entry * e;

        assert(dht);
        assert(key);

        e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        list_head_init(&e->next);
        list_head_init(&e->vals);

        e->n_vals = 0;

        e->key = dht_dup_key(key, dht->b);
        if (e->key == NULL) {
                free(e);
                return NULL;
        }

        return e;
}

static void dht_entry_destroy(struct dht_entry * e)
{
        struct list_head * p;
        struct list_head * h;

        assert(e);

        list_for_each_safe(p, h, &e->vals) {
                struct val * v = list_entry(p, struct val, next);
                list_del(&v->next);
                val_destroy(v);
        }

        free(e->key);

        free(e);
}

static int dht_entry_add_addr(struct dht_entry * e,
                              uint64_t           addr,
                              time_t             exp)
{
        struct list_head * p;
        struct val * val;
        struct timespec t;

        clock_gettime(CLOCK_REALTIME_COARSE, &t);

        list_for_each(p, &e->vals) {
                struct val * v = list_entry(p, struct val, next);
                if (v->addr == addr) {
                        if (v->t_exp < t.tv_sec + exp) {
                                v->t_exp = t.tv_sec + exp;
                                v->t_rep = t.tv_sec + KAD_T_REPL;
                        }

                        return 0;
                }
        }

        val = val_create(addr, exp);
        if (val == NULL)
                return -ENOMEM;

        list_add(&val->next, &e->vals);
        ++e->n_vals;

        return 0;
}


static void dht_entry_del_addr(struct dht_entry * e,
                               uint64_t           addr)
{
        struct list_head * p;
        struct list_head * h;

        assert(e);

        list_for_each_safe(p, h, &e->vals) {
                struct val * v = list_entry(p, struct val, next);
                if (v->addr == addr) {
                        list_del(&v->next);
                        val_destroy(v);
                        --e->n_vals;
                }
        }

        if (e->n_vals == 0) {
                list_del(&e->next);
                dht_entry_destroy(e);
        }
}

static uint64_t dht_entry_get_addr(struct dht *       dht,
                                   struct dht_entry * e)
{
        struct list_head * p;

        assert(e);
        assert(!list_is_empty(&e->vals));

        list_for_each(p, &e->vals) {
                struct val * v = list_entry(p, struct val, next);
                if (v->addr != dht->addr)
                        return v->addr;
        }

        return 0;
}

/* Forward declaration. */
static struct lookup * kad_lookup(struct dht *    dht,
                                  const uint8_t * key,
                                  enum kad_code   code);


/* Build a refresh list. */
static void bucket_refresh(struct dht *       dht,
                           struct bucket *    b,
                           time_t             t,
                           struct list_head * r)
{
        size_t i;

        if (*b->children != NULL)
                for (i = 0; i < (1L << KAD_BETA); ++i)
                        bucket_refresh(dht, b->children[i], t, r);

        if (b->n_contacts == 0)
                return;

        if (t > b->t_refr) {
                struct contact * c;
                struct contact * d;
                c = list_first_entry(&b->contacts, struct contact, next);
                d = contact_create(c->id, dht->b, c->addr);
                if (d != NULL)
                        list_add(&d->next, r);
                return;
        }
}


static struct bucket * bucket_create(void)
{
        struct bucket * b;
        struct timespec t;
        size_t          i;

        b = malloc(sizeof(*b));
        if (b == NULL)
                return NULL;

        list_head_init(&b->contacts);
        b->n_contacts = 0;

        list_head_init(&b->alts);
        b->n_alts = 0;

        clock_gettime(CLOCK_REALTIME_COARSE, &t);
        b->t_refr = t.tv_sec + KAD_T_REFR;

        for (i = 0; i < (1L << KAD_BETA); ++i)
                b->children[i]  = NULL;

        b->parent = NULL;
        b->depth = 0;

        return b;
}

static void bucket_destroy(struct bucket * b)
{
        struct list_head * p;
        struct list_head * h;
        size_t             i;

        assert(b);

        for (i = 0; i < (1L << KAD_BETA); ++i)
                if (b->children[i] != NULL)
                        bucket_destroy(b->children[i]);

        list_for_each_safe(p, h, &b->contacts) {
                struct contact * c = list_entry(p, struct contact, next);
                list_del(&c->next);
                contact_destroy(c);
                --b->n_contacts;
        }

        list_for_each_safe(p, h, &b->alts) {
                struct contact * c = list_entry(p, struct contact, next);
                list_del(&c->next);
                contact_destroy(c);
                --b->n_contacts;
        }

        free(b);
}

static bool bucket_has_id(struct bucket * b,
                          const uint8_t * id)
{
        uint8_t mask;
        uint8_t byte;

        if (b->depth == 0)
                return true;

        byte = id[(b->depth * KAD_BETA) / CHAR_BIT];

        mask = ((1L << KAD_BETA) - 1) & 0xFF;

        byte >>= (CHAR_BIT - KAD_BETA) -
                (((b->depth - 1) * KAD_BETA) & (CHAR_BIT - 1));

        return ((byte & mask) == b->mask);
}

static int split_bucket(struct bucket * b)
{
        struct list_head * p;
        struct list_head * h;
        uint8_t mask = 0;
        size_t i;
        size_t c;

        assert(b);
        assert(b->n_alts == 0);
        assert(b->n_contacts);
        assert(b->children[0] == NULL);

        c = b->n_contacts;

        for (i = 0; i < (1L << KAD_BETA); ++i) {
                b->children[i] = bucket_create();
                if (b->children[i] == NULL) {
                        size_t j;
                        for (j = 0; j < i; ++j)
                                bucket_destroy(b->children[j]);
                        return -1;
                }

                b->children[i]->depth  = b->depth + 1;
                b->children[i]->mask   = mask;
                b->children[i]->parent = b;

                list_for_each_safe(p, h, &b->contacts) {
                        struct contact * c;
                        c = list_entry(p, struct contact, next);
                        if (bucket_has_id(b->children[i], c->id)) {
                                list_del(&c->next);
                                --b->n_contacts;
                                list_add(&c->next, &b->children[i]->contacts);
                                ++b->children[i]->n_contacts;
                        }
                }

                mask++;
        }

        for (i = 0; i < (1L << KAD_BETA); ++i)
                if (b->children[i]->n_contacts == c)
                        split_bucket(b->children[i]);

        return 0;
}

/* Locked externally to mandate update as (final) part of join transaction. */
static int dht_update_bucket(struct dht *    dht,
                             const uint8_t * id,
                             uint64_t        addr)
{
        struct list_head * p;
        struct list_head * h;
        struct bucket *    b;
        struct contact *   c;

        assert(dht);

        b = dht_get_bucket(dht, id);
        if (b == NULL)
                return -1;

        c = contact_create(id, dht->b, addr);
        if (c == NULL)
                return -1;

        list_for_each_safe(p, h, &b->contacts) {
                struct contact * d = list_entry(p, struct contact, next);
                if (d->addr == addr) {
                        list_del(&d->next);
                        contact_destroy(d);
                        --b->n_contacts;
                }
        }

        if (b->n_contacts == dht->k) {
                if (bucket_has_id(b, dht->id)) {
                        list_add_tail(&c->next, &b->contacts);
                        ++b->n_contacts;
                        if (split_bucket(b)) {
                                list_del(&c->next);
                                contact_destroy(c);
                                --b->n_contacts;
                        }
                } else if (b->n_alts == dht->k) {
                        struct contact * d;
                        d = list_first_entry(&b->alts, struct contact, next);
                        list_del(&d->next);
                        contact_destroy(d);
                        list_add_tail(&c->next, &b->alts);
                } else {
                        list_add_tail(&c->next, &b->alts);
                        ++b->n_alts;
                }
        } else {
                list_add_tail(&c->next, &b->contacts);
                ++b->n_contacts;
        }

        return 0;
}

static int send_msg(struct dht * dht,
                    kad_msg_t *  msg,
                    uint64_t     addr)
{
#ifndef __DHT_TEST__
        struct shm_du_buff * sdb;
        size_t               len;
#endif
        int                  retr = 0;

        if (msg->code == KAD_RESPONSE)
                retr = KAD_RESP_RETR;

        pthread_rwlock_wrlock(&dht->lock);

        if (dht->id != NULL) {
                msg->has_s_id = true;
                msg->s_id.data = dht->id;
                msg->s_id.len  = dht->b;
        }

        msg->s_addr = dht->addr;

        if (msg->code < KAD_STORE) {
                msg->cookie = bmp_allocate(dht->cookies);
                if (!bmp_is_id_valid(dht->cookies, msg->cookie)) {
                        pthread_rwlock_unlock(&dht->lock);
                        goto fail_bmp_alloc;
                }
        }

        pthread_rwlock_unlock(&dht->lock);

#ifndef __DHT_TEST__
        len = kad_msg__get_packed_size(msg);
        if (len == 0)
                goto fail_msg;

        while (true) {
                if (ipcp_sdb_reserve(&sdb, len))
                        goto fail_msg;

                kad_msg__pack(msg, shm_du_buff_head(sdb));

                if (dt_write_packet(addr, QOS_CUBE_BE, dht->eid, sdb) == 0)
                        break;

                ipcp_sdb_release(sdb);

                sleep(1);

                if (--retr < 0)
                        goto fail_msg;
        }

#else
        (void) addr;
        (void) retr;
#endif /* __DHT_TEST__ */

        if (msg->code < KAD_STORE && dht_get_state(dht) != DHT_SHUTDOWN)
                kad_req_create(dht, msg, addr);

        return msg->cookie;
#ifndef __DHT_TEST__
 fail_msg:
        pthread_rwlock_wrlock(&dht->lock);
        bmp_release(dht->cookies, msg->cookie);
        pthread_rwlock_unlock(&dht->lock);
#endif /* !__DHT_TEST__ */
 fail_bmp_alloc:
        return -1;
}

static struct dht_entry * dht_find_entry(struct dht *    dht,
                                         const uint8_t * key)
{
        struct list_head * p;

        list_for_each(p, &dht->entries) {
                struct dht_entry * e = list_entry(p, struct dht_entry, next);
                if (!memcmp(key, e->key, dht->b))
                        return e;
        }

        return NULL;
}

static int kad_add(struct dht *              dht,
                   const kad_contact_msg_t * contacts,
                   ssize_t                   n,
                   time_t                    exp)
{
        struct dht_entry * e;

        pthread_rwlock_wrlock(&dht->lock);

        while (n-- > 0) {
                if (contacts[n].id.len != dht->b)
                        log_warn("Bad key length in contact data.");

                e = dht_find_entry(dht, contacts[n].id.data);
                if (e != NULL) {
                        if (dht_entry_add_addr(e, contacts[n].addr, exp))
                                goto fail;
                } else {
                        e = dht_entry_create(dht, contacts[n].id.data);
                        if (e == NULL)
                                goto fail;

                        if (dht_entry_add_addr(e, contacts[n].addr, exp)) {
                                dht_entry_destroy(e);
                                goto fail;
                        }

                        list_add(&e->next, &dht->entries);
                }
        }

        pthread_rwlock_unlock(&dht->lock);
        return 0;

 fail:
        pthread_rwlock_unlock(&dht->lock);
        return -ENOMEM;
}

static int wait_resp(struct dht * dht,
                     kad_msg_t *  msg,
                     time_t       timeo)
{
        struct kad_req * req;

        assert(dht);
        assert(msg);

        pthread_rwlock_rdlock(&dht->lock);

        req = dht_find_request(dht, msg);
        if (req == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return -EPERM;
        }

        pthread_rwlock_unlock(&dht->lock);

        return kad_req_wait(req, timeo);
}

static int kad_store(struct dht *    dht,
                     const uint8_t * key,
                     uint64_t        addr,
                     uint64_t        r_addr,
                     time_t          ttl)
{
        kad_msg_t msg = KAD_MSG__INIT;
        kad_contact_msg_t cmsg = KAD_CONTACT_MSG__INIT;
        kad_contact_msg_t * cmsgp[1];

        cmsg.id.data = (uint8_t *) key;
        cmsg.addr    = addr;

        pthread_rwlock_rdlock(&dht->lock);

        cmsg.id.len  = dht->b;

        pthread_rwlock_unlock(&dht->lock);

        cmsgp[0] = &cmsg;

        msg.code         = KAD_STORE;
        msg.has_t_expire = true;
        msg.t_expire     = ttl;
        msg.n_contacts   = 1;
        msg.contacts     = cmsgp;

        if (send_msg(dht, &msg, r_addr) < 0)
                return -1;

        return 0;
}

static ssize_t kad_find(struct dht *     dht,
                        struct lookup *  lu,
                        const uint64_t * addrs,
                        enum kad_code    code)
{
        kad_msg_t msg  = KAD_MSG__INIT;
        ssize_t   sent = 0;

        assert(dht);
        assert(lu->key);

        msg.code = code;

        msg.has_key       = true;
        msg.key.data      = (uint8_t *) lu->key;
        msg.key.len       = dht->b;

        while (*addrs != 0) {
                struct cookie_el * c;
                int ret;

                if (*addrs == dht->addr) {
                        ++addrs;
                        continue;
                }

                ret = send_msg(dht, &msg, *addrs);
                if (ret < 0)
                        break;

                c = malloc(sizeof(*c));
                if (c == NULL)
                        break;

                c->cookie = (uint32_t) ret;

                pthread_mutex_lock(&lu->lock);

                list_add_tail(&c->next, &lu->cookies);

                pthread_mutex_unlock(&lu->lock);

                ++sent;
                ++addrs;
        }

        return sent;
}

static void lookup_detach(struct dht *    dht,
                          struct lookup * lu)
{
        pthread_rwlock_wrlock(&dht->lock);

        list_del(&lu->next);

        pthread_rwlock_unlock(&dht->lock);
}

static struct lookup * kad_lookup(struct dht *    dht,
                                  const uint8_t * id,
                                  enum kad_code   code)
{
        uint64_t          addrs[KAD_ALPHA + 1];
        enum lookup_state state;
        struct lookup *   lu;

        lu = lookup_create(dht, id);
        if (lu == NULL)
                return NULL;

        lookup_new_addrs(lu, addrs);

        if (addrs[0] == 0) {
                lookup_detach(dht, lu);
                lookup_destroy(lu);
                return NULL;
        }

        if (kad_find(dht, lu, addrs, code) == 0) {
                lookup_detach(dht, lu);
                return lu;
        }

        while ((state = lookup_wait(lu)) != LU_COMPLETE) {
                switch (state) {
                case LU_UPDATE:
                        lookup_new_addrs(lu, addrs);
                        if (addrs[0] == 0)
                                break;

                        kad_find(dht, lu, addrs, code);
                        break;
                case LU_DESTROY:
                        lookup_detach(dht, lu);
                        lookup_set_state(lu, LU_NULL);
                        return NULL;
                default:
                        break;
                }
        }

        assert(state == LU_COMPLETE);

        lookup_detach(dht, lu);

        return lu;
}

static void kad_publish(struct dht *    dht,
                        const uint8_t * key,
                        uint64_t        addr,
                        time_t          exp)
{
        struct lookup * lu;
        uint64_t      * addrs;
        ssize_t         n;
        size_t          k;
        time_t          t_expire;


        assert(dht);
        assert(key);

        pthread_rwlock_rdlock(&dht->lock);

        k        = dht->k;
        t_expire = dht->t_expire;

        pthread_rwlock_unlock(&dht->lock);

        addrs = malloc(k * sizeof(*addrs));
        if (addrs == NULL)
                return;

        lu = kad_lookup(dht, key, KAD_FIND_NODE);
        if (lu == NULL) {
                free(addrs);
                return;
        }

        n = lookup_contact_addrs(lu, addrs);

        while (n-- > 0) {
                if (addrs[n] == dht->addr) {
                        kad_contact_msg_t msg = KAD_CONTACT_MSG__INIT;
                        msg.id.data = (uint8_t *) key;
                        msg.id.len  = dht->b;
                        msg.addr    = addr;
                        kad_add(dht, &msg, 1, exp);
                } else {
                        if (kad_store(dht, key, addr, addrs[n], t_expire))
                                log_warn("Failed to send store message.");
                }
        }

        lookup_destroy(lu);

        free(addrs);
}

static int kad_join(struct dht * dht,
                    uint64_t     addr)
{
        kad_msg_t       msg = KAD_MSG__INIT;

        msg.code = KAD_JOIN;

        msg.has_alpha       = true;
        msg.has_b           = true;
        msg.has_k           = true;
        msg.has_t_refresh   = true;
        msg.has_t_replicate = true;
        msg.alpha           = KAD_ALPHA;
        msg.k               = KAD_K;
        msg.t_refresh       = KAD_T_REFR;
        msg.t_replicate     = KAD_T_REPL;

        pthread_rwlock_rdlock(&dht->lock);

        msg.b               = dht->b;

        pthread_rwlock_unlock(&dht->lock);

        if (send_msg(dht, &msg, addr) < 0)
                return -1;

        if (wait_resp(dht, &msg, KAD_T_JOIN) < 0)
                return -1;

        dht->id = create_id(dht->b);
        if (dht->id == NULL)
                return -1;

        pthread_rwlock_wrlock(&dht->lock);

        dht_update_bucket(dht, dht->id, dht->addr);

        pthread_rwlock_unlock(&dht->lock);

        return 0;
}

static void dht_dead_peer(struct dht * dht,
                          uint8_t *    key,
                          uint64_t     addr)
{
        struct list_head * p;
        struct list_head * h;
        struct bucket *    b;

        b = dht_get_bucket(dht, key);

        list_for_each_safe(p, h, &b->contacts) {
                struct contact * c = list_entry(p, struct contact, next);
                if (b->n_contacts + b->n_alts <= dht->k) {
                        ++c->fails;
                        return;
                }

                if (c->addr == addr) {
                        list_del(&c->next);
                        contact_destroy(c);
                        --b->n_contacts;
                        break;
                }
        }

        while (b->n_contacts < dht->k && b->n_alts > 0) {
                struct contact * c;
                c = list_first_entry(&b->alts, struct contact, next);
                list_del(&c->next);
                --b->n_alts;
                list_add(&c->next, &b->contacts);
                ++b->n_contacts;
        }
}

static int dht_del(struct dht *    dht,
                   const uint8_t * key,
                   uint64_t        addr)
{
        struct dht_entry * e;

        e = dht_find_entry(dht, key);
        if (e == NULL) {
                return -EPERM;
        }

        dht_entry_del_addr(e, addr);

        return 0;
}

static buffer_t dht_retrieve(struct dht *    dht,
                             const uint8_t * key)
{
        struct dht_entry * e;
        struct list_head * p;
        buffer_t           buf;
        uint64_t *         pos;
        size_t             addrs = 0;

        pthread_rwlock_rdlock(&dht->lock);

        e = dht_find_entry(dht, key);
        if (e == NULL)
                goto fail;

        buf.len = MIN(DHT_RETR_ADDR, e->n_vals);
        if (buf.len == 0)
                goto fail;

        pos = malloc(sizeof(dht->addr) * buf.len);
        if (pos == NULL)
                goto fail;

        buf.data = (uint8_t *) pos;

        list_for_each(p, &e->vals) {
                struct val * v = list_entry(p, struct val, next);
                *pos++ = v->addr;
                if (++addrs >= buf.len)
                        break;
        }

        pthread_rwlock_unlock(&dht->lock);

        return buf;

 fail:
        pthread_rwlock_unlock(&dht->lock);
        buf.len  = 0;
        buf.data = NULL;
        return buf;
}

static ssize_t dht_get_contacts(struct dht *          dht,
                                const uint8_t *       key,
                                kad_contact_msg_t *** msgs)
{
        struct list_head   l;
        struct list_head * p;
        struct list_head * h;
        size_t             len;
        size_t             i = 0;

        list_head_init(&l);

        pthread_rwlock_wrlock(&dht->lock);

        len = dht_contact_list(dht, &l, key);
        if (len == 0) {
                pthread_rwlock_unlock(&dht->lock);
                *msgs = NULL;
                return 0;
        }

        *msgs = malloc(len * sizeof(**msgs));
        if (*msgs == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return 0;
        }

        list_for_each_safe(p, h, &l) {
                struct contact * c = list_entry(p, struct contact, next);
                (*msgs)[i] = malloc(sizeof(***msgs));
                if ((*msgs)[i] == NULL) {
                        pthread_rwlock_unlock(&dht->lock);
                        while (i > 0)
                                free(*msgs[--i]);
                        free(*msgs);
                        *msgs = NULL;
                        return 0;
                }

                kad_contact_msg__init((*msgs)[i]);

                (*msgs)[i]->id.data = c->id;
                (*msgs)[i]->id.len  = dht->b;
                (*msgs)[i++]->addr  = c->addr;
                list_del(&c->next);
                free(c);
        }

        pthread_rwlock_unlock(&dht->lock);

        return i;
}

static time_t gcd(time_t a,
                  time_t b)
{
        if (a == 0)
                return b;

        return gcd(b % a, a);
}

static void * work(void * o)
{
        struct dht *       dht;
        struct timespec    now;
        struct list_head * p;
        struct list_head * h;
        struct list_head   reflist;
        time_t             intv;
        struct lookup *    lu;

        dht = (struct dht *) o;

        pthread_rwlock_rdlock(&dht->lock);

        intv = gcd(dht->t_expire, dht->t_repub);
        intv = gcd(intv, gcd(KAD_T_REPL, KAD_T_REFR)) / 2;

        pthread_rwlock_unlock(&dht->lock);

        list_head_init(&reflist);

        while (true) {
                clock_gettime(CLOCK_REALTIME_COARSE, &now);

                pthread_rwlock_wrlock(&dht->lock);

                /* Republish registered hashes. */
                list_for_each(p, &dht->refs) {
                        struct ref_entry * e;
                        uint8_t *          key;
                        uint64_t           addr;
                        time_t             t_expire;
                        e = list_entry(p, struct ref_entry, next);
                        if (now.tv_sec > e->t_rep) {
                                key = dht_dup_key(e->key, dht->b);
                                if (key == NULL)
                                        continue;
                                addr = dht->addr;
                                t_expire = dht->t_expire;
                                e->t_rep = now.tv_sec + dht->t_repub;

                                pthread_rwlock_unlock(&dht->lock);
                                kad_publish(dht, key, addr, t_expire);
                                pthread_rwlock_wrlock(&dht->lock);
                                free(key);
                        }
                }

                /* Remove stale entries and republish if necessary. */
                list_for_each_safe(p, h, &dht->entries) {
                        struct list_head * p1;
                        struct list_head * h1;
                        struct dht_entry * e;
                        uint8_t *          key;
                        time_t             t_expire;
                        e = list_entry (p, struct dht_entry, next);
                        list_for_each_safe(p1, h1, &e->vals) {
                                struct val * v;
                                uint64_t     addr;
                                v = list_entry(p1, struct val, next);
                                if (now.tv_sec > v->t_exp) {
                                        list_del(&v->next);
                                        val_destroy(v);
                                        continue;
                                }

                                if (now.tv_sec > v->t_rep) {
                                        key  = dht_dup_key(e->key, dht->b);
                                        addr = v->addr;
                                        t_expire = dht->t_expire = now.tv_sec;
                                        v->t_rep = now.tv_sec + dht->t_replic;
                                        pthread_rwlock_unlock(&dht->lock);
                                        kad_publish(dht, key, addr, t_expire);
                                        pthread_rwlock_wrlock(&dht->lock);
                                        free(key);
                                }
                        }
                }

                /* Check the requests list for unresponsive nodes. */
                list_for_each_safe(p, h, &dht->requests) {
                        struct kad_req * r;
                        r = list_entry(p, struct kad_req, next);
                        if (now.tv_sec > r->t_exp) {
                                list_del(&r->next);
                                bmp_release(dht->cookies, r->cookie);
                                dht_dead_peer(dht, r->key, r->addr);
                                kad_req_destroy(r);
                        }
                }

                /* Refresh unaccessed buckets. */
                bucket_refresh(dht, dht->buckets, now.tv_sec, &reflist);

                pthread_rwlock_unlock(&dht->lock);

                list_for_each_safe(p, h, &reflist) {
                        struct contact * c;
                        c = list_entry(p, struct contact, next);
                        lu = kad_lookup(dht, c->id, KAD_FIND_NODE);
                        if (lu != NULL)
                                lookup_destroy(lu);
                        list_del(&c->next);
                        contact_destroy(c);
                }

                sleep(intv);
        }

        return (void *) 0;
}

static int kad_handle_join_resp(struct dht *     dht,
                                struct kad_req * req,
                                kad_msg_t *      msg)
{
        assert(dht);
        assert(req);
        assert(msg);

        /* We might send version numbers later to warn of updates if needed. */
        if (!(msg->has_alpha && msg->has_b && msg->has_k && msg->has_t_expire &&
              msg->has_t_refresh && msg->has_t_replicate)) {
                log_warn("Join refused by remote.");
                return -1;
        }

        if (msg->b < sizeof(uint64_t)) {
                log_err("Hash sizes less than 8 bytes unsupported.");
                return -1;
        }

        pthread_rwlock_wrlock(&dht->lock);

        dht->buckets = bucket_create();
        if (dht->buckets == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return -1;
        }

        /* Likely corrupt packet. The member will refuse, we might here too. */
        if (msg->alpha != KAD_ALPHA || msg->k != KAD_K)
                log_warn("Different kademlia parameters detected.");

        if (msg->t_replicate != KAD_T_REPL)
                log_warn("Different kademlia replication time detected.");

        if (msg->t_refresh != KAD_T_REFR)
                log_warn("Different kademlia refresh time detected.");

        dht->k        = msg->k;
        dht->b        = msg->b;
        dht->t_expire = msg->t_expire;
        dht->t_repub  = MAX(1, dht->t_expire - 10);

        if (pthread_create(&dht->worker, NULL, work, dht)) {
                bucket_destroy(dht->buckets);
                pthread_rwlock_unlock(&dht->lock);
                return -1;
        }

        kad_req_respond(req);

        dht_update_bucket(dht, msg->s_id.data, msg->s_addr);

        pthread_rwlock_unlock(&dht->lock);

        log_dbg("Enrollment of DHT completed.");

        return 0;
}

static int kad_handle_find_resp(struct dht *     dht,
                                struct kad_req * req,
                                kad_msg_t *      msg)
{
        struct lookup * lu;

        assert(dht);
        assert(req);
        assert(msg);

        pthread_rwlock_rdlock(&dht->lock);

        lu = dht_find_lookup(dht, req->cookie);
        if (lu == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return -1;
        }

        lookup_update(dht, lu, msg);

        pthread_rwlock_unlock(&dht->lock);

        return 0;
}

static void kad_handle_response(struct dht * dht,
                                kad_msg_t *  msg)
{
        struct kad_req * req;

        assert(dht);
        assert(msg);

        pthread_rwlock_wrlock(&dht->lock);

        req = dht_find_request(dht, msg);
        if (req == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return;
        }

        bmp_release(dht->cookies, req->cookie);
        list_del(&req->next);

        pthread_rwlock_unlock(&dht->lock);

        switch(req->code) {
        case KAD_JOIN:
                if (kad_handle_join_resp(dht, req, msg))
                        log_err("Enrollment of DHT failed.");
                break;
        case KAD_FIND_VALUE:
        case KAD_FIND_NODE:
                if (dht_get_state(dht) != DHT_RUNNING)
                        break;
                kad_handle_find_resp(dht, req, msg);
                break;
        default:
                break;
        }

        kad_req_destroy(req);
}

int dht_bootstrap(void * dir)
{
        struct dht * dht;

        dht = (struct dht *) dir;

        assert(dht);

        pthread_rwlock_wrlock(&dht->lock);

#ifndef __DHT_TEST__
        dht->b        = hash_len(ipcpi.dir_hash_algo);
#else
        dht->b        = DHT_TEST_KEY_LEN;
#endif

        dht->id = create_id(dht->b);
        if (dht->id == NULL)
                goto fail_id;

        dht->buckets = bucket_create();
        if (dht->buckets == NULL)
                goto fail_buckets;

        dht->buckets->depth = 0;
        dht->buckets->mask  = 0;

        dht->t_expire = 86400; /* 1 day */
        dht->t_repub  = dht->t_expire - 10;
        dht->k        = KAD_K;

        if (pthread_create(&dht->worker, NULL, work, dht))
                goto fail_pthread_create;

        dht->state = DHT_RUNNING;

        dht_update_bucket(dht, dht->id, dht->addr);

        pthread_rwlock_unlock(&dht->lock);

        return 0;

 fail_pthread_create:
        bucket_destroy(dht->buckets);
        dht->buckets = NULL;
 fail_buckets:
        free(dht->id);
        dht->id = NULL;
 fail_id:
        pthread_rwlock_unlock(&dht->lock);
        return -1;
}

static struct ref_entry * ref_entry_get(struct dht *    dht,
                                        const uint8_t * key)
{
        struct list_head * p;

        list_for_each(p, &dht->refs) {
                struct ref_entry * r = list_entry(p, struct ref_entry, next);
                if (!memcmp(key, r->key, dht-> b) )
                        return r;
        }

        return NULL;
}

int dht_reg(void *          dir,
            const uint8_t * key)
{
        struct dht *       dht;
        struct ref_entry * e;
        uint64_t           addr;
        time_t             t_expire;

        dht = (struct dht *) dir;

        assert(dht);
        assert(key);
        assert(dht->addr != 0);

        if (dht_wait_running(dht))
                return -1;

        pthread_rwlock_wrlock(&dht->lock);

        if (ref_entry_get(dht, key) != NULL) {
                log_dbg("Name already registered.");
                pthread_rwlock_unlock(&dht->lock);
                return 0;
        }

        e = ref_entry_create(dht, key);
        if (e == NULL) {
                pthread_rwlock_unlock(&dht->lock);
                return -ENOMEM;
        }

        list_add(&e->next, &dht->refs);

        t_expire = dht->t_expire;
        addr = dht->addr;

        pthread_rwlock_unlock(&dht->lock);

        kad_publish(dht, key, addr, t_expire);

        return 0;
}

int dht_unreg(void *          dir,
              const uint8_t * key)
{
        struct dht *       dht;
        struct list_head * p;
        struct list_head * h;

        dht = (struct dht *) dir;

        assert(dht);
        assert(key);

        if (dht_get_state(dht) != DHT_RUNNING)
                return -1;

        pthread_rwlock_wrlock(&dht->lock);

        list_for_each_safe(p, h, &dht->refs) {
                struct ref_entry * r = list_entry(p, struct ref_entry, next);
                if (!memcmp(key, r->key, dht-> b) ) {
                        list_del(&r->next);
                        ref_entry_destroy(r);
                }
        }

        dht_del(dht, key, dht->addr);

        pthread_rwlock_unlock(&dht->lock);

        return 0;
}

uint64_t dht_query(void *          dir,
                   const uint8_t * key)
{
        struct dht *       dht;
        struct dht_entry * e;
        struct lookup *    lu;
        uint64_t           addrs[KAD_K];
        size_t             n;

        dht = (struct dht *) dir;

        assert(dht);

        addrs[0] = 0;

        if (dht_wait_running(dht))
                return 0;

        pthread_rwlock_rdlock(&dht->lock);

        e = dht_find_entry(dht, key);
        if (e != NULL)
                addrs[0] = dht_entry_get_addr(dht, e);

        pthread_rwlock_unlock(&dht->lock);

        if (addrs[0] != 0)
                return addrs[0];

        lu = kad_lookup(dht, key, KAD_FIND_VALUE);
        if (lu == NULL)
                return 0;

        n = lookup_get_addrs(lu, addrs);
        if (n == 0) {
                lookup_destroy(lu);
                return 0;
        }

        lookup_destroy(lu);

        /* Current behaviour is anycast and return the first peer address. */
        if (addrs[0] != dht->addr)
                return addrs[0];

        if (n > 1)
                return addrs[1];

        return 0;
}

static void * dht_handle_packet(void * o)
{
        struct dht * dht = (struct dht *) o;

        assert(dht);

        while (true) {
                kad_msg_t *          msg;
                kad_contact_msg_t ** cmsgs;
                kad_msg_t            resp_msg = KAD_MSG__INIT;
                uint64_t             addr;
                buffer_t             buf;
                size_t               i;
                size_t               b;
                size_t               t_expire;
                struct cmd *         cmd;

                pthread_mutex_lock(&dht->mtx);

                pthread_cleanup_push(__cleanup_mutex_unlock, &dht->mtx);

                while (list_is_empty(&dht->cmds))
                        pthread_cond_wait(&dht->cond, &dht->mtx);

                cmd = list_last_entry(&dht->cmds, struct cmd, next);
                list_del(&cmd->next);

                pthread_cleanup_pop(true);

                i = shm_du_buff_len(cmd->sdb);

                msg = kad_msg__unpack(NULL, i, shm_du_buff_head(cmd->sdb));
#ifndef __DHT_TEST__
                ipcp_sdb_release(cmd->sdb);
#endif
                free(cmd);

                if (msg == NULL) {
                        log_err("Failed to unpack message.");
                        continue;
                }

                if (msg->code != KAD_RESPONSE && dht_wait_running(dht)) {
                        kad_msg__free_unpacked(msg, NULL);
                        log_dbg("Got a request message when not running.");
                        continue;
                }

                pthread_rwlock_rdlock(&dht->lock);

                b        = dht->b;
                t_expire = dht->t_expire;

                pthread_rwlock_unlock(&dht->lock);

                if (msg->has_key && msg->key.len != b) {
                        kad_msg__free_unpacked(msg, NULL);
                        log_warn("Bad key in message.");
                        continue;
                }

                if (msg->has_s_id && !msg->has_b && msg->s_id.len != b) {
                        kad_msg__free_unpacked(msg, NULL);
                        log_warn("Bad source ID in message of type %d.",
                                 msg->code);
                        continue;
                }

                tpm_dec(dht->tpm);

                addr = msg->s_addr;

                resp_msg.code   = KAD_RESPONSE;
                resp_msg.cookie = msg->cookie;

                switch(msg->code) {
                case KAD_JOIN:
                        /* Refuse enrollee on check fails. */
                        if (msg->alpha != KAD_ALPHA || msg->k != KAD_K) {
                                log_warn("Parameter mismatch. "
                                         "DHT enrolment refused.");
                                break;
                        }

                        if (msg->t_replicate != KAD_T_REPL) {
                                log_warn("Replication time mismatch. "
                                         "DHT enrolment refused.");

                                break;
                        }

                        if (msg->t_refresh != KAD_T_REFR) {
                                log_warn("Refresh time mismatch. "
                                         "DHT enrolment refused.");
                                break;
                        }

                        resp_msg.has_alpha       = true;
                        resp_msg.has_b           = true;
                        resp_msg.has_k           = true;
                        resp_msg.has_t_expire    = true;
                        resp_msg.has_t_refresh   = true;
                        resp_msg.has_t_replicate = true;
                        resp_msg.alpha           = KAD_ALPHA;
                        resp_msg.b               = b;
                        resp_msg.k               = KAD_K;
                        resp_msg.t_expire        = t_expire;
                        resp_msg.t_refresh       = KAD_T_REFR;
                        resp_msg.t_replicate     = KAD_T_REPL;
                        break;
                case KAD_FIND_VALUE:
                        buf = dht_retrieve(dht, msg->key.data);
                        if (buf.len != 0) {
                                resp_msg.n_addrs = buf.len;
                                resp_msg.addrs   = (uint64_t *) buf.data;
                                break;
                        }
                        /* FALLTHRU */
                case KAD_FIND_NODE:
                        /* Return k closest contacts. */
                        resp_msg.n_contacts =
                                dht_get_contacts(dht, msg->key.data, &cmsgs);
                        resp_msg.contacts = cmsgs;
                        break;
                case KAD_STORE:
                        if (msg->n_contacts < 1) {
                                log_warn("No contacts in store message.");
                                break;
                        }

                        if (!msg->has_t_expire) {
                                log_warn("No expiry time in store message.");
                                break;
                        }

                        kad_add(dht, *msg->contacts, msg->n_contacts,
                                msg->t_expire);
                        break;
                case KAD_RESPONSE:
                        kad_handle_response(dht, msg);
                        break;
                default:
                        assert(false);
                        break;
                }

                if (msg->code != KAD_JOIN) {
                        pthread_rwlock_wrlock(&dht->lock);
                        if (dht_get_state(dht) == DHT_JOINING &&
                            dht->buckets == NULL) {
                                pthread_rwlock_unlock(&dht->lock);
                                goto finish;
                        }

                        if (dht_update_bucket(dht, msg->s_id.data, addr))
                                log_warn("Failed to update bucket.");
                        pthread_rwlock_unlock(&dht->lock);
                }

                if (msg->code < KAD_STORE && send_msg(dht, &resp_msg, addr) < 0)
                                log_warn("Failed to send response.");

 finish:
                kad_msg__free_unpacked(msg, NULL);

                if (resp_msg.n_addrs > 0)
                        free(resp_msg.addrs);

                if (resp_msg.n_contacts == 0) {
                        tpm_inc(dht->tpm);
                        continue;
                }

                for (i = 0; i < resp_msg.n_contacts; ++i)
                        kad_contact_msg__free_unpacked(resp_msg.contacts[i],
                                                       NULL);
                free(resp_msg.contacts);

                tpm_inc(dht->tpm);
        }

        return (void *) 0;
}

static void dht_post_packet(void *               comp,
                            struct shm_du_buff * sdb)
{
        struct cmd * cmd;
        struct dht * dht = (struct dht *) comp;

        if (dht_get_state(dht) == DHT_SHUTDOWN) {
#ifndef __DHT_TEST__
                ipcp_sdb_release(sdb);
#endif
                return;
        }

        cmd = malloc(sizeof(*cmd));
        if (cmd == NULL) {
                log_err("Command failed. Out of memory.");
                return;
        }

        cmd->sdb = sdb;

        pthread_mutex_lock(&dht->mtx);

        list_add(&cmd->next, &dht->cmds);

        pthread_cond_signal(&dht->cond);

        pthread_mutex_unlock(&dht->mtx);
}

void dht_destroy(void * dir)
{
        struct dht *       dht;
        struct list_head * p;
        struct list_head * h;

        dht = (struct dht *) dir;
        if (dht == NULL)
                return;

#ifndef __DHT_TEST__
        tpm_stop(dht->tpm);

        tpm_destroy(dht->tpm);
#endif
        if (dht_get_state(dht) == DHT_RUNNING) {
                dht_set_state(dht, DHT_SHUTDOWN);
                pthread_cancel(dht->worker);
                pthread_join(dht->worker, NULL);
        }

        pthread_rwlock_wrlock(&dht->lock);

        list_for_each_safe(p, h, &dht->cmds) {
                struct cmd * c = list_entry(p, struct cmd, next);
                list_del(&c->next);
#ifndef __DHT_TEST__
                ipcp_sdb_release(c->sdb);
#endif
                free(c);
        }

        list_for_each_safe(p, h, &dht->entries) {
                struct dht_entry * e = list_entry(p, struct dht_entry, next);
                list_del(&e->next);
                dht_entry_destroy(e);
        }

        list_for_each_safe(p, h, &dht->requests) {
                struct kad_req * r = list_entry(p, struct kad_req, next);
                list_del(&r->next);
                kad_req_destroy(r);
        }

        list_for_each_safe(p, h, &dht->refs) {
                struct ref_entry * e = list_entry(p, struct ref_entry, next);
                list_del(&e->next);
                ref_entry_destroy(e);
        }

        list_for_each_safe(p, h, &dht->lookups) {
                struct lookup * l = list_entry(p, struct lookup, next);
                list_del(&l->next);
                lookup_destroy(l);
        }

        pthread_rwlock_unlock(&dht->lock);

        if (dht->buckets != NULL)
                bucket_destroy(dht->buckets);

        bmp_destroy(dht->cookies);

        pthread_mutex_destroy(&dht->mtx);

        pthread_rwlock_destroy(&dht->lock);

        free(dht->id);

        free(dht);
}

static void * join_thr(void * o)
{
        struct join_info * info = (struct join_info *) o;
        struct lookup *    lu;
        size_t             retr = 0;

        assert(info);

        while (kad_join(info->dht, info->addr)) {
                if (dht_get_state(info->dht) == DHT_SHUTDOWN) {
                        log_dbg("DHT enrollment aborted.");
                        goto finish;
                }

                if (retr++ == KAD_JOIN_RETR) {
                        dht_set_state(info->dht, DHT_INIT);
                        log_warn("DHT enrollment attempt failed.");
                        goto finish;
                }

                sleep(KAD_JOIN_INTV);
        }

        dht_set_state(info->dht, DHT_RUNNING);

        lu = kad_lookup(info->dht, info->dht->id, KAD_FIND_NODE);
        if (lu != NULL)
                lookup_destroy(lu);

 finish:
        free(info);

        return (void *) 0;
}

static void handle_event(void *       self,
                         int          event,
                         const void * o)
{
        struct dht * dht = (struct dht *) self;

        if (event == NOTIFY_DT_CONN_ADD) {
                pthread_t          thr;
                struct join_info * inf;
                struct conn *      c     = (struct conn *) o;
                struct timespec    slack = {0, DHT_ENROLL_SLACK * MILLION};

                /* Give the pff some time to update for the new link. */
                nanosleep(&slack, NULL);

                switch(dht_get_state(dht)) {
                case DHT_INIT:
                        inf = malloc(sizeof(*inf));
                        if (inf == NULL)
                                break;

                        inf->dht  = dht;
                        inf->addr = c->conn_info.addr;

                        if (dht_set_state(dht, DHT_JOINING) == 0 ||
                            dht_wait_running(dht)) {
                                if (pthread_create(&thr, NULL, join_thr, inf)) {
                                        dht_set_state(dht, DHT_INIT);
                                        free(inf);
                                        return;
                                }
                                pthread_detach(thr);
                        } else {
                                free(inf);
                        }
                        break;
                case DHT_RUNNING:
                        /*
                         * FIXME: this lookup for effiency reasons
                         * causes a SEGV when stressed with rapid
                         * enrollments.
                         * lu = kad_lookup(dht, dht->id, KAD_FIND_NODE);
                         * if (lu != NULL)
                         *         lookup_destroy(lu);
                         */
                        break;
                default:
                        break;
                }
        }
}

void * dht_create(void)
{
        struct dht * dht;

        dht = malloc(sizeof(*dht));
        if (dht == NULL)
                goto fail_malloc;

        dht->buckets = NULL;

        list_head_init(&dht->entries);
        list_head_init(&dht->requests);
        list_head_init(&dht->refs);
        list_head_init(&dht->lookups);
        list_head_init(&dht->cmds);

        if (pthread_rwlock_init(&dht->lock, NULL))
                goto fail_rwlock;

        if (pthread_mutex_init(&dht->mtx, NULL))
                goto fail_mutex;

        if (pthread_cond_init(&dht->cond, NULL))
                goto fail_cond;

        dht->cookies = bmp_create(DHT_MAX_REQS, 1);
        if (dht->cookies == NULL)
                goto fail_bmp;

        dht->b    = 0;
        dht->id   = NULL;
#ifndef __DHT_TEST__
        dht->addr = ipcpi.dt_addr;
        dht->tpm = tpm_create(2, 1, dht_handle_packet, dht);
        if (dht->tpm == NULL)
                goto fail_tpm_create;

        if (tpm_start(dht->tpm))
                goto fail_tpm_start;

        dht->eid   = dt_reg_comp(dht, &dht_post_packet, DHT);
        if ((int) dht->eid < 0)
                goto fail_tpm_start;

        if (notifier_reg(handle_event, dht))
                goto fail_notifier_reg;
#else
        (void) handle_event;
        (void) dht_handle_packet;
        (void) dht_post_packet;
#endif
        dht->state = DHT_INIT;

        return (void *) dht;
#ifndef __DHT_TEST__
 fail_notifier_reg:
        tpm_stop(dht->tpm);
 fail_tpm_start:
        tpm_destroy(dht->tpm);
 fail_tpm_create:
        bmp_destroy(dht->cookies);
#endif
 fail_bmp:
        pthread_cond_destroy(&dht->cond);
 fail_cond:
        pthread_mutex_destroy(&dht->mtx);
 fail_mutex:
        pthread_rwlock_destroy(&dht->lock);
 fail_rwlock:
        free(dht);
 fail_malloc:
        return NULL;
}
