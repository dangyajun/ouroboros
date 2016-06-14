/*
 * Ouroboros - Copyright (C) 2016
 *
 * Time utilities
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

#ifndef OUROBOROS_TIME_UTILS_H
#define OUROBOROS_TIME_UTILS_H

#ifdef MILLION
#undef MILLION
#endif

#ifdef BILLION
#undef BILLION
#endif

#define MILLION  1000000L
#define BILLION  1000000000L

#include <sys/time.h>
#include <limits.h> /* LONG_MAX */

/* functions for timespecs */
#define ts_diff_ns(t0, tx) (((tx)->tv_sec - (t0)->tv_sec) * BILLION     \
                            + ((tx)->tv_nsec - (t0)->tv_nsec))
#define ts_diff_us(t0, tx) (((tx)->tv_sec - (t0)->tv_sec) * MILLION     \
                            + ((tx)->tv_nsec - (t0)->tv_nsec) / 1000L)
#define ts_diff_ms(t0, tx) (((tx)->tv_sec - (t0)->tv_sec) * 1000L       \
                            + ((tx)->tv_nsec - (t0)->tv_nsec) / MILLION)

/* functions for timevals are the same */
#define tv_diff_us(t0, tx) (((tx)->tv_sec - (t0)->tv_sec) * MILLION     \
                            + ((tx)->tv_usec - (t0)->tv_usec) / 1000L)
#define tv_diff_ms(t0, tx) (((tx)->tv_sec - (t0)->tv_sec) * 1000L       \
                            + ((tx)->tv_usec - (t0)->tv_usec) / MILLION)

/* functions for timespecs */
int ts_add(const struct timespec * t,
           const struct timespec * intv,
           struct timespec *       res);

int ts_diff(const struct timespec * t,
            const struct timespec * intv,
            struct timespec *       res);

/* functions for timevals */
int tv_add(const struct timeval * t,
           const struct timeval * intv,
           struct timeval *       res);

int tv_diff(const struct timeval * t,
            const struct timeval * intv,
            struct timeval *       res);

/* copying a timeval into a timespec */
int tv_to_ts(const struct timeval * src,
             struct timespec *      dst);

/* copying a timespec into a timeval (loss of resolution) */
int ts_to_tv(const struct timespec * src,
             struct timeval *        dst);

#endif /* OUROBOROS_TIME_UTILS_H */