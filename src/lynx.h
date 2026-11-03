/*
 * LYNX - zero-allocation variable sync between microcontrollers.
 * C99, no heap, O(1) work per received byte, works over any byte link.
 *
 * Both nodes bind their variables to the same small ids. After that, a call
 * like lx_write(&h, LX_BIT(3)) sends variable 3 to the other node, which
 * copies it into its own variable 3 and answers with an ACK.
 *
 *   lx_init(&h, my_uart_send, my_event_handler, NULL);
 *   lx_bind(&h, 3, &setpoint, sizeof setpoint);
 *   for (;;) {
 *       while (uart_has_byte()) lx_rx(&h, uart_read_byte());
 *       lx_tick(&h, millis());               // timeouts and retries
 *       ...lx_publish / lx_write / lx_read...
 *   }
 *
 * Frame on the wire: COBS([HDR][ID][PAYLOAD][CRC16]) followed by 0x00,
 * where HDR = (CMD << 4) | SEQ. Bus mode (LX_BUS=1) adds [DST][SRC] after
 * HDR. The full format is in README.md; how the code is organised, and how
 * a frame travels through it, is in docs/architecture.md.
 *
 * MIT License (c) 2026 Ashwath
 */
#ifndef LYNX_H
#define LYNX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "lynx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* Mask with one variable id set. Masks select the variables to send:
 * lx_write(&h, LX_BIT(0) | LX_BIT(2)) sends variables 0 and 2 together. */
#define LX_BIT(id)  ((uint32_t)1u << (id))

/* Return codes of the API calls */
enum {
    LX_OK        =  0,
    LX_ERR_BUSY  = -1,  /* a reliable write or read is already in flight */
    LX_ERR_ID    = -2,  /* bad id, or an unbound variable in the mask */
    LX_ERR_SIZE  = -3,  /* variable or batch too large for LX_MAX_PAYLOAD */
    LX_ERR_ADDR  = -4,  /* LX_BUS: bad address (reliable op to broadcast, to self) */
};

/* Results of lx_write / lx_read, passed to the event callback.
 * Every write or read ends with exactly one of these. */
enum {
    LX_EV_DONE      = 1,  /* write ACKed, or read response committed */
    LX_EV_TIMEOUT   = 2,  /* no answer after all retries */
    LX_EV_NACK_ID   = 3,  /* remote has no such variable */
    LX_EV_NACK_SIZE = 4,  /* size mismatch between nodes */
};

/* Largest frame before COBS: HDR [DST SRC] ID PAYLOAD CRC_HI CRC_LO */
#define LX_RAW_MAX  (4u + 2u * LX_BUS + LX_MAX_PAYLOAD)
/* Largest frame on the wire: COBS adds up to 1 byte per 254, plus the
 * trailing 0x00 delimiter (and the leading one with LX_LEAD_DELIM) */
#define LX_ENC_MAX  (LX_RAW_MAX + 3u + LX_LEAD_DELIM)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* Send `len` bytes on the link. Each call is one complete frame. */
typedef void (*lx_tx_fn)(void *ctx, const uint8_t *data, uint16_t len);
/* A write or read finished: `event` is an LX_EV_* code, `mask` the
 * variables the transaction was about. */
typedef void (*lx_event_fn)(void *ctx, uint8_t event, uint32_t mask);

typedef struct {
    void   *ptr;
    uint8_t size;
} lx_var_t;

/* One LYNX endpoint. Allocate it statically (it holds all buffers) and
 * treat it as opaque, except for the link statistics at the end. */
