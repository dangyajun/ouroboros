/*
 * Ouroboros - Copyright (C) 2016
 *
 * Ring buffer for application processes
 *
 *    Dimitri Staessens <dimitri.staessens@intec.ugent.be>
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

#include <ouroboros/config.h>
#include <ouroboros/errno.h>

#define OUROBOROS_PREFIX "shm_ap_rbuff"

#include <ouroboros/logs.h>
#include <ouroboros/shm_ap_rbuff.h>
#include <ouroboros/lockfile.h>

#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#define SHM_RBUFF_FILE_SIZE (SHM_RBUFF_SIZE * sizeof(struct rb_entry)          \
                             + 2 * sizeof(size_t) + sizeof(pthread_mutex_t)    \
                             + sizeof (pthread_cond_t))

#define shm_rbuff_used(rb)((*rb->ptr_head + SHM_RBUFF_SIZE - *rb->ptr_tail)    \
                          & (SHM_RBUFF_SIZE - 1))
#define shm_rbuff_free(rb)(shm_rbuff_used(rb) + 1 < SHM_RBUFF_SIZE)
#define shm_rbuff_empty(rb) (*rb->ptr_head == *rb->ptr_tail)
#define head_el_ptr (rb->shm_base + *rb->ptr_head)
#define tail_el_ptr (rb->shm_base + *rb->ptr_tail)

struct shm_ap_rbuff {
        struct rb_entry * shm_base;    /* start of entry */
        size_t *          ptr_head;    /* start of ringbuffer head */
        size_t *          ptr_tail;    /* start of ringbuffer tail */
        pthread_mutex_t * shm_mutex;   /* lock all free space in shm */
        pthread_cond_t *  work;        /* threads will wait for a signal */
        pid_t             api;         /* api to which this rb belongs */
        int               fd;
};

struct shm_ap_rbuff * shm_ap_rbuff_create()
{
        struct shm_ap_rbuff * rb;
        int                   shm_fd;
        struct rb_entry *     shm_base;
        pthread_mutexattr_t   mattr;
        pthread_condattr_t    cattr;
        char                  fn[25];

        sprintf(fn, SHM_AP_RBUFF_PREFIX "%d", getpid());

        rb = malloc(sizeof(*rb));
        if (rb == NULL) {
                LOG_DBGF("Could not allocate struct.");
                return NULL;
        }

        shm_fd = shm_open(fn, O_CREAT | O_EXCL | O_RDWR, 0666);
        if (shm_fd == -1) {
                LOG_DBGF("Failed creating ring buffer.");
                free(rb);
                return NULL;
        }

        if (fchmod(shm_fd, 0666)) {
                LOG_DBGF("Failed to chmod shared memory.");
                free(rb);
                return NULL;
        }

        if (ftruncate(shm_fd, SHM_RBUFF_FILE_SIZE - 1) < 0) {
                LOG_DBGF("Failed to extend ringbuffer.");
                free(rb);
                return NULL;
        }

        if (write(shm_fd, "", 1) != 1) {
                LOG_DBGF("Failed to finalise extension of ringbuffer.");
                free(rb);
                return NULL;
        }

        shm_base = mmap(NULL,
                        SHM_RBUFF_FILE_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        shm_fd,
                        0);

        if (shm_base == MAP_FAILED) {
                LOG_DBGF("Failed to map shared memory.");
                if (close(shm_fd) == -1)
                        LOG_DBGF("Failed to close invalid shm.");

                if (shm_unlink(fn) == -1)
                        LOG_DBGF("Failed to remove invalid shm.");

                free(rb);
                return NULL;
        }

        rb->shm_base  = shm_base;
        rb->ptr_head  = (size_t *) (rb->shm_base + SHM_RBUFF_SIZE);
        rb->ptr_tail  = rb->ptr_head + 1;
        rb->shm_mutex = (pthread_mutex_t *) (rb->ptr_tail + 1);
        rb->work      = (pthread_cond_t *) (rb->shm_mutex + 1);

        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(rb->shm_mutex, &mattr);

        pthread_condattr_init(&cattr);
        pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(rb->work, &cattr);

        *rb->ptr_head = 0;
        *rb->ptr_tail = 0;

        rb->fd  = shm_fd;
        rb->api = getpid();

        return rb;
}

struct shm_ap_rbuff * shm_ap_rbuff_open(pid_t api)
{
        struct shm_ap_rbuff * rb;
        int                   shm_fd;
        struct rb_entry *     shm_base;
        char                  fn[25];

        sprintf(fn, SHM_AP_RBUFF_PREFIX "%d", api);

        rb = malloc(sizeof(*rb));
        if (rb == NULL) {
                LOG_DBGF("Could not allocate struct.");
                return NULL;
        }

        shm_fd = shm_open(fn, O_RDWR, 0666);
        if (shm_fd == -1) {
                LOG_DBGF("%d failed opening shared memory %s.", getpid(), fn);
                return NULL;
        }

        shm_base = mmap(NULL,
                        SHM_RBUFF_FILE_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        shm_fd,
                        0);

        if (shm_base == MAP_FAILED) {
                LOG_DBGF("Failed to map shared memory.");
                if (close(shm_fd) == -1)
                        LOG_DBGF("Failed to close invalid shm.");

                if (shm_unlink(fn) == -1)
                        LOG_DBGF("Failed to remove invalid shm.");

                free(rb);
                return NULL;
        }

        rb->shm_base  = shm_base;
        rb->ptr_head  = (size_t *) (rb->shm_base + SHM_RBUFF_SIZE);
        rb->ptr_tail  = rb->ptr_head + 1;
        rb->shm_mutex = (pthread_mutex_t *) (rb->ptr_tail + 1);
        rb->work      = (pthread_cond_t *) (rb->shm_mutex + 1);

