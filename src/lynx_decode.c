/*
 * Receiving bytes: link bytes -> complete, CRC-checked frames.
 *
 * Every byte is handled as it arrives, in constant time: the COBS decoder
 * writes the decoded byte into h->rx_buf and the CRC is updated with it at
 * once. When the 0x00 delimiter arrives the frame is already checked:
 * running the CRC over data and CRC together leaves 0 for an intact frame.
 * Valid frames go to lx__dispatch (lynx_dispatch.c).
 *
 * With LX_ISR_SAFE, lx_rx() only stores the byte in a lock-free queue and
 * lx_poll() runs the decoder later, in the application's task.
 *
 * MIT License (c) 2026 Ashwath
 */
#include "lynx_internal.h"

/* ------------------------------------------------------------------ */
/* COBS + CRC decoder                                                  */
/* ------------------------------------------------------------------ */

/* Store one decoded byte. A frame longer than any valid one is dropped. */
static inline void rx_store(lx_t *h, uint8_t b)
{
    if (h->rx_len >= LX_RAW_MAX) { h->rx_drop = 1; return; }
    h->rx_buf[h->rx_len++] = b;
    h->rx_crc = lx__crc_step(h->rx_crc, b);
}

static inline void rx_reset(lx_t *h)
{
    h->rx_len = 0;
    h->rx_code = 0;
    h->rx_rem = 0;
    h->rx_drop = 0;
    h->rx_crc = 0xFFFFu;
}

/* The 0x00 delimiter arrived: pass the frame on if it is intact. */
static inline void rx_end_of_frame(lx_t *h)
{
    if (h->rx_code) {                   /* anything since the last delimiter? */
        bool intact = !h->rx_drop       /* no overflow, no bytes lost          */
                   && h->rx_rem == 0;   /* last COBS block is complete         */
        bool long_enough = h->rx_len >= HEAD_LEN + CRC_LEN;
        if (intact && long_enough && h->rx_crc == 0)
            lx__dispatch(h, h->rx_buf, (uint8_t)(h->rx_len - CRC_LEN));
        else
            h->st_rx_bad++;
    }
    rx_reset(h);
}

/* One byte from the link. See lynx_encode.c for how COBS blocks work. */
static inline void rx_byte(lx_t *h, uint8_t b)
{
    if (b == 0x00u) {
        rx_end_of_frame(h);
        return;
    }
    if (h->rx_drop) return;             /* skip to the next delimiter */

    if (h->rx_rem == 0) {
        /* b is the code byte of a new block. The previous block stood for
         * its data plus one 0x00, unless it was a full block (code 0xFF). */
        if (h->rx_code && h->rx_code != 0xFFu) rx_store(h, 0x00u);
        h->rx_code = b;
        h->rx_rem = (uint8_t)(b - 1u);
    } else {
        rx_store(h, b);
        h->rx_rem--;
    }
}

#if !LX_ISR_SAFE

void lx_rx(lx_t *h, uint8_t b)
{
    rx_byte(h, b);
}

#else /* LX_ISR_SAFE */

/* ------------------------------------------------------------------ */
/* LX_ISR_SAFE: lock-free byte queue between lx_rx and lx_poll         */
/* ------------------------------------------------------------------ */

/* One producer (lx_rx, typically the UART ISR) and one consumer (lx_poll,
 * the application's task), so no lock is needed:
 *   - lx_rx owns rxq_head, rxq_gap, rxq_lost and st_rx_ovf;
 *   - lx_poll owns rxq_tail.
 * Acquire/release on the indices makes the byte visible before the index
 * that publishes it, also across cores.
 *
 * When the queue is full, lx_rx drops the byte and flags the slot of the
 * next byte it does store ("bytes are missing before this one"). lx_poll
 * then discards the frame that byte belongs to, so exactly the frames that
 * lost bytes are thrown away, however many gaps there are. */
#if defined(__GNUC__)
#define LOAD_RLX(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE_RLX(p, v)  __atomic_store_n((p), (uint8_t)(v), __ATOMIC_RELAXED)
#else
#define LOAD_RLX(p)      (*(volatile uint8_t *)(p))
#define STORE_RLX(p, v)  (*(volatile uint8_t *)(p) = (uint8_t)(v))
#endif
#define RXQ_NEXT(i)  ((uint8_t)(((i) + 1u) & (LX_RXQ_SIZE - 1u)))
#define GAP_BIT(i)   ((uint8_t)(1u << ((i) & 7u)))

void lx_rx(lx_t *h, uint8_t b)
{
    uint8_t head = h->rxq_head;
    uint8_t next = RXQ_NEXT(head);
    if (next == LX_LOAD8(&h->rxq_tail)) {           /* full: drop the byte */
        h->st_rx_ovf++;
        h->rxq_lost = 1;
        return;
    }
    /* Set or clear this slot's gap flag. Only lx_rx writes the bitmap, so a
     * plain read-modify-write is enough; relaxed atomics keep TSan quiet. */
    uint8_t *gap = &h->rxq_gap[head >> 3];
    uint8_t flags = LOAD_RLX(gap);
    STORE_RLX(gap, h->rxq_lost ? (flags | GAP_BIT(head)) : (flags & (uint8_t)~GAP_BIT(head)));
    h->rxq_lost = 0;

    h->rxq[head] = b;
    LX_STORE8(&h->rxq_head, next);                  /* publish the byte */
}

size_t lx_poll(lx_t *h)
{
    uint8_t tail = h->rxq_tail;
    uint8_t head = LX_LOAD8(&h->rxq_head);
    size_t n = 0;
    while (tail != head) {
        if (LOAD_RLX(&h->rxq_gap[tail >> 3]) & GAP_BIT(tail))
            h->rx_drop = 1;             /* bytes are missing right before this one */
        uint8_t b = h->rxq[tail];
        tail = RXQ_NEXT(tail);
        LX_STORE8(&h->rxq_tail, tail);  /* free the slot */
        rx_byte(h, b);
        n++;
    }
    return n;
}

#endif /* LX_ISR_SAFE */

void lx_rx_buf(lx_t *h, const uint8_t *data, size_t len)
{
    while (len--) lx_rx(h, *data++);
}
