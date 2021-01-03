/*
 * Ouroboros - Copyright (C) 2016 - 2021
 *
 * Undirected graph structure
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *    Nick Aerts        <nick.aerts@ugent.be>
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

#if defined(__linux__) || defined(__CYGWIN__)
#define _DEFAULT_SOURCE
#else
#define _POSIX_C_SOURCE 200112L
#endif

#define OUROBOROS_PREFIX "graph"

#include <ouroboros/logs.h>
#include <ouroboros/errno.h>
#include <ouroboros/list.h>

#include "graph.h"
#include "ipcp.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

struct vertex {
        struct list_head next;
        uint64_t         addr;
        struct list_head edges;
        int              index;
};

struct edge {
        struct list_head next;
        struct vertex *  nb;
        qosspec_t        qs;
        int              announced;
};

struct graph {
        size_t           nr_vertices;
        struct list_head vertices;
        pthread_mutex_t  lock;
};

static struct edge * find_edge_by_addr(struct vertex * vertex,
                                       uint64_t        dst_addr)
{
        struct list_head * p;

        assert(vertex);

        list_for_each(p, &vertex->edges) {
                struct edge * e = list_entry(p, struct edge, next);
                if (e->nb->addr == dst_addr)
                        return e;
        }

        return NULL;
}

static struct vertex * find_vertex_by_addr(struct graph * graph,
                                           uint64_t       addr)
{
        struct list_head * p;

        assert(graph);

        list_for_each(p, &graph->vertices) {
                struct vertex * e = list_entry(p, struct vertex, next);
                if (e->addr == addr)
                        return e;
        }

        return NULL;
}

static struct edge * add_edge(struct vertex * vertex,
                              struct vertex * nb)
{
        struct edge * edge;

        assert(vertex);
        assert(nb);

        edge = malloc(sizeof(*edge));
        if (edge == NULL)
                return NULL;

        edge->nb = nb;
        edge->announced = 0;

        list_add(&edge->next, &vertex->edges);

        return edge;
}

static void del_edge(struct edge * edge)
{
        assert(edge);

        list_del(&edge->next);
        free(edge);
}

static struct vertex * add_vertex(struct graph * graph,
                                  uint64_t       addr)
{
        struct vertex *    vertex;
        struct list_head * p;
        int                i = 0;

        assert(graph);

        vertex = malloc(sizeof(*vertex));
        if (vertex == NULL)
                return NULL;

        list_head_init(&vertex->edges);
        vertex->addr = addr;

        /* Keep them ordered on address. */
        list_for_each(p, &graph->vertices) {
                struct vertex * v = list_entry(p, struct vertex, next);
                if (v->addr > addr)
                        break;
                i++;
        }

        vertex->index = i;

        list_add_tail(&vertex->next, p);

        /* Increase the index of the vertices to the right. */
        list_for_each(p, &graph->vertices) {
                struct vertex * v = list_entry(p, struct vertex, next);
                if (v->addr > addr)
                        v->index++;
        }

        graph->nr_vertices++;

        return vertex;
}

static void del_vertex(struct graph *  graph,
                       struct vertex * vertex)
{
        struct list_head * p;
        struct list_head * h;

        assert(graph);
        assert(vertex);

        list_del(&vertex->next);

        /* Decrease the index of the vertices to the right. */
        list_for_each(p, &graph->vertices) {
                struct vertex * v = list_entry(p, struct vertex, next);
                if (v->addr > vertex->addr)
                        v->index--;
        }

        list_for_each_safe(p, h, &vertex->edges) {
                struct edge * e = list_entry(p, struct edge, next);
                del_edge(e);
        }

        free(vertex);

        graph->nr_vertices--;
}

struct graph * graph_create(void)
{
        struct graph * graph;

        graph = malloc(sizeof(*graph));
        if (graph == NULL)
                return NULL;

        if (pthread_mutex_init(&graph->lock, NULL)) {
                free(graph);
                return NULL;
        }

        graph->nr_vertices = 0;
        list_head_init(&graph->vertices);

        return graph;
}