typedef struct {
    /* Bound variables */
    lx_var_t          vars[LX_MAX_VARS];
    uint32_t          bound;        /* bit i set: id i is bound                 */
    volatile uint32_t updated;      /* bit i set: the peer changed variable i   */

    /* Callbacks given to lx_init */
    lx_tx_fn    tx;
    lx_event_fn ev;
    void       *ctx;

    /* Receive decoder (lynx_decode.c). COBS decoding and the CRC both run
     * as each byte arrives, so a frame is never scanned twice. */
    uint8_t  rx_buf[LX_RAW_MAX];    /* decoded frame so far                   */
    uint16_t rx_crc;                /* CRC over rx_buf; 0 at the end = valid  */
    uint8_t  rx_len;                /* bytes in rx_buf                        */
    uint8_t  rx_code;               /* code byte of the current COBS block    */
    uint8_t  rx_rem;                /* data bytes left in that block          */
    uint8_t  rx_drop;               /* frame is lost: ignore bytes until 0x00 */

    /* Encoded frames ready to send. Replies from the RX path use their own
     * buffer, so they never overwrite a frame the app is sending. */
    uint8_t  tx_buf[LX_ENC_MAX];    /* app path: publish, write, read          */
    uint8_t  rsp_buf[LX_ENC_MAX];   /* RX path: ACK, NACK, read response       */

    /* The reliable transaction in flight, at most one (lynx_txn.c).
     * "p_" = pending. */
    volatile uint32_t p_mask;       /* variables it is about                   */
    volatile uint8_t  p_cmd;        /* state: 0 idle, else see lynx_internal.h */
    uint8_t  p_seq;                 /* its sequence number, echoed by answers  */
    uint8_t  p_tries;               /* retransmissions so far                  */
    uint8_t  seq;                   /* last sequence number used               */
    uint32_t now;                   /* clock from the last lx_tick, in ms      */
    uint32_t deadline;              /* retransmit or give up at this time      */

#if LX_BUS
    uint8_t  addr;          /* this node                                  */
    uint8_t  peer;          /* destination of lx_publish/lx_write/lx_read */
    uint8_t  p_dst;         /* node the transaction in flight talks to    */
#endif

#if LX_ISR_SAFE
    /* lx_rx -> lx_poll byte queue: one producer, one consumer, no lock */
    uint8_t  rxq[LX_RXQ_SIZE];
    uint8_t  rxq_gap[(LX_RXQ_SIZE + 7u) / 8u];  /* bit i: bytes lost before rxq[i] */
    uint8_t  rxq_head, rxq_tail;    /* written only by lx_rx / lx_poll        */
    uint8_t  rxq_lost;              /* lx_rx only: dropped since last enqueue */
#endif

    /* Link statistics (read-only for the application). One writer each:
     * the RX path owns rx_ok, rx_bad and rsp; the app path owns tx and retx. */
    uint32_t st_rx_ok;      /* valid frames received                      */
    uint32_t st_rx_bad;     /* corrupt frames, or unknown commands        */
    uint32_t st_tx;         /* frames sent by publish / write / read      */
    uint32_t st_retx;       /* retransmissions                            */
    uint32_t st_rsp;        /* replies sent (ACK, NACK, read response)    */
#if LX_ISR_SAFE
    uint32_t st_rx_ovf;     /* bytes lx_rx dropped: the queue was full    */
#endif
} lx_t;

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

/* Reset the handle. `ev` may be NULL. `ctx` is passed to both callbacks. */
void lx_init(lx_t *h, lx_tx_fn tx, lx_event_fn ev, void *ctx);

/* Bind variable `id` (0..LX_MAX_VARS-1) to `size` bytes at `ptr`. Both nodes
 * must use the same id, size and memory layout (endianness, padding). */
int  lx_bind(lx_t *h, uint8_t id, void *ptr, uint8_t size);

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* Fire-and-forget: send the variables in `mask` in one frame, no ACK. */
int  lx_publish(lx_t *h, uint32_t mask);

/* Reliable write: one frame, committed all-or-nothing on the remote, then
 * ACK; retransmitted automatically. Ends with one event. */
int  lx_write(lx_t *h, uint32_t mask);

/* Ask the remote for its value of `id`; committed locally when the answer
 * arrives. Ends with one event. */
