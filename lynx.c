/* LYNX protocol implementation. MIT License (c) 2026 Ashwath */
#include "lynx.h"
#include <string.h>

enum {
    CMD_WRITE   = 1, CMD_WRITE_M = 2,   /* reliable, single / batch */
    CMD_PUB     = 3, CMD_PUB_M   = 4,   /* no ACK,   single / batch */
    CMD_READ    = 5, CMD_READ_RSP = 6,
    CMD_ACK     = 7, CMD_NACK    = 8,
};
#define BATCH_ID 0xFFu
#define P_CLOSING 0x0Fu    /* p_cmd: answered, event being delivered (still busy) */
#define HEAD_LEN (2u + 2u * LX_BUS)     /* HDR [DST SRC] ID */

/* LX_BUS threads a destination address through the frame builders; with it
 * off these expand to nothing and the point-to-point build is unchanged. */
#if LX_BUS
#define BUS_P       , uint8_t dst
#define BUS_A(x)    , (x)
#else
#define BUS_P
#define BUS_A(x)
#endif

/* ---------------- CRC-16/CCITT-FALSE, table-free, ~6 ops/byte ------- */
static inline uint16_t crc_step(uint16_t crc, uint8_t b)
{
    uint8_t x = (uint8_t)((crc >> 8) ^ b);
    x ^= (uint8_t)(x >> 4);
    return (uint16_t)((crc << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ x);
}

/* 1 = __builtin_ctz is a native instruction. Elsewhere it becomes a call to
 * libgcc's __ctzsi2 (+256 B __clz_tab on RV32IMC), so use a binary search. */
#ifndef LX_CTZ_BUILTIN
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || \
    defined(__ARM_FEATURE_CLZ) || defined(__XCHAL_HAVE_NSA) || defined(__riscv_zbb))
#define LX_CTZ_BUILTIN 1
#else
#define LX_CTZ_BUILTIN 0
#endif
#endif

static inline uint8_t ctz32(uint32_t m)    /* m != 0 */
{
#if LX_CTZ_BUILTIN
    return (uint8_t)__builtin_ctz(m);
#else
    uint8_t n = 0;
    if (!(m & 0xFFFFu)) { n = 16u; m >>= 16; }
    if (!(m & 0x00FFu)) { n = (uint8_t)(n + 8u); m >>= 8; }
    if (!(m & 0x000Fu)) { n = (uint8_t)(n + 4u); m >>= 4; }
    if (!(m & 0x0003u)) { n = (uint8_t)(n + 2u); m >>= 2; }
    return (uint8_t)(n + (uint8_t)(~m & 1u));
#endif
}

/* ---------------- Streaming encoder: CRC + COBS in a single pass ----- */
typedef struct {
    uint8_t *base, *dst, *code_ptr;
    uint8_t  code;
    uint16_t crc;
} enc_t;

static inline void enc_begin(enc_t *e, uint8_t *buf)
{
    e->base = buf;
#if LX_LEAD_DELIM
    *buf++ = 0x00u;
#endif
    e->code_ptr = buf; e->dst = buf + 1;
    e->code = 1;   e->crc = 0xFFFFu;
}

static inline void enc_raw(enc_t *e, uint8_t b)
{
    if (b) {
        *e->dst++ = b;
        if (++e->code == 0xFFu) {
            *e->code_ptr = 0xFFu; e->code_ptr = e->dst++; e->code = 1;
        }
    } else {
        *e->code_ptr = e->code; e->code_ptr = e->dst++; e->code = 1;
    }
}

static inline void enc_byte(enc_t *e, uint8_t b)
{
    e->crc = crc_step(e->crc, b);
    enc_raw(e, b);
}

static inline uint16_t enc_end(enc_t *e)
{
    uint16_t c = e->crc;
    enc_raw(e, (uint8_t)(c >> 8));      /* big-endian => CRC residue is 0 */
    enc_raw(e, (uint8_t)c);
    *e->code_ptr = e->code;
    *e->dst++ = 0x00u;                  /* frame delimiter */
    return (uint16_t)(e->dst - e->base);
}