void graph_destroy(struct graph * graph)
{
        struct list_head * p = NULL;
        struct list_head * n = NULL;

        assert(graph);

        pthread_mutex_lock(&graph->lock);

        list_for_each_safe(p, n, &graph->vertices) {
                struct vertex * e = list_entry(p, struct vertex, next);
                del_vertex(graph, e);
        }

        pthread_mutex_unlock(&graph->lock);

        pthread_mutex_destroy(&graph->lock);

        free(graph);
}

int graph_update_edge(struct graph * graph,
                      uint64_t       s_addr,
                      uint64_t       d_addr,
                      qosspec_t      qs)
{
        struct vertex * v;
        struct edge *   e;
        struct vertex * nb;
        struct edge *   nb_e;

        assert(graph);

        pthread_mutex_lock(&graph->lock);

        v = find_vertex_by_addr(graph, s_addr);
        if (v == NULL) {
                v = add_vertex(graph, s_addr);
                if (v == NULL) {
                        pthread_mutex_unlock(&graph->lock);
                        log_err("Failed to add vertex.");
                        return -ENOMEM;
                }
        }

        nb = find_vertex_by_addr(graph, d_addr);
        if (nb == NULL) {
                nb = add_vertex(graph, d_addr);
                if (nb == NULL) {
                        if (list_is_empty(&v->edges))
                                del_vertex(graph, v);
                        pthread_mutex_unlock(&graph->lock);
                        log_err("Failed to add vertex.");
                        return -ENOMEM;
                }
        }

        e = find_edge_by_addr(v, d_addr);
        if (e == NULL) {
                e = add_edge(v, nb);
                if (e == NULL) {
                        if (list_is_empty(&v->edges))
                                del_vertex(graph, v);
                        if (list_is_empty(&nb->edges))
                                del_vertex(graph, nb);
                        pthread_mutex_unlock(&graph->lock);
                        log_err("Failed to add edge.");
                        return -ENOMEM;
                }
        }

        e->announced++;
        e->qs = qs;

        nb_e = find_edge_by_addr(nb, s_addr);
        if (nb_e == NULL) {
                nb_e = add_edge(nb, v);
                if (nb_e == NULL) {
                        if (--e->announced == 0)
                                del_edge(e);
                        if (list_is_empty(&v->edges))
                                del_vertex(graph, v);
                        if (list_is_empty(&nb->edges))
                                del_vertex(graph, nb);
                        pthread_mutex_unlock(&graph->lock);
                        log_err("Failed to add edge.");
                        return -ENOMEM;
                }
        }

        nb_e->announced++;
        nb_e->qs = qs;

        pthread_mutex_unlock(&graph->lock);

        return 0;
}

int graph_del_edge(struct graph * graph,
                   uint64_t       s_addr,
                   uint64_t       d_addr)
{
        struct vertex * v;
        struct edge *   e;
        struct vertex * nb;
        struct edge *   nb_e;

        assert(graph);

        pthread_mutex_lock(&graph->lock);

        v = find_vertex_by_addr(graph, s_addr);
        if (v == NULL) {
                pthread_mutex_unlock(&graph->lock);
                log_err("No such source vertex.");
                return -1;
        }

        nb = find_vertex_by_addr(graph, d_addr);
        if (nb == NULL) {
                pthread_mutex_unlock(&graph->lock);
                log_err("No such destination vertex.");
                return -1;
        }

        e = find_edge_by_addr(v, d_addr);
        if (e == NULL) {
                pthread_mutex_unlock(&graph->lock);
                log_err("No such source edge.");
                return -1;
        }

        nb_e = find_edge_by_addr(nb, s_addr);
        if (nb_e == NULL) {
                pthread_mutex_unlock(&graph->lock);
                log_err("No such destination edge.");
                return -1;
        }

        if (--e->announced == 0)
                del_edge(e);
        if (--nb_e->announced == 0)
                del_edge(nb_e);

        /* Removing vertex if it was the last edge */
        if (list_is_empty(&v->edges))
                del_vertex(graph, v);
        if (list_is_empty(&nb->edges))
                del_vertex(graph, nb);

        pthread_mutex_unlock(&graph->lock);

        return 0;
}

