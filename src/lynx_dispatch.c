/*
 * Acting on received frames.
 *
 * lynx_decode.c hands over each frame that passed the CRC check. Here it is
 * filtered by address (bus mode), then handled by command:
 *
 *   WRITE, WRITE_M     check, commit the values, answer ACK or NACK
 *   PUB, PUB_M         check, commit the values, no answer
 *   READ               answer READ_RSP with our value, or NACK
 *   READ_RSP, ACK,     answers to OUR request: give them to the transaction
 *   NACK                 in flight (lynx_txn.c), which ends it with an event
 *
 * Everything here runs on the RX path, so replies use h->rsp_buf and are
 * counted in st_rsp (see lynx.h, Concurrency).
 *
 * MIT License (c) 2026 Ashwath
 */
#include "lynx_internal.h"

/* A received frame, split into its fields. */
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t id;                 /* variable id, or BATCH_ID                */
    uint8_t len;                /* payload bytes                           */
    const uint8_t *payload;
#if LX_BUS
    uint8_t src;                /* sender: replies go back to it           */
#endif
} rx_frame_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Copy a received value into the bound variable and flag it as updated.
 * Call under the lock. */
static inline void commit(lx_t *h, uint8_t id, const uint8_t *value)
{
    memcpy(h->vars[id].ptr, value, h->vars[id].size);
    h->updated |= LX_BIT(id);
}

/* Can a value of `len` bytes be stored in variable `id`?
 * Returns 0 if yes, else the NACK reason. */
static int check_value(const lx_t *h, uint8_t id, uint8_t len)
{
    if (!lx__is_bound(h, id)) return LX_EV_NACK_ID;
    if (len != h->vars[id].size) return LX_EV_NACK_SIZE;
    return 0;
}

/* Answer a request: ACK if err is 0, else NACK with err as the reason. */
static void answer(lx_t *h, const rx_frame_t *f, int err)
{
    uint8_t cmd = err ? CMD_NACK : CMD_ACK;
    uint16_t len = lx__build_short(h, h->rsp_buf, cmd, f->seq, f->id, (uint8_t)err
                                   BUS_ADDR_ARG(f->src));
    lx__send(h, &h->st_rsp, h->rsp_buf, len);
}

/* ------------------------------------------------------------------ */
/* Requests from the other node                                        */
/* ------------------------------------------------------------------ */

/* WRITE / PUB: one variable. */
static void on_write(lx_t *h, const rx_frame_t *f)
{
    int err = check_value(h, f->id, f->len);
    if (!err) {
        LX_ENTER_CRITICAL();
        commit(h, f->id, f->payload);
        LX_EXIT_CRITICAL();
    }
    if (f->cmd == CMD_WRITE) answer(h, f, err);
}

/* Check every (id, len, value) entry of a batch payload.
 * Returns 0 if all of them can be committed, else the NACK reason. */
static int check_batch(const lx_t *h, const uint8_t *p, uint8_t len)
{
    uint8_t i = 0;
    while (i < len) {
        if (len - i < 2) return LX_EV_NACK_SIZE;            /* entry header cut off */
        uint8_t id = p[i], size = p[i + 1];
        if (len - i - 2 < size) return LX_EV_NACK_SIZE;     /* value runs past the end */
        int err = check_value(h, id, size);
        if (err) return err;
        i = (uint8_t)(i + 2u + size);
    }
    return 0;
}

/* WRITE_M / PUB_M: several variables, all or nothing. Everything is checked
 * first, then committed in one critical section. */
static void on_write_batch(lx_t *h, const rx_frame_t *f)
{
    const uint8_t *p = f->payload;
    int err = check_batch(h, p, f->len);
    if (!err) {
        LX_ENTER_CRITICAL();
        for (uint8_t i = 0; i < f->len; i = (uint8_t)(i + 2u + p[i + 1]))
            commit(h, p[i], p + i + 2);
        LX_EXIT_CRITICAL();
    }
    if (f->cmd == CMD_WRITE_M) answer(h, f, err);
}

