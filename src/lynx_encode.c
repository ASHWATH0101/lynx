/*
 * Building frames: variables -> bytes ready for the tx callback.
 *
 * A frame is encoded in one pass. Each byte goes through the CRC and the
 * COBS encoder as it is produced, so the raw frame never exists in memory
 * and nothing is scanned twice.
 *
 * COBS (Consistent Overhead Byte Stuffing) removes every 0x00 from the
 * frame, so that 0x00 can mark where frames end. The output is a series of
 * blocks. Each block is a code byte followed by (code - 1) non-zero bytes:
 *   - code < 0xFF: the block stood for those bytes plus one 0x00;
 *   - code = 0xFF: 254 non-zero bytes, no 0x00 (a long run with no zero).
 * Example: raw 11 22 00 33  ->  03 11 22 02 33  then the 00 delimiter.
 * A block's code byte comes before its data, but is only known when the
 * block ends, so the encoder reserves its slot (code_at) and fills it in.
 *
 * MIT License (c) 2026 Ashwath
 */
#include "lynx_internal.h"

/* ------------------------------------------------------------------ */
/* Lowest set bit                                                      */
/* ------------------------------------------------------------------ */

/* LX_CTZ_BUILTIN = 1: __builtin_ctz is a native instruction. Elsewhere GCC
 * turns it into a call to libgcc's __ctzsi2 (+256 B __clz_tab on RV32IMC),
 * so a 5-step binary search is used instead. */
#ifndef LX_CTZ_BUILTIN
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || \
    defined(__ARM_FEATURE_CLZ) || defined(__XCHAL_HAVE_NSA) || defined(__riscv_zbb))
#define LX_CTZ_BUILTIN 1
#else
#define LX_CTZ_BUILTIN 0
#endif
#endif

/* Index of the lowest set bit of m (m != 0): the first variable id in a mask. */
static inline uint8_t first_id(uint32_t m)
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

/* More than one bit set? (m & (m - 1) clears the lowest set bit.) */
static inline bool several(uint32_t m)
{
    return (m & (m - 1u)) != 0;
}

/* ------------------------------------------------------------------ */
/* Streaming encoder: CRC + COBS in one pass                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *start;     /* first byte of the output buffer                */
    uint8_t *out;       /* where the next data byte goes                  */
    uint8_t *code_at;   /* reserved slot for the current block's code     */
    uint8_t  code;      /* 1 + data bytes in the current block so far     */
    uint16_t crc;       /* CRC of every raw byte so far                   */
} frame_enc_t;

static inline void enc_begin(frame_enc_t *e, uint8_t *buf)
{
    e->start = buf;
#if LX_LEAD_DELIM
    *buf++ = 0x00u;                     /* ends any noise before this frame */
#endif
    e->code_at = buf;                   /* first block's code goes here */
    e->out = buf + 1;
    e->code = 1;
    e->crc = 0xFFFFu;
}

/* COBS-encode one raw byte (no CRC). */
static inline void enc_cobs(frame_enc_t *e, uint8_t b)
{
    if (b != 0x00u) {
        *e->out++ = b;
        if (++e->code == 0xFFu) {       /* block full: close it, no zero implied */
            *e->code_at = 0xFFu;
            e->code_at = e->out++;
            e->code = 1;
        }
    } else {                            /* a zero ends the block */
        *e->code_at = e->code;
        e->code_at = e->out++;
        e->code = 1;
    }
}

/* Add one byte to the frame: CRC and COBS. */
static inline void enc_byte(frame_enc_t *e, uint8_t b)
{
    e->crc = lx__crc_step(e->crc, b);
    enc_cobs(e, b);
}

/* Add a bound variable's bytes, copied under the lock so that a commit from
 * the RX path cannot change it halfway. */
static inline void enc_var(frame_enc_t *e, const lx_var_t *v)
{
    const uint8_t *p = (const uint8_t *)v->ptr;
    LX_ENTER_CRITICAL();
    for (uint8_t i = 0; i < v->size; i++) enc_byte(e, p[i]);
    LX_EXIT_CRITICAL();
}