static int get_min_vertex(struct graph *   graph,
                          int *            dist,
                          bool *           used,
                          struct vertex ** v)
{
        int                min = INT_MAX;
        int                index = -1;
        int                i = 0;
        struct list_head * p;

        assert(v);
        assert(graph);
        assert(dist);
        assert(used);

        *v = NULL;

        list_for_each(p, &graph->vertices) {
                if (!used[i] && dist[i] < min) {
                        min = dist[i];
                        index = i;
                        *v = list_entry(p, struct vertex, next);
                }

                i++;
        }

        if (index != -1)
                used[index] = true;

        return index;
}

static int dijkstra(struct graph *    graph,
                    uint64_t          src,
                    struct vertex *** nhops,
                    int **            dist)
{
        bool *             used;
        struct list_head * p = NULL;
        int                i = 0;
        struct vertex *    v = NULL;
        struct edge *      e = NULL;
        int                alt;

        assert(graph);
        assert(nhops);
        assert(dist);

        *nhops = malloc(sizeof(**nhops) * graph->nr_vertices);
        if (*nhops == NULL)
                goto fail_pnhops;

        *dist = malloc(sizeof(**dist) * graph->nr_vertices);
        if (*dist == NULL)
                goto fail_pdist;

        used = malloc(sizeof(*used) * graph->nr_vertices);
        if (used == NULL)
                goto fail_used;

        /* Init the data structures */
        memset(used, 0, sizeof(*used) * graph->nr_vertices);
        memset(*nhops, 0, sizeof(**nhops) * graph->nr_vertices);
        memset(*dist, 0, sizeof(**dist) * graph->nr_vertices);

        list_for_each(p, &graph->vertices) {
                v = list_entry(p, struct vertex, next);
                (*dist)[i++]  = (v->addr == src) ? 0 : INT_MAX;
        }

        /* Perform actual Dijkstra */
        i = get_min_vertex(graph, *dist, used, &v);
        while (v != NULL) {
                list_for_each(p, &v->edges) {
                        e = list_entry(p, struct edge, next);

                        /* Only include it if both sides announced it. */
                        if (e->announced != 2)
                                continue;

                        /*
                         * NOTE: Current weight is just hop count.
                         * Method could be extended to use a different
                         * weight for a different QoS cube.
                         */
                        alt = (*dist)[i] + 1;
                        if (alt < (*dist)[e->nb->index]) {
                                (*dist)[e->nb->index] = alt;
                                if (v->addr == src)
                                        (*nhops)[e->nb->index] = e->nb;
                                else
                                        (*nhops)[e->nb->index] = (*nhops)[i];
                        }
                }
                i = get_min_vertex(graph, *dist, used, &v);
        }

        free(used);

        return 0;

 fail_used:
        free(*dist);
 fail_pdist:
        free(*nhops);
 fail_pnhops:
        return -1;

}

static void free_routing_table(struct list_head * table)
{
        struct list_head * h;
        struct list_head * p;
        struct list_head * q;
        struct list_head * i;

        assert(table);

        list_for_each_safe(p, h, table) {
                struct routing_table * t =
                        list_entry(p, struct routing_table, next);
                list_for_each_safe(q, i, &t->nhops) {
                        struct nhop * n =
                                list_entry(q, struct nhop, next);
                        list_del(&n->next);
                        free(n);
                }
                list_del(&t->next);
                free(t);
        }
}

void graph_free_routing_table(struct graph *     graph,
                              struct list_head * table)
{
        assert(table);

        pthread_mutex_lock(&graph->lock);

        free_routing_table(table);

        pthread_mutex_unlock(&graph->lock);
}