int  lx_read(lx_t *h, uint8_t id);

/* A write or read is in flight. Stays true until its event callback has
 * returned. Only one can be in flight; publishes can be sent any time. */
static inline bool lx_busy(const lx_t *h) { return LX_LOAD8(&h->p_cmd) != 0; }

/* Drive timeouts and retries. Call often with a millisecond clock. */
void lx_tick(lx_t *h, uint32_t now_ms);

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* Feed bytes received from the link.
 * Default build: decodes in place; replies (tx) and events (ev) happen right
 * here. From an ISR, both callbacks must be ISR-safe and non-blocking.
 * LX_ISR_SAFE: only queues the bytes (lock-free, no callbacks); a full queue
 * drops bytes (st_rx_ovf) and the frame they belonged to. */
void lx_rx(lx_t *h, uint8_t byte);
void lx_rx_buf(lx_t *h, const uint8_t *data, size_t len);

#if LX_ISR_SAFE
/* Decode and dispatch everything queued so far. Returns bytes processed. */
size_t lx_poll(lx_t *h);
#else
static inline size_t lx_poll(lx_t *h) { (void)h; return 0; }  /* nothing is queued */
#endif

/* Fetch and clear the mask of variables the peer has changed. */
uint32_t lx_take_updates(lx_t *h);

/* Copy a bound variable out / in under the critical section, so a commit
 * from lx_rx in another task or ISR can't tear it. Single-threaded code may
 * just use the variable directly. */
int  lx_get(lx_t *h, uint8_t id, void *out);
int  lx_set(lx_t *h, uint8_t id, const void *in);

/* ------------------------------------------------------------------ */
/* Concurrency                                                         */
/* ------------------------------------------------------------------ */

/* Two contexts may use a handle at the same time:
 *  - the RX path: lx_rx / lx_rx_buf (default build) or lx_poll (LX_ISR_SAFE);
 *  - the app path: every other lx_* call, all from ONE task.
 * LX_ENTER/EXIT_CRITICAL guard what they share: the transaction in flight,
 * the update flags and the bound variables (use lx_get / lx_set for a
 * variable the other context writes or sends). Each transaction ends with
 * exactly one event: DONE or NACK from the RX path, TIMEOUT from lx_tick,
 * and lx_busy() stays true until that callback has returned.
 * tx() is called from both paths and must keep each call's bytes together on
 * the wire. Without critical-section hooks (one context) the callback may
 * start the next transaction; with hooks, start it after the callback.
 * With LX_ISR_SAFE and lx_poll() called from the app task, everything but
 * the byte queue runs in that task: no hooks needed. */

/* ------------------------------------------------------------------ */
/* Multi-drop bus (LX_BUS=1)                                           */
/* ------------------------------------------------------------------ */

#if LX_BUS
/* Every node needs a unique address 0x00..0xFE.
 * Nodes act only on frames sent to their address or to LX_ADDR_BCAST, and
 * drop frames carrying their own address as source (bus echo).
 * Broadcasts are never answered: a broadcast WRITE commits like a publish,
 * a broadcast READ is ignored. Only the node a transaction was sent to can
 * complete it (h->p_dst is still valid inside the event callback). */
#define LX_ADDR_BCAST 0xFFu

/* Set this node's address and the default destination (`peer`) used by
 * lx_publish/lx_write/lx_read. After lx_init: self = 0, peer = broadcast. */
int  lx_set_addr(lx_t *h, uint8_t self, uint8_t peer);
int  lx_publish_to(lx_t *h, uint8_t dst, uint32_t mask);  /* dst may be broadcast */
int  lx_write_to(lx_t *h, uint8_t dst, uint32_t mask);    /* unicast only */
int  lx_read_from(lx_t *h, uint8_t src, uint8_t id);      /* unicast only */
#endif

#ifdef __cplusplus
}
#endif
#endif /* LYNX_H */
