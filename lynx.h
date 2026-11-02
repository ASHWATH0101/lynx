/*
 * LYNX - zero-allocation variable-sync protocol for MCU-to-MCU links.
 * C99, no heap, O(1) work per received byte, transport-agnostic.
 *
 * Wire format (before COBS):  [HDR][ID][PAYLOAD 0..N][CRC_HI][CRC_LO]
 *   HDR = (CMD << 4) | SEQ      CRC-16/CCITT-FALSE over HDR..PAYLOAD
 * LX_BUS=1 inserts [DST][SRC] after HDR (DST 0xFF = broadcast).
 * On the wire: COBS(frame) + 0x00 delimiter.
 *
 * MIT License (c) 2026 Ashwath
 */
#ifndef LYNX_H
#define LYNX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time configuration (override with -D or before #include)    */
/* ------------------------------------------------------------------ */
/* Platform ports (port/<platform>/lynx_port.h) are pulled in by the
 * ESP-IDF / Zephyr build integration via -DLX_PORT; they map Kconfig to the
 * options below and set the critical-section hooks. */
#ifdef LX_PORT
#include "lynx_port.h"
#endif

#ifndef LX_MAX_VARS
#define LX_MAX_VARS     32u     /* <= 32: all flags live in one uint32_t */
#endif
#ifndef LX_MAX_PAYLOAD
#define LX_MAX_PAYLOAD  64u     /* <= 250 */
#endif
#ifndef LX_TIMEOUT_MS
#define LX_TIMEOUT_MS   20u     /* ACK / READ_RESP timeout */
#endif
#ifndef LX_LEAD_DELIM
#define LX_LEAD_DELIM   0       /* 1 = also send 0x00 before each frame:
                                   +1 byte/frame, but line noise can never
                                   eat the next frame. 0 = lose at most one. */
#endif
#ifndef LX_RETRIES
#define LX_RETRIES      3u      /* automatic retransmissions */
#endif
#ifndef LX_BUS
#define LX_BUS          0       /* 1 = multi-drop bus: +2 address bytes/frame,
                                   node addresses, broadcast. 0 = point-to-point,
                                   compiles to exactly the same code as before. */
#endif
#ifndef LX_ISR_SAFE
#define LX_ISR_SAFE     0       /* 1 = lx_rx() only queues the byte (lock-free,
                                   safe in any ISR); lx_poll() decodes, replies
                                   and fires events in the caller's context. */
#endif
#ifndef LX_RXQ_SIZE
#define LX_RXQ_SIZE     128u    /* LX_ISR_SAFE queue: power of two, 2..256,
                                   holds LX_RXQ_SIZE - 1 bytes between polls */
#endif

/* Critical section hooks. Map to your platform, e.g.
 *   ESP-IDF : portENTER_CRITICAL(&lx_mux) / portEXIT_CRITICAL(&lx_mux)
 *   Zephyr  : k_spinlock_key_t k = k_spin_lock(&l) / k_spin_unlock(&l, k)
 *   Cortex-M: uint32_t pm = __get_PRIMASK(); __disable_irq() / __set_PRIMASK(pm)
 * LX_ENTER_CRITICAL() may declare a local: LYNX always pairs it with one or
 * more LX_EXIT_CRITICAL() in the same or a nested block, and never nests.
 * Default is no-op (fine when everything runs in one thread). */
#ifndef LX_ENTER_CRITICAL
#define LX_ENTER_CRITICAL() ((void)0)
#define LX_EXIT_CRITICAL()  ((void)0)
#define LX_ONE_CONTEXT  1       /* no hooks: the app promises a single context */
#endif
#ifndef LX_ONE_CONTEXT
#define LX_ONE_CONTEXT  0
#endif

#if LX_MAX_VARS > 32u
#error "LX_MAX_VARS must be <= 32"
#endif
#if LX_MAX_PAYLOAD > 250u
#error "LX_MAX_PAYLOAD must be <= 250"
#endif
#if LX_BUS && LX_MAX_PAYLOAD > 248u
#error "LX_MAX_PAYLOAD must be <= 248 with LX_BUS"
#endif
#if LX_ISR_SAFE && (LX_RXQ_SIZE < 2u || LX_RXQ_SIZE > 256u || (LX_RXQ_SIZE & (LX_RXQ_SIZE - 1u)))
#error "LX_RXQ_SIZE must be a power of two in 2..256"
#endif

/* Byte hand-over between contexts: p_cmd (read without the lock by lx_busy
 * and lx_tick) and the LX_ISR_SAFE queue indices. Acquire/release, so that
 * once lx_busy() reads false everything the event callback did is visible.
 * Override for compilers without the GNU __atomic builtins. */
#ifndef LX_LOAD8
#if defined(__GNUC__)
#define LX_LOAD8(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LX_STORE8(p, v)  __atomic_store_n((p), (uint8_t)(v), __ATOMIC_RELEASE)
#else
#define LX_LOAD8(p)      (*(p))
#define LX_STORE8(p, v)  (*(p) = (uint8_t)(v))
#endif
#endif

