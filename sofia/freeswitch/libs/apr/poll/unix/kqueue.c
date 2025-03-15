/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fspr_arch_poll_private.h"

#ifdef POLLSET_USES_KQUEUE

static fspr_int16_t get_kqueue_revent(fspr_int16_t event, fspr_int16_t flags)
{
    fspr_int16_t rv = 0;

    if (event == EVFILT_READ)
        rv |= APR_POLLIN;
    if (event == EVFILT_WRITE)
        rv |= APR_POLLOUT;
    if (flags & EV_EOF)
        rv |= APR_POLLHUP;
    if (flags & EV_ERROR)
        rv |= APR_POLLERR;

    return rv;
}

struct fspr_pollset_t
{
    fspr_pool_t *pool;
    fspr_uint32_t nelts;
    fspr_uint32_t nalloc;
    int kqueue_fd;
    struct kevent kevent;
    struct kevent *ke_set;
    fspr_pollfd_t *result_set;
    fspr_uint32_t flags;
#if APR_HAS_THREADS
    /* A thread mutex to protect operations on the rings */
    fspr_thread_mutex_t *ring_lock;
#endif
    /* A ring containing all of the pollfd_t that are active */
    APR_RING_HEAD(pfd_query_ring_t, pfd_elem_t) query_ring;
    /* A ring of pollfd_t that have been used, and then _remove'd */
    APR_RING_HEAD(pfd_free_ring_t, pfd_elem_t) free_ring;
    /* A ring of pollfd_t where rings that have been _remove'd but
       might still be inside a _poll */
    APR_RING_HEAD(pfd_dead_ring_t, pfd_elem_t) dead_ring;
};

static fspr_status_t backend_cleanup(void *p_)
{
    fspr_pollset_t *pollset = (fspr_pollset_t *) p_;
    close(pollset->kqueue_fd);
    return APR_SUCCESS;
}

APR_DECLARE(fspr_status_t) fspr_pollset_create(fspr_pollset_t **pollset,
                                             fspr_uint32_t size,
                                             fspr_pool_t *p,
                                             fspr_uint32_t flags)
{
    fspr_status_t rv = APR_SUCCESS;
    *pollset = fspr_palloc(p, sizeof(**pollset));
#if APR_HAS_THREADS
    if (flags & APR_POLLSET_THREADSAFE &&
        ((rv = fspr_thread_mutex_create(&(*pollset)->ring_lock,
                                       APR_THREAD_MUTEX_DEFAULT,
                                       p) != APR_SUCCESS))) {
        *pollset = NULL;
        return rv;
    }
#else
    if (flags & APR_POLLSET_THREADSAFE) {
        *pollset = NULL;
        return APR_ENOTIMPL;
    }
#endif
    (*pollset)->nelts = 0;
    (*pollset)->nalloc = size;
    (*pollset)->flags = flags;
    (*pollset)->pool = p;

    (*pollset)->ke_set =
        (struct kevent *) fspr_palloc(p, size * sizeof(struct kevent));

    memset((*pollset)->ke_set, 0, size * sizeof(struct kevent));

    (*pollset)->kqueue_fd = kqueue();

    if ((*pollset)->kqueue_fd == -1) {
        return APR_ENOMEM;
    }

    fspr_pool_cleanup_register(p, (void *) (*pollset), backend_cleanup,
                              fspr_pool_cleanup_null);

    (*pollset)->result_set = fspr_palloc(p, size * sizeof(fspr_pollfd_t));

    APR_RING_INIT(&(*pollset)->query_ring, pfd_elem_t, link);
    APR_RING_INIT(&(*pollset)->free_ring, pfd_elem_t, link);
    APR_RING_INIT(&(*pollset)->dead_ring, pfd_elem_t, link);

    return rv;
}

APR_DECLARE(fspr_status_t) fspr_pollset_destroy(fspr_pollset_t * pollset)
{
    return fspr_pool_cleanup_run(pollset->pool, pollset, backend_cleanup);
}