/* READ: send our value of the variable back. */
static void on_read(lx_t *h, const rx_frame_t *f)
{
    int err = !lx__is_bound(h, f->id) ? LX_EV_NACK_ID
            : f->len != 0             ? LX_EV_NACK_SIZE     /* a READ carries no payload */
            : 0;
    if (err) {
        answer(h, f, err);
        return;
    }
    uint16_t len = lx__build_vars(h, h->rsp_buf, CMD_READ_RSP, f->seq, LX_BIT(f->id)
                                  BUS_ADDR_ARG(f->src));
    lx__send(h, &h->st_rsp, h->rsp_buf, len);
}

/* ------------------------------------------------------------------ */
/* Answers to our own write or read                                    */
/* ------------------------------------------------------------------ */

/* READ_RSP: matching it, committing the value and closing the transaction
 * happen in one critical section, so the value is in place before anyone can
 * see the read as finished. A response with a bad id or size still ends the
 * read, with the matching NACK event. */
static void on_read_rsp(lx_t *h, const rx_frame_t *f)
{
    int err = check_value(h, f->id, f->len);
    bool ours = false;
    uint32_t mask = 0;

    LX_ENTER_CRITICAL();
    if (lx__txn_matches(h, CMD_READ, f->seq BUS_ADDR_ARG(f->src))
        && f->id < LX_MAX_VARS && LX_BIT(f->id) == h->p_mask) {
        if (!err) commit(h, f->id, f->payload);
        mask = h->p_mask;
        LX_STORE8(&h->p_cmd, TXN_CLOSING);
        ours = true;
    }
    LX_EXIT_CRITICAL();

    if (ours) lx__txn_finish(h, err ? (uint8_t)err : LX_EV_DONE, mask);
}

static void on_ack(lx_t *h, const rx_frame_t *f)
{
    uint32_t mask;
    if (lx__txn_claim(h, CMD_WRITE, f->seq, &mask BUS_ADDR_ARG(f->src)))
        lx__txn_finish(h, LX_EV_DONE, mask);
}

/* NACK answers a write or a read. Its reason byte comes off the wire, so only
 * the two NACK reasons are accepted: it must never be passed on as DONE. */
static void on_nack(lx_t *h, const rx_frame_t *f)
{
    uint32_t mask;
    if (f->len != 1) return;
    uint8_t reason = f->payload[0];
    if (reason != LX_EV_NACK_ID && reason != LX_EV_NACK_SIZE) return;
    if (lx__txn_claim(h, 0, f->seq, &mask BUS_ADDR_ARG(f->src)))
        lx__txn_finish(h, reason, mask);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

#if LX_BUS
/* Is this frame for us? A broadcast WRITE is turned into a publish, because
 * broadcasts are never answered (every node would answer at once). */
static bool for_us(const lx_t *h, uint8_t *cmd, uint8_t dst, uint8_t src)
{
    if (src == h->addr || src == LX_ADDR_BCAST) return false;   /* own echo / bogus source */
    if (dst == LX_ADDR_BCAST) {
        if (*cmd == CMD_WRITE)        *cmd = CMD_PUB;
        else if (*cmd == CMD_WRITE_M) *cmd = CMD_PUB_M;
        else if (*cmd != CMD_PUB && *cmd != CMD_PUB_M) return false;
        return true;
    }
    return dst == h->addr;                                      /* else someone else's */
}
#endif

void lx__dispatch(lx_t *h, const uint8_t *raw, uint8_t n)
{
    rx_frame_t f;
    f.cmd = raw[0] >> 4;
    f.seq = raw[0] & 0x0Fu;
#if LX_BUS
    f.src = raw[2];
    if (!for_us(h, &f.cmd, raw[1], f.src)) return;
    raw += 2;                           /* skip DST SRC */
    n = (uint8_t)(n - 2u);
#endif
    f.id = raw[1];
    f.payload = raw + 2;
    f.len = (uint8_t)(n - 2u);

    switch (f.cmd) {
    case CMD_WRITE:
    case CMD_PUB:      on_write(h, &f);       break;
    case CMD_WRITE_M:
    case CMD_PUB_M:    on_write_batch(h, &f); break;
    case CMD_READ:     on_read(h, &f);        break;
    case CMD_READ_RSP: on_read_rsp(h, &f);    break;
    case CMD_ACK:      on_ack(h, &f);         break;
    case CMD_NACK:     on_nack(h, &f);        break;
    default:
        h->st_rx_bad++;                 /* unknown command */
        return;
    }
    h->st_rx_ok++;
}