static inline void enc_var(enc_t *e, const lx_var_t *v)
{
    const uint8_t *p = (const uint8_t *)v->ptr;
    LX_ENTER_CRITICAL();
    for (uint8_t i = 0; i < v->size; i++) enc_byte(e, p[i]);
    LX_EXIT_CRITICAL();
}

/* HDR [DST SRC]; the caller writes ID next */
static inline void enc_hdr(enc_t *e, const lx_t *h, uint8_t hdr BUS_P)
{
    enc_byte(e, hdr);
#if LX_BUS
    enc_byte(e, dst);
    enc_byte(e, h->addr);
#else
    (void)h;
#endif
}

/* Validate mask, return payload bytes needed (or <0 error). */
static int batch_size(const lx_t *h, uint32_t mask)
{
    if (!mask || (mask & ~h->bound)) return LX_ERR_ID;
    if (!(mask & (mask - 1u))) return h->vars[ctz32(mask)].size;
    int n = 0;
    for (uint32_t m = mask; m; m &= m - 1u) n += 2 + h->vars[ctz32(m)].size;
    return n;
}

/* cmd is the single-var command; batch uses cmd+1. */
static uint16_t build_vars(lx_t *h, uint8_t *buf, uint8_t cmd, uint8_t seq, uint32_t mask BUS_P)
{
    enc_t e; enc_begin(&e, buf);
    if (!(mask & (mask - 1u))) {
        uint8_t id = ctz32(mask);
        enc_hdr(&e, h, (uint8_t)((cmd << 4) | seq) BUS_A(dst));
        enc_byte(&e, id);
        enc_var(&e, &h->vars[id]);
    } else {
        enc_hdr(&e, h, (uint8_t)(((cmd + 1u) << 4) | seq) BUS_A(dst));
        enc_byte(&e, BATCH_ID);
        for (uint32_t m = mask; m; m &= m - 1u) {
            uint8_t id = ctz32(m);
            enc_byte(&e, id);
            enc_byte(&e, h->vars[id].size);
            enc_var(&e, &h->vars[id]);
        }
    }
    return enc_end(&e);
}

/* `st` is st_tx (app path) or st_rsp (RX path): one writer per counter */
static void send(lx_t *h, uint32_t *st, const uint8_t *buf, uint16_t len)
{
    (*st)++;
    h->tx(h->ctx, buf, len);
}

static void reply(lx_t *h, uint8_t cmd, uint8_t seq, uint8_t id, int reason BUS_P)
{
    enc_t e; enc_begin(&e, h->rsp_buf);
    enc_hdr(&e, h, (uint8_t)((cmd << 4) | seq) BUS_A(dst));
    enc_byte(&e, id);
    if (reason >= 0) enc_byte(&e, (uint8_t)reason);
    send(h, &h->st_rsp, h->rsp_buf, enc_end(&e));
}

/* ---------------- Public API ---------------------------------------- */
void lx_init(lx_t *h, lx_tx_fn tx, lx_event_fn ev, void *ctx)
{
    memset(h, 0, sizeof *h);
    h->tx = tx; h->ev = ev; h->ctx = ctx;
    h->rx_crc = 0xFFFFu;
#if LX_BUS
    h->peer = LX_ADDR_BCAST;
#endif
}

int lx_bind(lx_t *h, uint8_t id, void *ptr, uint8_t size)
{
    if (id >= LX_MAX_VARS || !ptr || !size) return LX_ERR_ID;
    if (size > LX_MAX_PAYLOAD) return LX_ERR_SIZE;
    /* lx_tick rebuilds a retransmission from the bindings: a larger size
     * there would overflow tx_buf (the batch was sized at lx_write) */
    if (lx_busy(h) && (h->p_mask & LX_BIT(id))) return LX_ERR_BUSY;
    h->vars[id].ptr = ptr;
    h->vars[id].size = size;
    h->bound |= LX_BIT(id);
    return LX_OK;
}

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
    if (id >= LX_MAX_VARS || !(h->bound & LX_BIT(id))) return LX_ERR_ID;
    LX_ENTER_CRITICAL();
    memcpy(out, h->vars[id].ptr, h->vars[id].size);
    LX_EXIT_CRITICAL();
    return LX_OK;
}