APR_DECLARE(fspr_status_t) fspr_pollset_add(fspr_pollset_t *pollset,
                                          const fspr_pollfd_t *descriptor)
{
    fspr_os_sock_t fd;
    pfd_elem_t *elem;
    fspr_status_t rv = APR_SUCCESS;

    pollset_lock_rings();

    if (!APR_RING_EMPTY(&(pollset->free_ring), pfd_elem_t, link)) {
        elem = APR_RING_FIRST(&(pollset->free_ring));
        APR_RING_REMOVE(elem, link);
    }
    else {
        elem = (pfd_elem_t *) fspr_palloc(pollset->pool, sizeof(pfd_elem_t));
        APR_RING_ELEM_INIT(elem, link);
    }
    elem->pfd = *descriptor;

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }

    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&pollset->kevent, fd, EVFILT_READ, EV_ADD, 0, 0, elem);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_ENOMEM;
        }
    }

    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        EV_SET(&pollset->kevent, fd, EVFILT_WRITE, EV_ADD, 0, 0, elem);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_ENOMEM;
        }
    }

    if (rv == APR_SUCCESS) {
        pollset->nelts++;
        APR_RING_INSERT_TAIL(&(pollset->query_ring), elem, pfd_elem_t, link);
    }
    else {
        APR_RING_INSERT_TAIL(&(pollset->free_ring), elem, pfd_elem_t, link);
    }

    pollset_unlock_rings();

    return rv;
}

APR_DECLARE(fspr_status_t) fspr_pollset_remove(fspr_pollset_t *pollset,
                                             const fspr_pollfd_t *descriptor)
{
    pfd_elem_t *ep;
    fspr_status_t rv = APR_SUCCESS;
    fspr_os_sock_t fd;

    pollset_lock_rings();

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }

    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&pollset->kevent, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }

    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        EV_SET(&pollset->kevent, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }

    if (!APR_RING_EMPTY(&(pollset->query_ring), pfd_elem_t, link)) {
        for (ep = APR_RING_FIRST(&(pollset->query_ring));
             ep != APR_RING_SENTINEL(&(pollset->query_ring),
                                     pfd_elem_t, link);
             ep = APR_RING_NEXT(ep, link)) {

            if (descriptor->desc.s == ep->pfd.desc.s) {
                APR_RING_REMOVE(ep, link);
                APR_RING_INSERT_TAIL(&(pollset->dead_ring),
                                     ep, pfd_elem_t, link);
                break;
            }
        }
    }

    pollset_unlock_rings();

    return rv;
}

APR_DECLARE(fspr_status_t) fspr_pollset_poll(fspr_pollset_t *pollset,
                                           fspr_interval_time_t timeout,
                                           fspr_int32_t *num,
                                           const fspr_pollfd_t **descriptors)
{
    int ret, i;
    struct timespec tv, *tvptr;
    fspr_status_t rv = APR_SUCCESS;

    if (timeout < 0) {
        tvptr = NULL;
    }
    else {
        tv.tv_sec = (long) fspr_time_sec(timeout);
        tv.tv_nsec = (long) fspr_time_msec(timeout);
        tvptr = &tv;
    }

    ret = kevent(pollset->kqueue_fd, NULL, 0, pollset->ke_set, pollset->nalloc,
                tvptr);
    (*num) = ret;
    if (ret < 0) {
        rv = fspr_get_netos_error();
    }
    else if (ret == 0) {
        rv = APR_TIMEUP;
    }
    else {
        for (i = 0; i < ret; i++) {
            pollset->result_set[i] =
                (((pfd_elem_t*)(pollset->ke_set[i].udata))->pfd);
            pollset->result_set[i].rtnevents =
                get_kqueue_revent(pollset->ke_set[i].filter,
                              pollset->ke_set[i].flags);
        }

        if (descriptors) {
            *descriptors = pollset->result_set;
        }
    }


    pollset_lock_rings();

    /* Shift all PFDs in the Dead Ring to be Free Ring */
    APR_RING_CONCAT(&(pollset->free_ring), &(pollset->dead_ring), pfd_elem_t, link);

    pollset_unlock_rings();

    return rv;
}

#endif /* POLLSET_USES_KQUEUE */