/* HDR, then [DST SRC] in bus builds. The caller adds ID next. */
static inline void enc_head(frame_enc_t *e, const lx_t *h, uint8_t cmd, uint8_t seq
                            BUS_ADDR_PARAM(dst))
{
    enc_byte(e, MAKE_HDR(cmd, seq));
#if LX_BUS
    enc_byte(e, dst);
    enc_byte(e, h->addr);
#else
    (void)h;
#endif
}

/* Append the CRC (big-endian), close the last block and add the 0x00
 * delimiter. Returns the number of bytes to send. */
static inline uint16_t enc_end(frame_enc_t *e)
{
    uint16_t c = e->crc;
    /* Big-endian makes the CRC over the whole raw frame, CRC included, come
     * out as 0: that is how the decoder checks it without a second pass. */
    enc_cobs(e, (uint8_t)(c >> 8));
    enc_cobs(e, (uint8_t)c);
    *e->code_at = e->code;
    *e->out++ = 0x00u;
    return (uint16_t)(e->out - e->start);
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

int lx__check_vars(const lx_t *h, uint32_t mask)
{
    if (!mask || (mask & ~h->bound)) return LX_ERR_ID;

    unsigned payload;
    if (!several(mask)) {
        payload = h->vars[first_id(mask)].size;         /* single: just the value */
    } else {
        payload = 0;                                    /* batch: (id, len, value) each */
        for (uint32_t m = mask; m; m &= m - 1u) payload += 2u + h->vars[first_id(m)].size;
    }
    return payload > LX_MAX_PAYLOAD ? LX_ERR_SIZE : LX_OK;
}

/* Single:  HDR(cmd)     [DST SRC] ID        VALUE                  CRC
 * Batch:   HDR(cmd + 1) [DST SRC] BATCH_ID  (ID LEN VALUE) ...     CRC  */
uint16_t lx__build_vars(lx_t *h, uint8_t *buf, uint8_t cmd, uint8_t seq, uint32_t mask
                        BUS_ADDR_PARAM(dst))
{
    frame_enc_t e;
    enc_begin(&e, buf);
    if (!several(mask)) {
        uint8_t id = first_id(mask);
        enc_head(&e, h, cmd, seq BUS_ADDR_ARG(dst));
        enc_byte(&e, id);
        enc_var(&e, &h->vars[id]);
    } else {
        enc_head(&e, h, (uint8_t)(cmd + 1u), seq BUS_ADDR_ARG(dst));
        enc_byte(&e, BATCH_ID);
        for (uint32_t m = mask; m; m &= m - 1u) {
            uint8_t id = first_id(m);
            enc_byte(&e, id);
            enc_byte(&e, h->vars[id].size);
            enc_var(&e, &h->vars[id]);
        }
    }
    return enc_end(&e);
}

/* HDR [DST SRC] ID [REASON] CRC */
uint16_t lx__build_short(lx_t *h, uint8_t *buf, uint8_t cmd, uint8_t seq, uint8_t id,
                         uint8_t reason BUS_ADDR_PARAM(dst))
{
    frame_enc_t e;
    enc_begin(&e, buf);
    enc_head(&e, h, cmd, seq BUS_ADDR_ARG(dst));
    enc_byte(&e, id);
    if (reason) enc_byte(&e, reason);
    return enc_end(&e);
}

/* The request of the transaction in flight, into h->tx_buf:
 * a WRITE of the variables in mask, or a READ of the one id in mask. */
uint16_t lx__build_request(lx_t *h, uint8_t cmd, uint8_t seq, uint32_t mask BUS_ADDR_PARAM(dst))
{
    if (cmd == CMD_READ)
        return lx__build_short(h, h->tx_buf, CMD_READ, seq, first_id(mask), 0 BUS_ADDR_ARG(dst));
    return lx__build_vars(h, h->tx_buf, CMD_WRITE, seq, mask BUS_ADDR_ARG(dst));
}