static int graph_routing_table_simple(struct graph *     graph,
                                      uint64_t           s_addr,
                                      struct list_head * table,
                                      int **             dist)
{
        struct vertex **       nhops;
        struct list_head *     p;
        int                    i = 0;
        struct vertex *        v;
        struct routing_table * t;
        struct nhop *          n;

        assert(graph);
        assert(table);
        assert(dist);

        /* We need at least 2 vertices for a table */
        if (graph->nr_vertices < 2)
                goto fail_vertices;

        if (dijkstra(graph, s_addr, &nhops, dist))
                goto fail_vertices;

        list_head_init(table);

        /* Now construct the routing table from the nhops. */
        list_for_each(p, &graph->vertices) {
                v = list_entry(p, struct vertex, next);

                /* This is the src */
                if (nhops[i] == NULL) {
                        i++;
                        continue;
                }

                t = malloc(sizeof(*t));
                if (t == NULL)
                        goto fail_t;

                list_head_init(&t->nhops);

                n = malloc(sizeof(*n));
                if (n == NULL)
                        goto fail_n;

                t->dst = v->addr;
                n->nhop = nhops[i]->addr;

                list_add(&n->next, &t->nhops);
                list_add(&t->next, table);

                i++;
        }

        free(nhops);

        return 0;

 fail_n:
        free(t);
 fail_t:
        free_routing_table(table);
        free(nhops);
        free(*dist);
 fail_vertices:
        *dist = NULL;
        return -1;
}

static int add_lfa_to_table(struct list_head * table,
                            uint64_t           addr,
                            uint64_t           lfa)
{
        struct list_head * p;
        struct nhop *      n;

        assert(table);

        n = malloc(sizeof(*n));
        if (n == NULL)
                return -1;

        n->nhop = lfa;

        list_for_each(p, table) {
                struct routing_table * t =
                        list_entry(p, struct routing_table, next);
                if (t->dst == addr) {
                        list_add_tail(&n->next, &t->nhops);
                        return 0;
                }
        }

        free(n);

        return -1;
}

static int graph_routing_table_lfa(struct graph *     graph,
                                   uint64_t           s_addr,
                                   struct list_head * table,
                                   int **             dist)
{
        int *              n_dist[PROG_MAX_FLOWS];
        uint64_t           addrs[PROG_MAX_FLOWS];
        int                n_index[PROG_MAX_FLOWS];
        struct list_head * p;
        struct list_head * q;
        struct vertex *    v;
        struct edge *      e;
        struct vertex **   nhops;
        int                i = 0;
        int                j;
        int                k;

        if (graph_routing_table_simple(graph, s_addr, table, dist))
                goto fail_table;

        for (j = 0; j < PROG_MAX_FLOWS; j++) {
                n_dist[j] = NULL;
                n_index[j] = -1;
                addrs[j] = -1;
        }

        list_for_each(p, &graph->vertices) {
                v = list_entry(p, struct vertex, next);

                if (v->addr != s_addr)
                        continue;

                /*
                 * Get the distances for every neighbor
                 * of the source.
                 */
                list_for_each(q, &v->edges) {
                        e = list_entry(q, struct edge, next);

                        addrs[i] = e->nb->addr;
                        n_index[i] = e->nb->index;
                        if (dijkstra(graph, e->nb->addr,
                                     &nhops, &(n_dist[i++])))
                                goto fail_dijkstra;

                        free(nhops);
                }

                break;
        }

        /* Loop though all nodes to see if we have a LFA for them. */
        list_for_each(p, &graph->vertices) {
                v = list_entry(p, struct vertex, next);

                if (v->addr == s_addr)
                        continue;

                /*
                 * Check for every neighbor if
                 * dist(neighbor, destination) <
                 * dist(neighbor, source) + dist(source, destination).
                 */
                for (j = 0; j < i; j++) {
                        /* Exclude ourselves. */
                        if (addrs[j] == v->addr)
                                continue;

                        if (n_dist[j][v->index] <
                            (*dist)[n_index[j]] + (*dist)[v->index])
                                if (add_lfa_to_table(table, v->addr,
                                                     addrs[j]))
                                        goto fail_add_lfa;
                }
        }

        for (j = 0; j < i; j++)
                free(n_dist[j]);

        return 0;

 fail_add_lfa:
        for (k = j; k < i; k++)
                free(n_dist[k]);
 fail_dijkstra:
        free_routing_table(table);
 fail_table:
        return -1;
}

