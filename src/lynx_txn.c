/*
 * The reliable transaction: one write or read in flight per handle.
 *
 * Life of a transaction (h->p_cmd, see lynx_internal.h):
 *
 *   lx_write / lx_read        lx__txn_begin: IDLE -> CMD_WRITE / CMD_READ,
 *                             takes the next SEQ (mod 16), sends the request
 *   no answer by deadline     lx_tick: retransmit the same request, up to
 *                             LX_RETRIES times, then close with TIMEOUT
 *   matching answer           lynx_dispatch.c: close with DONE or NACK
 *   closing                   -> TXN_CLOSING, deliver the one event,
 *                             -> IDLE (lx_busy() turns false only now)
 *
 * The RX path (answers) and the app path (lx_tick) can run in different
 * contexts. Both decide under the lock whether they are the one closing the
 * transaction, so it ends with exactly one event.
 *
 * MIT License (c) 2026 Ashwath
 */
#include "lynx_internal.h"

/* "now is at or after t" for a millisecond clock that wraps around. */
static inline bool reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

/* Send (or resend) a request. The caller passes a copy of the request,
 * taken under the lock: by the time this runs, the RX path may already have
 * ended the transaction. */
static void transmit(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t mask BUS_ADDR_PARAM(dst))
{
    uint16_t len = lx__build_request(h, cmd, seq, mask BUS_ADDR_ARG(dst));
    lx__send(h, &h->st_tx, h->tx_buf, len);
}

int lx__txn_begin(lx_t *h, uint8_t cmd, uint32_t mask BUS_ADDR_PARAM(dst))
{
    /* A refused attempt still uses up a SEQ value; that is harmless. */
    uint8_t seq = h->seq = (uint8_t)((h->seq + 1u) & 0x0Fu);

    LX_ENTER_CRITICAL();
    uint8_t state = LX_LOAD8(&h->p_cmd);
    /* Busy, unless the previous transaction is closing and we are being
     * called from its event callback in a one-context build. */
    if (state != TXN_IDLE && !(LX_ONE_CONTEXT && state == TXN_CLOSING)) {
        LX_EXIT_CRITICAL();
        return LX_ERR_BUSY;
    }
    LX_STORE8(&h->p_cmd, cmd);
    h->p_mask = mask;
    h->p_seq = seq;
#if LX_BUS
    h->p_dst = dst;
#endif
    h->p_tries = 0;
    h->deadline = h->now + LX_TIMEOUT_MS;
    LX_EXIT_CRITICAL();

    transmit(h, cmd, seq, mask BUS_ADDR_ARG(dst));
    return LX_OK;
}

void lx_tick(lx_t *h, uint32_t now_ms)
{
    h->now = now_ms;
    if (LX_LOAD8(&h->p_cmd) == TXN_IDLE || !reached(now_ms, h->deadline)) return;

    /* The deadline has passed. Decide under the lock (an answer may be
     * closing the transaction right now on the RX path), and copy what a
     * retransmission needs before the lock drops. */
    uint8_t resend_cmd = 0, seq = 0;
    uint32_t mask = 0;
    bool give_up = false;
#if LX_BUS
    uint8_t dst = 0;
#endif

    LX_ENTER_CRITICAL();
    uint8_t state = LX_LOAD8(&h->p_cmd);
    if (state != TXN_IDLE && state != TXN_CLOSING && reached(now_ms, h->deadline)) {
        mask = h->p_mask;
        if (h->p_tries >= LX_RETRIES) {
            LX_STORE8(&h->p_cmd, TXN_CLOSING);
            give_up = true;
        } else {
            resend_cmd = state;
            seq = h->p_seq;
#if LX_BUS
            dst = h->p_dst;
#endif
            h->p_tries++;
            h->deadline = now_ms + LX_TIMEOUT_MS;
        }
    }
    LX_EXIT_CRITICAL();

    if (give_up) {
        lx__txn_finish(h, LX_EV_TIMEOUT, mask);
    } else if (resend_cmd) {
        h->st_retx++;
        transmit(h, resend_cmd, seq, mask BUS_ADDR_ARG(dst));
    }
}

bool lx__txn_matches(const lx_t *h, uint8_t cmd, uint8_t seq BUS_ADDR_PARAM(from))
{
#if LX_BUS
    if (from != h->p_dst) return false;     /* only the node we asked may answer */
#endif
    uint8_t state = LX_LOAD8(&h->p_cmd);
    return state != TXN_IDLE && state != TXN_CLOSING
        && (cmd == 0 || state == cmd)
        && seq == h->p_seq;                 /* not a late answer to an older request */
}

bool lx__txn_claim(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t *mask BUS_ADDR_PARAM(from))
{
    bool won = false;
    LX_ENTER_CRITICAL();
    if (lx__txn_matches(h, cmd, seq BUS_ADDR_ARG(from))) {
        *mask = h->p_mask;
        LX_STORE8(&h->p_cmd, TXN_CLOSING);
        won = true;
    }
    LX_EXIT_CRITICAL();
    return won;
}

/* lx_busy() stays true while the callback runs, so no other context can
 * start a new transaction and take this event for its own. In a one-context
 * build the callback may itself start the next transaction; then p_cmd is
 * no longer TXN_CLOSING and must be left alone. */
void lx__txn_finish(lx_t *h, uint8_t event, uint32_t mask)
{
    if (h->ev) h->ev(h->ctx, event, mask);
    if (LX_LOAD8(&h->p_cmd) == TXN_CLOSING) LX_STORE8(&h->p_cmd, TXN_IDLE);
}