int lx_set(lx_t *h, uint8_t id, const void *in)
{
    if (id >= LX_MAX_VARS || !(h->bound & LX_BIT(id))) return LX_ERR_ID;
    LX_ENTER_CRITICAL();
    memcpy(h->vars[id].ptr, in, h->vars[id].size);
    LX_EXIT_CRITICAL();
    return LX_OK;
}

static int do_publish(lx_t *h, uint32_t mask BUS_P)
{
    int n = batch_size(h, mask);
    if (n < 0) return n;
    if (n > (int)LX_MAX_PAYLOAD) return LX_ERR_SIZE;
    send(h, &h->st_tx, h->tx_buf, build_vars(h, h->tx_buf, CMD_PUB, 0, mask BUS_A(dst)));
    return LX_OK;
}

/* Returns the new transaction's SEQ, or LX_ERR_BUSY. A refused request must
 * not use up a SEQ, or retrying while busy could give the next transaction
 * the previous one's SEQ, and a late answer to that one would end it. */
static int start_txn(lx_t *h, uint8_t cmd, uint32_t mask BUS_P)
{
    uint8_t seq = (uint8_t)((h->seq + 1u) & 0x0Fu);
    LX_ENTER_CRITICAL();
    uint8_t cur = LX_LOAD8(&h->p_cmd);   /* finish() clears it outside the lock */
    if (cur && !(LX_ONE_CONTEXT && cur == P_CLOSING)) { LX_EXIT_CRITICAL(); return LX_ERR_BUSY; }
    LX_STORE8(&h->p_cmd, cmd); h->p_mask = mask; h->p_seq = h->seq = seq;
#if LX_BUS
    h->p_dst = dst;
#endif
    h->p_tries = 0; h->deadline = h->now + LX_TIMEOUT_MS;
    LX_EXIT_CRITICAL();
    return seq;
}

/* (Re)send a request from values the caller captured: by the time this runs
 * the RX path may already have ended the transaction. */
static void transmit(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t mask BUS_P)
{
    uint16_t len;
    if (cmd == CMD_READ) {
        enc_t e; enc_begin(&e, h->tx_buf);
        enc_hdr(&e, h, (uint8_t)((CMD_READ << 4) | seq) BUS_A(dst));
        enc_byte(&e, ctz32(mask));
        len = enc_end(&e);
    } else {
        len = build_vars(h, h->tx_buf, CMD_WRITE, seq, mask BUS_A(dst));
    }
    send(h, &h->st_tx, h->tx_buf, len);
}

static int do_write(lx_t *h, uint32_t mask BUS_P)
{
    int n = batch_size(h, mask);
    if (n < 0) return n;
    if (n > (int)LX_MAX_PAYLOAD) return LX_ERR_SIZE;
    int seq = start_txn(h, CMD_WRITE, mask BUS_A(dst));
    if (seq < 0) return seq;
    transmit(h, CMD_WRITE, (uint8_t)seq, mask BUS_A(dst));
    return LX_OK;
}

static int do_read(lx_t *h, uint8_t id BUS_P)
{
    if (id >= LX_MAX_VARS || !(h->bound & LX_BIT(id))) return LX_ERR_ID;
    int seq = start_txn(h, CMD_READ, LX_BIT(id) BUS_A(dst));
    if (seq < 0) return seq;
    transmit(h, CMD_READ, (uint8_t)seq, LX_BIT(id) BUS_A(dst));
    return LX_OK;
}

#if LX_BUS
int lx_set_addr(lx_t *h, uint8_t self, uint8_t peer)
{
    if (self == LX_ADDR_BCAST || self == peer) return LX_ERR_ADDR;
    h->addr = self; h->peer = peer;
    return LX_OK;
}

int lx_publish_to(lx_t *h, uint8_t dst, uint32_t mask)
{
    return dst == h->addr ? LX_ERR_ADDR : do_publish(h, mask, dst);
}