static int graph_routing_table_ecmp(struct graph *     graph,
                                    uint64_t           s_addr,
                                    struct list_head * table,
                                    int **             dist)
{
        struct vertex **       nhops;
        struct list_head *     p;
        struct list_head *     h;
        size_t                 i;
        struct vertex *        v;
        struct vertex *        src_v;
        struct edge *          e;
        struct routing_table * t;
        struct nhop *          n;
        struct list_head *     forwarding;

        assert(graph);
        assert(dist);

        if (graph-> nr_vertices < 2)
                goto fail_vertices;

        forwarding = malloc(sizeof(*forwarding) * graph->nr_vertices);
        if (forwarding == NULL)
                goto fail_vertices;

        for (i = 0; i < graph->nr_vertices; ++i)
                list_head_init(&forwarding[i]);

        if (dijkstra(graph, s_addr, &nhops, dist))
                goto fail_dijkstra;

        free(nhops);

        src_v = find_vertex_by_addr(graph, s_addr);
        if (src_v == NULL)
                goto fail_src_v;

        list_for_each(p, &src_v->edges) {
                int * tmp_dist;

                e = list_entry(p, struct edge, next);
                if (dijkstra(graph, e->nb->addr, &nhops, &tmp_dist))
                        goto fail_src_v;

                free(nhops);

                list_for_each(h, &graph->vertices) {
                        v = list_entry(h, struct vertex, next);
                        if (tmp_dist[v->index] + 1 == (*dist)[v->index]) {
                                n = malloc(sizeof(*n));
                                if (n == NULL) {
                                        free(tmp_dist);
                                        goto fail_src_v;
                                }
                                n->nhop = e->nb->addr;
                                list_add_tail(&n->next, &forwarding[v->index]);
                        }
                }

                free(tmp_dist);
        }

        list_head_init(table);
        i = 0;
        list_for_each(p, &graph->vertices) {
                v = list_entry(p, struct vertex, next);
                if (v->addr == s_addr) {
                        ++i;
                        continue;
                }

                t = malloc(sizeof(*t));
                if (t == NULL)
                        goto fail_t;

                t->dst = v->addr;

                list_head_init(&t->nhops);
                if (&forwarding[i] != forwarding[i].nxt) {
                        t->nhops.nxt = forwarding[i].nxt;
                        t->nhops.prv = forwarding[i].prv;
                        forwarding[i].prv->nxt = &t->nhops;
                        forwarding[i].nxt->prv = &t->nhops;
                }

                list_add(&t->next, table);
                ++i;
        }

        free(*dist);
        *dist = NULL;
        free(forwarding);

        return 0;

 fail_t:
        free_routing_table(table);
 fail_src_v:
        free(*dist);
 fail_dijkstra:
        free(forwarding);
 fail_vertices:
        *dist = NULL;
        return -1;
}

int graph_routing_table(struct graph *     graph,
                        enum routing_algo  algo,
                        uint64_t           s_addr,
                        struct list_head * table)
{
        int * s_dist;

        assert(graph);
        assert(table);

        pthread_mutex_lock(&graph->lock);

        switch (algo) {
        case ROUTING_SIMPLE:
                /* LFA uses the s_dist this returns. */
                if (graph_routing_table_simple(graph, s_addr, table, &s_dist))
                        goto fail_table;
                break;
        case ROUTING_LFA:
                if (graph_routing_table_lfa(graph, s_addr, table, &s_dist))
                        goto fail_table;
                break;

        case ROUTING_ECMP:
                if (graph_routing_table_ecmp(graph, s_addr, table, &s_dist))
                        goto fail_table;
                break;
        default:
                log_err("Unsupported algorithm.");
                goto fail_algo;
        }

        pthread_mutex_unlock(&graph->lock);

        free(s_dist);

        return 0;

 fail_algo:
        free(s_dist);
 fail_table:
        pthread_mutex_unlock(&graph->lock);
        return -1;
}