        rb->fd = shm_fd;
        rb->api = api;

        return rb;
}
void shm_ap_rbuff_close(struct shm_ap_rbuff * rb)
{
        if (rb == NULL) {
                LOG_DBGF("Bogus input. Bugging out.");
                return;
        }

        if (close(rb->fd) < 0)
                LOG_DBGF("Couldn't close shared memory.");

        if (munmap(rb->shm_base, SHM_RBUFF_FILE_SIZE) == -1)
                LOG_DBGF("Couldn't unmap shared memory.");

        free(rb);
}

void shm_ap_rbuff_destroy(struct shm_ap_rbuff * rb)
{
        char fn[25];
        struct lockfile * lf = NULL;

        if (rb == NULL) {
                LOG_DBGF("Bogus input. Bugging out.");
                return;
        }

        if (rb->api != getpid()) {
                lf = lockfile_open();
                if (lf == NULL)
                        return;
                if (lockfile_owner(lf) == getpid()) {
                        LOG_DBGF("Ringbuffer %d destroyed by IRMd %d.",
                                 rb->api, getpid());
                        lockfile_close(lf);
                } else {
                        LOG_ERR("AP-I %d tried to destroy rbuff owned by %d.",
                                getpid(), rb->api);
                        lockfile_close(lf);
                        return;
                }
        }

        if (close(rb->fd) < 0)
                LOG_DBGF("Couldn't close shared memory.");

        sprintf(fn, SHM_AP_RBUFF_PREFIX "%d", rb->api);

        if (munmap(rb->shm_base, SHM_RBUFF_FILE_SIZE) == -1)
                LOG_DBGF("Couldn't unmap shared memory.");

        if (shm_unlink(fn) == -1)
                LOG_DBGF("Failed to unlink shm.");

        free(rb);
}

int shm_ap_rbuff_write(struct shm_ap_rbuff * rb, struct rb_entry * e)
{
        if (rb == NULL || e == NULL)
                return -1;

        if (pthread_mutex_lock(rb->shm_mutex) == EOWNERDEAD) {
                LOG_DBGF("Recovering dead mutex.");
                pthread_mutex_consistent(rb->shm_mutex);
        }

        if (!shm_rbuff_free(rb)) {
                pthread_mutex_unlock(rb->shm_mutex);
                return -1;
        }

        if (shm_rbuff_empty(rb))
                pthread_cond_broadcast(rb->work);

        *head_el_ptr = *e;
        *rb->ptr_head = (*rb->ptr_head + 1) & (SHM_RBUFF_SIZE -1);

        pthread_mutex_unlock(rb->shm_mutex);

        return 0;
}

struct rb_entry * shm_ap_rbuff_read(struct shm_ap_rbuff * rb)
{
        struct rb_entry * e = NULL;

        if (rb == NULL)
                return NULL;

        pthread_cleanup_push((void(*)(void *))pthread_mutex_unlock,
                             (void *) rb->shm_mutex);

        if (pthread_mutex_lock(rb->shm_mutex) == EOWNERDEAD) {
                LOG_DBGF("Recovering dead mutex.");
                pthread_mutex_consistent(rb->shm_mutex);
        }

        while (tail_el_ptr->port_id < 0)
                *rb->ptr_tail = (*rb->ptr_tail + 1) & (SHM_RBUFF_SIZE -1);

        while(shm_rbuff_empty(rb))
                if (pthread_cond_wait(rb->work, rb->shm_mutex)
                    == EOWNERDEAD) {
                LOG_DBGF("Recovering dead mutex.");
                pthread_mutex_consistent(rb->shm_mutex);
        }

        e = malloc(sizeof(*e));
        if (e == NULL) {
                pthread_mutex_unlock(rb->shm_mutex);
                return NULL;
        }

        *e = *(rb->shm_base + *rb->ptr_tail);

        *rb->ptr_tail = (*rb->ptr_tail + 1) & (SHM_RBUFF_SIZE -1);

        pthread_cleanup_pop(1);

        return e;
}

ssize_t shm_ap_rbuff_read_port(struct shm_ap_rbuff * rb, int port_id)
{
        ssize_t idx = -1;

        if (pthread_mutex_lock(rb->shm_mutex) == EOWNERDEAD) {
                LOG_DBGF("Recovering dead mutex.");
                pthread_mutex_consistent(rb->shm_mutex);
        }

        if (shm_rbuff_empty(rb)) {
                pthread_mutex_unlock(rb->shm_mutex);
                return -1;
        }

        while (tail_el_ptr->port_id < 0)
                *rb->ptr_tail = (*rb->ptr_tail + 1) & (SHM_RBUFF_SIZE -1);

        if (tail_el_ptr->port_id != port_id) {
                pthread_mutex_unlock(rb->shm_mutex);
                return -1;
        }

        idx = tail_el_ptr->index;

        *rb->ptr_tail = (*rb->ptr_tail + 1) & (SHM_RBUFF_SIZE -1);

        pthread_mutex_unlock(rb->shm_mutex);

        return idx;
}

pid_t shm_ap_rbuff_get_api(struct shm_ap_rbuff *rb)
{
        pid_t api = -1;
        if (rb == NULL)
                return -1;

        pthread_mutex_lock(rb->shm_mutex);
        api = rb->api;
        pthread_mutex_unlock(rb->shm_mutex);

        return api;
}

void shm_ap_rbuff_reset(struct shm_ap_rbuff * rb)
{
        if (rb == NULL)
                return;

        pthread_mutex_lock(rb->shm_mutex);
        *rb->ptr_tail = 0;
        *rb->ptr_head = 0;
        pthread_mutex_unlock(rb->shm_mutex);
}