/* Reliable ops need exactly one responder: no broadcast, no self. */
int lx_write_to(lx_t *h, uint8_t dst, uint32_t mask)
{
    return (dst == h->addr || dst == LX_ADDR_BCAST) ? LX_ERR_ADDR : do_write(h, mask, dst);
}

int lx_read_from(lx_t *h, uint8_t src, uint8_t id)
{
    return (src == h->addr || src == LX_ADDR_BCAST) ? LX_ERR_ADDR : do_read(h, id, src);
}

int lx_publish(lx_t *h, uint32_t mask) { return lx_publish_to(h, h->peer, mask); }
int lx_write(lx_t *h, uint32_t mask)   { return lx_write_to(h, h->peer, mask); }
int lx_read(lx_t *h, uint8_t id)       { return lx_read_from(h, h->peer, id); }
#else
int lx_publish(lx_t *h, uint32_t mask) { return do_publish(h, mask); }
int lx_write(lx_t *h, uint32_t mask)   { return do_write(h, mask); }
int lx_read(lx_t *h, uint8_t id)       { return do_read(h, id); }
#endif

/* Deliver the one event of a transaction already marked P_CLOSING, then let
 * it go idle: lx_busy() stays true until the callback has returned, so no
 * other context can start a new transaction and mistake this event for it.
 * (One-context builds: the callback may itself have started the next one.) */
static void finish(lx_t *h, uint8_t ev, uint32_t mask)
{
    if (h->ev) h->ev(h->ctx, ev, mask);
    if (LX_LOAD8(&h->p_cmd) == P_CLOSING) LX_STORE8(&h->p_cmd, 0);
}

/* Does an answer from `from` with `seq` belong to the transaction in flight?
 * Call under the lock. On a bus only the node that was asked may answer. */
static bool answers(const lx_t *h, uint8_t cmd, uint8_t seq, uint8_t from)
{
#if LX_BUS
    if (from != h->p_dst) return false;
#else
    (void)from;
#endif
    uint8_t cur = LX_LOAD8(&h->p_cmd);
    return cur && cur != P_CLOSING && (!cmd || cur == cmd) && seq == h->p_seq;
}

/* Close the transaction in flight if the answer belongs to it (cmd 0 = any).
 * Decided under the lock, so exactly one of lx_tick and the RX path wins. */
static bool claim(lx_t *h, uint8_t cmd, uint8_t seq, uint8_t from, uint32_t *mask)
{
    bool won = false;
    LX_ENTER_CRITICAL();
    if (answers(h, cmd, seq, from)) {
        *mask = h->p_mask; LX_STORE8(&h->p_cmd, P_CLOSING); won = true;
    }
    LX_EXIT_CRITICAL();
    return won;
}

void lx_tick(lx_t *h, uint32_t now_ms)
{
    h->now = now_ms;
    if (!LX_LOAD8(&h->p_cmd) || (int32_t)(now_ms - h->deadline) < 0) return;

    /* Decide and snapshot under the lock: once it drops, an answer may end
     * the transaction, and the retransmit must still send what was asked. */
    uint8_t cmd = 0, seq = 0;
    uint32_t mask = 0;
    bool give_up = false;
#if LX_BUS
    uint8_t dst = 0;
#endif
    LX_ENTER_CRITICAL();
    uint8_t cur = LX_LOAD8(&h->p_cmd);
    if (cur && cur != P_CLOSING && (int32_t)(now_ms - h->deadline) >= 0) {
        mask = h->p_mask;
        if (h->p_tries >= LX_RETRIES) {
            LX_STORE8(&h->p_cmd, P_CLOSING); give_up = true;
        } else {
            cmd = cur; seq = h->p_seq;
#if LX_BUS
            dst = h->p_dst;
#endif
            h->p_tries++; h->deadline = now_ms + LX_TIMEOUT_MS;
        }
    }
    LX_EXIT_CRITICAL();

    if (give_up)  finish(h, LX_EV_TIMEOUT, mask);
    else if (cmd) { h->st_retx++; transmit(h, cmd, seq, mask BUS_A(dst)); }
}

