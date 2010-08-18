#ifndef _URCU_WFQUEUE_H
#define _URCU_WFQUEUE_H

/*
 * wfqueue.h
 *
 * Userspace RCU library - Queue with Wait-Free Enqueue/Blocking Dequeue
 *
 * Copyright 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <assert.h>
#include <urcu/compiler.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (!defined(_GNU_SOURCE) && !defined(_LGPL_SOURCE))
#error "Dynamic loader LGPL wrappers not implemented yet"
#endif

#define WFQ_ADAPT_ATTEMPTS		10	/* Retry if being set */
#define WFQ_WAIT			10	/* Wait 10 ms if being set */

/*
 * Queue with wait-free enqueue/blocking dequeue.
 * This implementation adds a dummy head node when the queue is empty to ensure
 * we can always update the queue locklessly.
 *
 * Inspired from half-wait-free/half-blocking queue implementation done by
 * Paul E. McKenney.
 */

struct wfq_node {
	struct wfq_node *next;
};

struct wfq_queue {
	struct wfq_node *head, **tail;
	struct wfq_node dummy;	/* Dummy node */
	pthread_mutex_t lock;
};

void wfq_node_init(struct wfq_node *node)
{
	node->next = NULL;
}

void wfq_init(struct wfq_queue *q)
{
	int ret;

	wfq_node_init(&q->dummy);
	/* Set queue head and tail */
	q->head = &q->dummy;
	q->tail = &q->dummy.next;
	ret = pthread_mutex_init(&q->lock, NULL);
	assert(!ret);
}

void wfq_enqueue(struct wfq_queue *q, struct wfq_node *node)
{
	struct wfq_node **old_tail;

	/*
	 * uatomic_xchg() implicit memory barrier orders earlier stores to data
	 * structure containing node and setting node->next to NULL before
	 * publication.
	 */
	old_tail = uatomic_xchg(&q->tail, node);
	/*
	 * At this point, dequeuers see a NULL old_tail->next, which indicates
	 * that the queue is being appended to. The following store will append
	 * "node" to the queue from a dequeuer perspective.
	 */
	STORE_SHARED(*old_tail, node);
}

/*
 * It is valid to reuse and free a dequeued node immediately.
 *
 * No need to go on a waitqueue here, as there is no possible state in which the
 * list could cause dequeue to busy-loop needlessly while waiting for another
 * thread to be scheduled. The queue appears empty until tail->next is set by
 * enqueue.
 */
struct wfq_node *
__wfq_dequeue_blocking(struct wfq_queue *q)
{
	struct wfq_node *node, *next;
	int attempt = 0;

	/*
	 * Queue is empty if it only contains the dummy node.
	 */
	if (q->head == &q->dummy && LOAD_SHARED(q->tail) == &q->dummy.next)
		return NULL;
	node = q->head;

	/*
	 * Adaptative busy-looping waiting for enqueuer to complete enqueue.
	 */
	while ((next = LOAD_SHARED(node->next)) == NULL) {
		if (++attempt >= WFQ_ADAPT_ATTEMPTS) {
			poll(NULL, 0, WFQ_WAIT);	/* Wait for 10ms */
			attempt = 0;
		} else
			cpu_relax();
	}
	/*
	 * Move queue head forward.
	 */
	q->head = next;
	/*
	 * Requeue dummy node if we just dequeued it.
	 */
	if (node == &q->dummy) {
		wfq_node_init(node);
		wfq_enqueue(q, node);
		return __wfq_dequeue_blocking(q);
	}
	return node;
}

struct wfq_node *
wfq_dequeue_blocking(struct wfq_queue *q)
{
	struct wfq_node *retnode;
	int ret;

	ret = pthread_mutex_lock(&q->lock);
	assert(!ret);
	retnode = __wfq_dequeue_blocking(q);
	ret = pthread_mutex_unlock(&q->lock);
	assert(!ret);
	return retnode;
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_WFQUEUE_H */