#define LX_RAW_MAX  (4u + 2u * LX_BUS + LX_MAX_PAYLOAD) /* hdr [+dst+src] + id + payload + crc16 */
#define LX_ENC_MAX  (LX_RAW_MAX + 3u + LX_LEAD_DELIM) /* COBS worst case + delimiter(s) */
#define LX_BIT(id)  ((uint32_t)1u << (id))

/* Return codes */
enum {
    LX_OK        =  0,
    LX_ERR_BUSY  = -1,  /* a reliable transaction is already in flight */
    LX_ERR_ID    = -2,  /* bad id / unbound variable in mask */
    LX_ERR_SIZE  = -3,  /* variable or batch too large */
    LX_ERR_ADDR  = -4,  /* LX_BUS: bad address (reliable op to broadcast, to self) */
};

/* Async events delivered to the event callback */
enum {
    LX_EV_DONE      = 1,  /* write ACKed, or read response committed */
    LX_EV_TIMEOUT   = 2,  /* no answer after all retries */
    LX_EV_NACK_ID   = 3,  /* remote has no such variable */
    LX_EV_NACK_SIZE = 4,  /* size mismatch between nodes */
};

typedef void (*lx_tx_fn)(void *ctx, const uint8_t *data, uint16_t len);
typedef void (*lx_event_fn)(void *ctx, uint8_t event, uint32_t mask);

typedef struct {
    void   *ptr;
    uint8_t size;
} lx_var_t;

typedef struct {
    lx_var_t          vars[LX_MAX_VARS];
    uint32_t          bound;        /* bitmask of bound ids         */
    volatile uint32_t updated;      /* bitmask of ids written remotely */

    lx_tx_fn    tx;
    lx_event_fn ev;
    void       *ctx;

    /* RX decoder (COBS + CRC run byte-by-byte, no second pass) */
    uint8_t  rx_buf[LX_RAW_MAX];
    uint16_t rx_crc;
    uint8_t  rx_len, rx_code, rx_rem, rx_drop;

    /* Separate buffers so RX-context replies never clobber app TX */
    uint8_t  tx_buf[LX_ENC_MAX];
    uint8_t  rsp_buf[LX_ENC_MAX];

    /* One reliable transaction in flight */
    volatile uint32_t p_mask;
    volatile uint8_t  p_cmd;        /* 0 = idle */
    uint8_t  p_seq, p_tries, seq;
    uint32_t now, deadline;

#if LX_BUS
    uint8_t  addr;          /* this node                                 */
    uint8_t  peer;          /* destination of lx_publish/lx_write/lx_read */
    uint8_t  p_dst;         /* node the transaction in flight talks to   */
#endif

#if LX_ISR_SAFE
    /* lx_rx -> lx_poll byte queue: one producer, one consumer, no lock */
    uint8_t  rxq[LX_RXQ_SIZE];
    uint8_t  rxq_gap[(LX_RXQ_SIZE + 7u) / 8u];  /* bit i: bytes lost before rxq[i] */
    uint8_t  rxq_head, rxq_tail;    /* written only by lx_rx / lx_poll       */
    uint8_t  rxq_lost;              /* lx_rx only: dropped since last enqueue */
#endif

    /* Link statistics. One writer each: the RX path owns rx_ok, rx_bad and
     * rsp (replies it sent), the app path owns tx (frames it sent) and retx. */
    uint32_t st_rx_ok, st_rx_bad, st_tx, st_retx, st_rsp;
#if LX_ISR_SAFE
    uint32_t st_rx_ovf;             /* bytes lx_rx dropped: queue was full   */
#endif
} lx_t;

void lx_init(lx_t *h, lx_tx_fn tx, lx_event_fn ev, void *ctx);
int  lx_bind(lx_t *h, uint8_t id, void *ptr, uint8_t size);

/* Concurrency. Two contexts may use a handle at the same time:
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

/* Feed received bytes.
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

/* Drive timeouts/retries. Call often with a millisecond clock. */
void lx_tick(lx_t *h, uint32_t now_ms);

/* Fire-and-forget: send any set of vars in ONE frame, no ACK. */
int  lx_publish(lx_t *h, uint32_t mask);
/* Reliable: one frame, atomic commit on the remote, ACK + auto-retry. */
int  lx_write(lx_t *h, uint32_t mask);
/* Ask the remote for its value of `id`; committed locally on reply. */
int  lx_read(lx_t *h, uint8_t id);

/* Atomically fetch-and-clear the "remotely updated" bitmask. */
uint32_t lx_take_updates(lx_t *h);

/* Copy a bound variable out / in under the critical section, so a commit
 * from lx_rx in another task or ISR can't tear it. Single-threaded code may
 * just use the variable directly. */
int  lx_get(lx_t *h, uint8_t id, void *out);
int  lx_set(lx_t *h, uint8_t id, const void *in);

#if LX_BUS
/* Multi-drop bus. Every node needs a unique address 0x00..0xFE.
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

static inline bool lx_busy(const lx_t *h) { return LX_LOAD8(&h->p_cmd) != 0; }

#ifdef __cplusplus
}
#endif
#endif /* LYNX_H */