/* ---------------- RX path ------------------------------------------- */
static inline void commit(lx_t *h, uint8_t id, const uint8_t *src)
{
    memcpy(h->vars[id].ptr, src, h->vars[id].size);
    h->updated |= LX_BIT(id);
}

/* 0 = ok, else NACK reason event code */
static int check_var(const lx_t *h, uint8_t id, uint8_t len)
{
    if (id >= LX_MAX_VARS || !(h->bound & LX_BIT(id))) return LX_EV_NACK_ID;
    if (len != h->vars[id].size) return LX_EV_NACK_SIZE;
    return 0;
}

static void on_frame(lx_t *h, const uint8_t *f, uint8_t n)
{
    uint8_t cmd = f[0] >> 4, seq = f[0] & 0x0Fu;
#if LX_BUS
    uint8_t dst = f[1], src = f[2];
    if (src == h->addr || src == LX_ADDR_BCAST) return;     /* own echo / bogus source */
    if (dst == LX_ADDR_BCAST) {                              /* broadcasts are never answered */
        if (cmd == CMD_WRITE)        cmd = CMD_PUB;
        else if (cmd == CMD_WRITE_M) cmd = CMD_PUB_M;
        else if (cmd != CMD_PUB && cmd != CMD_PUB_M) return;
    } else if (dst != h->addr) {
        return;                                              /* someone else's frame */
    }
    f += 2; n = (uint8_t)(n - 2u);
#else
    const uint8_t src = 0;      /* point to point: every answer is from the peer */
#endif
    uint8_t id = f[1];
    const uint8_t *p = f + 2;
    uint8_t plen = (uint8_t)(n - 2u);
    uint32_t mask;
    int err;

    switch (cmd) {
    case CMD_WRITE: case CMD_PUB:
        err = check_var(h, id, plen);
        if (!err) { LX_ENTER_CRITICAL(); commit(h, id, p); LX_EXIT_CRITICAL(); }
        if (cmd == CMD_WRITE) reply(h, err ? CMD_NACK : CMD_ACK, seq, id, err ? err : -1 BUS_A(src));
        break;

    case CMD_WRITE_M: case CMD_PUB_M: {
        /* validate everything first, then commit atomically */
        uint8_t i = 0; err = 0;
        while (i < plen) {
            if (plen - i < 2) { err = LX_EV_NACK_SIZE; break; }
            uint8_t vid = p[i], vlen = p[i + 1];
            if (plen - i - 2 < vlen) { err = LX_EV_NACK_SIZE; break; }
            if ((err = check_var(h, vid, vlen))) break;
            i = (uint8_t)(i + 2u + vlen);
        }
        if (!err) {
            LX_ENTER_CRITICAL();
            for (i = 0; i < plen; i = (uint8_t)(i + 2u + p[i + 1])) commit(h, p[i], p + i + 2);
            LX_EXIT_CRITICAL();
        }
        if (cmd == CMD_WRITE_M) reply(h, err ? CMD_NACK : CMD_ACK, seq, id, err ? err : -1 BUS_A(src));
        break;
    }

    case CMD_READ:
        err = (id >= LX_MAX_VARS || !(h->bound & LX_BIT(id))) ? LX_EV_NACK_ID
            : (plen ? LX_EV_NACK_SIZE : 0);
        if (err) {
            reply(h, CMD_NACK, seq, id, err BUS_A(src));
        } else {
            enc_t e; enc_begin(&e, h->rsp_buf);
            enc_hdr(&e, h, (uint8_t)((CMD_READ_RSP << 4) | seq) BUS_A(src));
            enc_byte(&e, id);
            enc_var(&e, &h->vars[id]);
            send(h, &h->st_rsp, h->rsp_buf, enc_end(&e));
        }
        break;

    case CMD_READ_RSP: {
        /* match, commit and end in one section: the value is in place
         * before anyone can see the transaction as finished */
        bool won = false;
        err = check_var(h, id, plen);
        LX_ENTER_CRITICAL();
        if (answers(h, CMD_READ, seq, src) && id < LX_MAX_VARS && LX_BIT(id) == h->p_mask) {
            if (!err) commit(h, id, p);
            mask = h->p_mask; LX_STORE8(&h->p_cmd, P_CLOSING); won = true;
        }
        LX_EXIT_CRITICAL();
        if (won) finish(h, err ? (uint8_t)err : LX_EV_DONE, mask);
        break;
    }

    case CMD_ACK:
        if (claim(h, CMD_WRITE, seq, src, &mask)) finish(h, LX_EV_DONE, mask);
        break;

    case CMD_NACK:  /* the reason comes off the wire: never let it read as DONE */
        if (plen == 1 && (p[0] == LX_EV_NACK_ID || p[0] == LX_EV_NACK_SIZE) && claim(h, 0, seq, src, &mask))
            finish(h, p[0], mask);
        break;

    default:
        h->st_rx_bad++;
        return;
    }
    h->st_rx_ok++;
}

