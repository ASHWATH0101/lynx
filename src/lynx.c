/*
 * LYNX public API: setup, sending, and access to bound variables.
 * Start reading here. The other files each do one job:
 *
 *   lynx_encode.c    variables -> encoded frames (COBS + CRC in one pass)
 *   lynx_decode.c    received bytes -> checked frames (and the ISR queue)
 *   lynx_dispatch.c  checked frame -> commit, reply, or answer to our request
 *   lynx_txn.c       the reliable write/read in flight: retries, timeout, event
 *
 * docs/architecture.md walks through them with the path of a frame.
 *
 * MIT License (c) 2026 Ashwath
 */
#include "lynx_internal.h"

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void lx_init(lx_t *h, lx_tx_fn tx, lx_event_fn ev, void *ctx)
{
    memset(h, 0, sizeof *h);
    h->tx = tx;
    h->ev = ev;
    h->ctx = ctx;
    h->rx_crc = 0xFFFFu;
#if LX_BUS
    h->peer = LX_ADDR_BCAST;
#endif
}

int lx_bind(lx_t *h, uint8_t id, void *ptr, uint8_t size)
{
    if (id >= LX_MAX_VARS || !ptr || !size) return LX_ERR_ID;
    if (size > LX_MAX_PAYLOAD) return LX_ERR_SIZE;
    h->vars[id].ptr = ptr;
    h->vars[id].size = size;
    h->bound |= LX_BIT(id);
    return LX_OK;
}

/* ------------------------------------------------------------------ */
/* Bound variables                                                     */
/* ------------------------------------------------------------------ */

uint32_t lx_take_updates(lx_t *h)
{
    LX_ENTER_CRITICAL();
    uint32_t m = h->updated;
    h->updated = 0;
    LX_EXIT_CRITICAL();
    return m;
}

int lx_get(lx_t *h, uint8_t id, void *out)
{
    if (!lx__is_bound(h, id)) return LX_ERR_ID;
    LX_ENTER_CRITICAL();
    memcpy(out, h->vars[id].ptr, h->vars[id].size);
    LX_EXIT_CRITICAL();
    return LX_OK;
}

int lx_set(lx_t *h, uint8_t id, const void *in)
{
    if (!lx__is_bound(h, id)) return LX_ERR_ID;
    LX_ENTER_CRITICAL();
    memcpy(h->vars[id].ptr, in, h->vars[id].size);
    LX_EXIT_CRITICAL();
    return LX_OK;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* A publish is one frame and nothing else: no SEQ, no state, no answer. */
static int publish(lx_t *h, uint32_t mask BUS_ADDR_PARAM(dst))
{
    int err = lx__check_vars(h, mask);
    if (err) return err;
    uint16_t len = lx__build_vars(h, h->tx_buf, CMD_PUB, 0, mask BUS_ADDR_ARG(dst));
    lx__send(h, &h->st_tx, h->tx_buf, len);
    return LX_OK;
}

/* Writes and reads become the transaction in flight (lynx_txn.c). */
static int write_vars(lx_t *h, uint32_t mask BUS_ADDR_PARAM(dst))
{
    int err = lx__check_vars(h, mask);
    if (err) return err;
    return lx__txn_begin(h, CMD_WRITE, mask BUS_ADDR_ARG(dst));
}

static int read_var(lx_t *h, uint8_t id BUS_ADDR_PARAM(dst))
{
    if (!lx__is_bound(h, id)) return LX_ERR_ID;
    return lx__txn_begin(h, CMD_READ, LX_BIT(id) BUS_ADDR_ARG(dst));
}

#if LX_BUS

int lx_set_addr(lx_t *h, uint8_t self, uint8_t peer)
{
    if (self == LX_ADDR_BCAST || self == peer) return LX_ERR_ADDR;
    h->addr = self;
    h->peer = peer;
    return LX_OK;
}

int lx_publish_to(lx_t *h, uint8_t dst, uint32_t mask)
{
    if (dst == h->addr) return LX_ERR_ADDR;
    return publish(h, mask, dst);
}

/* Writes and reads need exactly one node to answer: no broadcast, no self. */
int lx_write_to(lx_t *h, uint8_t dst, uint32_t mask)
{
    if (dst == h->addr || dst == LX_ADDR_BCAST) return LX_ERR_ADDR;
    return write_vars(h, mask, dst);
}

int lx_read_from(lx_t *h, uint8_t src, uint8_t id)
{
    if (src == h->addr || src == LX_ADDR_BCAST) return LX_ERR_ADDR;
    return read_var(h, id, src);
}

int lx_publish(lx_t *h, uint32_t mask) { return lx_publish_to(h, h->peer, mask); }
int lx_write(lx_t *h, uint32_t mask)   { return lx_write_to(h, h->peer, mask); }
int lx_read(lx_t *h, uint8_t id)       { return lx_read_from(h, h->peer, id); }

#else

int lx_publish(lx_t *h, uint32_t mask) { return publish(h, mask); }
int lx_write(lx_t *h, uint32_t mask)   { return write_vars(h, mask); }
int lx_read(lx_t *h, uint8_t id)       { return read_var(h, id); }

#endif
