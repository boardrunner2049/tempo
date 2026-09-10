/* The delivery queue: values in flight between participants.
 *
 * Ordering is (time, connection_id, enqueue sequence). Connection id comes
 * before the sequence so that the order is a property of the compiled
 * federation, not of the host's execution history (S26).
 */
#include "tempo_internal.h"

void tempo__dq_init(tempo_delivery_queue_t *q, const tempo_route_t *routes)
{
    memset(q, 0, sizeof(*q));
    q->routes = routes;
}

void tempo__dq_clear(tempo_delivery_queue_t *q)
{
    q->n = 0;
    q->arena_len = 0;
}

void tempo__dq_free(tempo_delivery_queue_t *q)
{
    free(q->items);
    free(q->arena);
    memset(q, 0, sizeof(*q));
}

const uint8_t *tempo__dq_payload(const tempo_delivery_queue_t *q,
                                 const tempo_delivery_t *d)
{
    if (!d->len)
        return NULL;
    return q->arena + d->payload;
}

static int dq_cmp(const tempo_delivery_queue_t *q,
                  const tempo_delivery_t *a,
                  const tempo_delivery_t *b)
{
    uint32_t ca, cb;

    if (a->time != b->time)
        return a->time < b->time ? -1 : 1;
    ca = q->routes[a->route].connection_id;
    cb = q->routes[b->route].connection_id;
    if (ca != cb)
        return ca < cb ? -1 : 1;
    if (a->seq != b->seq)
        return a->seq < b->seq ? -1 : 1;
    return 0;
}

int tempo__dq_push(tempo_delivery_queue_t *q, tempo_delivery_t *d,
                   const void *payload, uint32_t payload_len)
{
    uint32_t i, parent;
    int rc;

    rc = TEMPO_RESERVE(&q->items, &q->cap, q->n + 1, tempo_delivery_t);
    if (rc != TEMPO_OK)
        return rc;

    /* Nothing is pending, so no popped entry's payload can still be in use. */
    if (q->n == 0)
        q->arena_len = 0;

    d->payload = 0;
    d->len = 0;
    if (payload_len) {
        uint32_t need = q->arena_len + payload_len;
        if (need > q->arena_cap) {
            uint32_t newcap = q->arena_cap ? q->arena_cap : 1024;
            uint8_t *p;
            while (newcap < need) {
                if (newcap > 0x7FFFFFFFu)
                    return TEMPO_ERR_OVERFLOW;
                newcap *= 2;
            }
            p = (uint8_t *)realloc(q->arena, newcap);
            if (!p)
                return TEMPO_ERR_NO_MEMORY;
            q->arena = p;
            q->arena_cap = newcap;
        }
        memcpy(q->arena + q->arena_len, payload, payload_len);
        d->payload = q->arena_len;
        d->len = payload_len;
        q->arena_len = need;
    }

    i = q->n++;
    q->items[i] = *d;
    while (i > 0) {
        parent = (i - 1) / 2;
        if (dq_cmp(q, &q->items[i], &q->items[parent]) >= 0)
            break;
        {
            tempo_delivery_t tmp = q->items[i];
            q->items[i] = q->items[parent];
            q->items[parent] = tmp;
        }
        i = parent;
    }
    return TEMPO_OK;
}

const tempo_delivery_t *tempo__dq_peek(const tempo_delivery_queue_t *q)
{
    return q->n ? &q->items[0] : NULL;
}

bool tempo__dq_pop(tempo_delivery_queue_t *q, tempo_delivery_t *out)
{
    uint32_t i, left, right, best;

    if (!q->n)
        return false;
    if (out)
        *out = q->items[0];
    q->n--;
    if (!q->n)
        return true;
    q->items[0] = q->items[q->n];

    i = 0;
    for (;;) {
        left = 2 * i + 1;
        right = left + 1;
        best = i;
        if (left < q->n && dq_cmp(q, &q->items[left], &q->items[best]) < 0)
            best = left;
        if (right < q->n && dq_cmp(q, &q->items[right], &q->items[best]) < 0)
            best = right;
        if (best == i)
            break;
        {
            tempo_delivery_t tmp = q->items[i];
            q->items[i] = q->items[best];
            q->items[best] = tmp;
        }
        i = best;
    }
    return true;
}
