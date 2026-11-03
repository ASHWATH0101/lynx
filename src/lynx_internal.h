/*
 * LYNX internals shared by the source files. Not part of the API: never
 * include this from application code.
 *
 * Functions that one source file provides to another are named lx__*.
 * See docs/architecture.md for how the files fit together.
 *
 * MIT License (c) 2026 Ashwath
 */
#ifndef LYNX_INTERNAL_H
#define LYNX_INTERNAL_H

#include "lynx.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Frame layout                                                        */
/* ------------------------------------------------------------------ */

/* Raw frame (before COBS):  HDR [DST SRC] ID PAYLOAD... CRC_HI CRC_LO
 * with HDR = CMD << 4 | SEQ. */
enum {
    CMD_WRITE    = 1,   /* one variable,  answered with ACK / NACK        */
    CMD_WRITE_M  = 2,   /* batch,         answered with ACK / NACK        */
    CMD_PUB      = 3,   /* one variable,  never answered                  */
    CMD_PUB_M    = 4,   /* batch,         never answered                  */
    CMD_READ     = 5,   /* request,       answered with READ_RSP / NACK   */
    CMD_READ_RSP = 6,   /* the variable's value                           */
    CMD_ACK      = 7,   /* write committed                                */
    CMD_NACK     = 8,   /* request refused, 1-byte reason: LX_EV_NACK_*   */
};

#define MAKE_HDR(cmd, seq)  ((uint8_t)(((cmd) << 4) | (seq)))
#define BATCH_ID   0xFFu                /* ID byte of a batch frame (WRITE_M / PUB_M) */
#define HEAD_LEN   (2u + 2u * LX_BUS)   /* HDR [DST SRC] ID */
#define CRC_LEN    2u

/* ------------------------------------------------------------------ */
/* Transaction state                                                   */
/* ------------------------------------------------------------------ */

/* h->p_cmd says where the reliable transaction (write or read) is:
 *   TXN_IDLE              nothing in flight, lx_busy() is false
 *   CMD_WRITE / CMD_READ  request sent, waiting for its answer or timeout
 *   TXN_CLOSING           answered or timed out; its one event is being
 *                         delivered. Still busy, so nobody can start a new
 *                         transaction and mistake the event for theirs.
 * Always read and written with LX_LOAD8 / LX_STORE8. */
#define TXN_IDLE     0u
#define TXN_CLOSING  0x0Fu

/* ------------------------------------------------------------------ */
/* Bus addresses as function parameters                                */
/* ------------------------------------------------------------------ */

/* In bus builds, functions that build or match a frame need the other
 * node's address. Point-to-point builds have no addresses, so there the
 * parameter must not exist at all (no code, no stack). These macros add it
 * only when LX_BUS is on:
 *
 *   int f(lx_t *h, uint32_t mask  BUS_ADDR_PARAM(dst));
 *       LX_BUS=1:  int f(lx_t *h, uint32_t mask, uint8_t dst);
 *       LX_BUS=0:  int f(lx_t *h, uint32_t mask);
 *
 *   f(h, mask  BUS_ADDR_ARG(h->peer));
 *       LX_BUS=1:  f(h, mask, (h->peer));
 *       LX_BUS=0:  f(h, mask);
 */
#if LX_BUS
#define BUS_ADDR_PARAM(name)  , uint8_t name
#define BUS_ADDR_ARG(x)       , (x)
#else
#define BUS_ADDR_PARAM(name)
#define BUS_ADDR_ARG(x)
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* One step of CRC-16/CCITT-FALSE (poly 0x1021), table-free, ~6 operations.
 * Used by the encoder and the decoder. */
static inline uint16_t lx__crc_step(uint16_t crc, uint8_t b)
{
    uint8_t x = (uint8_t)((crc >> 8) ^ b);
    x ^= (uint8_t)(x >> 4);
    return (uint16_t)((crc << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ x);
}

static inline bool lx__is_bound(const lx_t *h, uint8_t id)
{
    return id < LX_MAX_VARS && (h->bound & LX_BIT(id));
}

/* Hand an encoded frame to the application's tx callback. `counter` is
 * st_tx (app path) or st_rsp (RX path), so each counter has one writer. */
static inline void lx__send(lx_t *h, uint32_t *counter, const uint8_t *frame, uint16_t len)
{
    (*counter)++;
    h->tx(h->ctx, frame, len);
}

/* ------------------------------------------------------------------ */
/* lynx_encode.c: building frames                                      */
/* ------------------------------------------------------------------ */

/* Check that every variable in `mask` is bound and that they fit in one
 * frame. Returns LX_OK, LX_ERR_ID or LX_ERR_SIZE. */
int lx__check_vars(const lx_t *h, uint32_t mask);

/* Encode a frame carrying the variables in `mask` into `buf`: `cmd` for a
 * single variable, cmd + 1 (the _M command) for several. Returns its length. */
uint16_t lx__build_vars(lx_t *h, uint8_t *buf, uint8_t cmd, uint8_t seq, uint32_t mask
                        BUS_ADDR_PARAM(dst));

/* Encode a frame without variable data: READ, ACK, or NACK. `reason` is the
 * NACK reason byte, or 0 for none. Returns its length. */
uint16_t lx__build_short(lx_t *h, uint8_t *buf, uint8_t cmd, uint8_t seq, uint8_t id,
                         uint8_t reason BUS_ADDR_PARAM(dst));

/* Encode the request of a write (CMD_WRITE, the variables in mask) or read
 * (CMD_READ, the one id in mask) into h->tx_buf. Returns its length. */
uint16_t lx__build_request(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t mask
                           BUS_ADDR_PARAM(dst));

/* ------------------------------------------------------------------ */
/* lynx_dispatch.c: acting on received frames                          */
/* ------------------------------------------------------------------ */

/* Act on one valid frame (CRC checked, CRC bytes removed, n >= HEAD_LEN). */
void lx__dispatch(lx_t *h, const uint8_t *frame, uint8_t n);

/* ------------------------------------------------------------------ */
/* lynx_txn.c: the reliable transaction                                */
/* ------------------------------------------------------------------ */

/* Start a write (CMD_WRITE, variables in mask) or read (CMD_READ, the one
 * id in mask) and send the request. Returns LX_OK or LX_ERR_BUSY. */
int lx__txn_begin(lx_t *h, uint8_t cmd, uint32_t mask BUS_ADDR_PARAM(dst));

/* Is an answer with this command and SEQ, from this node, for the
 * transaction in flight? `cmd` 0 matches any request. Call under the lock. */
bool lx__txn_matches(const lx_t *h, uint8_t cmd, uint8_t seq BUS_ADDR_PARAM(from));

/* If the answer matches, close the transaction (TXN_CLOSING) and return its
 * mask in *mask. Then call lx__txn_finish. Exactly one caller can win. */
bool lx__txn_claim(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t *mask BUS_ADDR_PARAM(from));

/* Deliver the event of a closed transaction, then mark the handle idle. */
void lx__txn_finish(lx_t *h, uint8_t event, uint32_t mask);

#endif /* LYNX_INTERNAL_H */