static inline void rx_push(lx_t *h, uint8_t b)
{
    if (h->rx_len >= LX_RAW_MAX) { h->rx_drop = 1; return; }
    h->rx_buf[h->rx_len++] = b;
    h->rx_crc = crc_step(h->rx_crc, b);
}

static inline void rx_byte(lx_t *h, uint8_t b)
{
    if (b == 0x00u) {                               /* end of frame */
        if (h->rx_code) {
            if (!h->rx_drop && h->rx_rem == 0 && h->rx_len >= HEAD_LEN + 2u && h->rx_crc == 0)
                on_frame(h, h->rx_buf, (uint8_t)(h->rx_len - 2u));
            else
                h->st_rx_bad++;
        }
        h->rx_len = 0; h->rx_code = 0; h->rx_rem = 0; h->rx_drop = 0;
        h->rx_crc = 0xFFFFu;
        return;
    }
    if (h->rx_drop) return;
    if (h->rx_rem == 0) {                           /* COBS code byte */
        if (h->rx_code && h->rx_code != 0xFFu) rx_push(h, 0x00u);
        h->rx_code = b;
        h->rx_rem = (uint8_t)(b - 1u);
    } else {
        rx_push(h, b);
        h->rx_rem--;
    }
}

#if LX_ISR_SAFE
/* Single-producer / single-consumer byte queue. lx_rx owns rxq_head, rxq_gap,
 * rxq_lost and st_rx_ovf; lx_poll owns rxq_tail. Acquire/release on the
 * indices orders the data, also across cores. When the queue is full lx_rx
 * drops the byte and flags the slot of the next byte it stores, so lx_poll
 * discards exactly the frames that lost bytes, however many gaps there are. */
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
    uint8_t head = h->rxq_head, next = RXQ_NEXT(head);
    if (next == LX_LOAD8(&h->rxq_tail)) {           /* full: drop */
        h->st_rx_ovf++; h->rxq_lost = 1;
        return;
    }
    uint8_t *g = &h->rxq_gap[head >> 3], v = LOAD_RLX(g);   /* we are its only writer */
    STORE_RLX(g, h->rxq_lost ? (v | GAP_BIT(head)) : (v & (uint8_t)~GAP_BIT(head)));
    h->rxq_lost = 0;
    h->rxq[head] = b;
    LX_STORE8(&h->rxq_head, next);
}

size_t lx_poll(lx_t *h)
{
    uint8_t tail = h->rxq_tail, head = LX_LOAD8(&h->rxq_head);
    size_t n = 0;
    while (tail != head) {
        if (LOAD_RLX(&h->rxq_gap[tail >> 3]) & GAP_BIT(tail))
            h->rx_drop = 1;     /* bytes are missing right before this one */
        uint8_t b = h->rxq[tail];
        tail = RXQ_NEXT(tail);
        LX_STORE8(&h->rxq_tail, tail);
        rx_byte(h, b);
        n++;
    }
    return n;
}
#else
void lx_rx(lx_t *h, uint8_t b)
{
    rx_byte(h, b);
}
#endif

void lx_rx_buf(lx_t *h, const uint8_t *data, size_t len)
{
    while (len--) lx_rx(h, *data++);
}
