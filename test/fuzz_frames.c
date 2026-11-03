/* Structured fuzzer: random but CRC-valid frames into a node, mixed with
 * random app calls. The noise and bit-flip tests in test_lynx.c almost never
 * get past the CRC, so they cannot reach bugs in what a valid frame carries
 * (ids, lengths, batch layout, SEQ, addresses); this one does. It found the
 * READ_RSP shift past 31 (test [21]) in under a millisecond.
 *
 * Every step is one of: a frame from the peer (any command, SEQ, id, length;
 * biased toward plausible ones, sometimes split in two), lx_write / lx_read /
 * lx_publish with random arguments, a rebind of a random id, a clock jump.
 * One-context builds also start transactions from inside the event callback.
 * Variables are malloc'ed at their exact size, so ASan sees any overrun.
 *
 * Checks, besides ASan/UBSan: every transaction ends with exactly one event,
 * whose code is DONE/TIMEOUT/NACK_ID/NACK_SIZE and whose mask is the one the
 * transaction was started with; lx_busy() is true inside the callback; no new
 * transaction starts while busy; a rebind is refused exactly when the id is in
 * flight; update flags only name bound ids; every frame the node sends is valid
 * COBS with a good CRC and at most LX_ENC_MAX bytes; critical sections balance.
 *
 *   tools/run_tests.sh runs it for every configuration. By hand:
 *     cc -std=c99 -O1 -fsanitize=address,undefined -I. lynx.c test/fuzz_frames.c && ./a.out [steps] [seed]
 *   Coverage-guided (clang + libFuzzer):
 *     clang -DLIBFUZZER -fsanitize=fuzzer,address,undefined -I. lynx.c test/fuzz_frames.c && ./a.out
 *
 * MIT License (c) 2026 Ashwath */
#include "../lynx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LX_PORT   /* test/port/lynx_port.h: critical-section contract checker */
int lx_cs_depth, lx_cs_max; long lx_cs_count;
void lx_cs_exit_hook(void) {}
#endif

#define FAIL(...) do { printf("INVARIANT BROKEN: " __VA_ARGS__); printf("\n"); abort(); } while (0)

/* ---- byte source: the libFuzzer input, or xorshift64 ---- */
static const uint8_t *in_p; static size_t in_n; static int use_rand;
static unsigned long long rs = 88172645463325252ull;
static uint8_t rb(void)
{
    if (use_rand) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint8_t)(rs >> 24); }
    if (!in_n) return 0;
    in_n--; return *in_p++;
}
static uint32_t rb32(void)
{
    uint32_t v = rb(); v |= (uint32_t)rb() << 8; v |= (uint32_t)rb() << 16; return v | (uint32_t)rb() << 24;
}

/* ---- reference CRC-16/CCITT-FALSE and COBS, written independently of lynx.c ---- */
static uint16_t crc_ref(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) { c ^= (uint16_t)(*d++ << 8); for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}
/* raw frame -> COBS(raw + CRC) + 0x00 */
static size_t encode_frame(uint8_t *out, const uint8_t *raw, size_t n)
{
    uint8_t f[300]; size_t o = 1, code_at = 0; uint8_t code = 1;
    memcpy(f, raw, n);
    uint16_t c = crc_ref(raw, n); f[n++] = (uint8_t)(c >> 8); f[n++] = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        if (f[i]) { out[o++] = f[i]; if (++code == 0xFF) { out[code_at] = code; code_at = o++; code = 1; } }
        else      { out[code_at] = code; code_at = o++; code = 1; }
    }
    out[code_at] = code; out[o++] = 0;
    return o;
}
/* COBS body (no delimiters) -> raw bytes incl. CRC; -1 if malformed */
static int cobs_dec(uint8_t *raw, const uint8_t *in, size_t n)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (!code) return -1;
        for (uint8_t k = 1; k < code; k++) { if (i >= n) return -1; raw[o++] = in[i++]; }
        if (code != 0xFF && i < n) raw[o++] = 0;
    }
    return (int)o;
}

/* ---- the node under test ---- */
static lx_t A;
static uint8_t *var[LX_MAX_VARS], vsize[LX_MAX_VARS];
static uint32_t txn_mask, chain_next;
static int txn_open, txn_events;
static long n_events, n_sent, n_chained, n_rebinds;
#if LX_BUS
#define ME   0x01u
#define PEER 0x02u
#endif

static void on_ev(void *ctx, uint8_t ev, uint32_t mask)
{
    (void)ctx;
    if (ev < LX_EV_DONE || ev > LX_EV_NACK_SIZE) FAIL("event code %u", ev);
    if (!txn_open) FAIL("event %u with no transaction", ev);
    if (++txn_events > 1) FAIL("second event (%u) for one transaction", ev);
    if (mask != txn_mask) FAIL("event mask %08lx, transaction %08lx", (unsigned long)mask, (unsigned long)txn_mask);
    if (!lx_busy(&A)) FAIL("lx_busy() false inside the callback");
    n_events++;
#if LX_ONE_CONTEXT
    if (chain_next) {                       /* start the next transaction from inside the event */
        uint32_t m = chain_next; chain_next = 0;
        if (lx_write(&A, m) == LX_OK) { txn_events = 0; txn_mask = m; n_chained++; }
    }
#endif
}

static void tx_check(void *ctx, const uint8_t *d, uint16_t len)
{
    (void)ctx;
    size_t s = LX_LEAD_DELIM ? 1u : 0u;
    uint8_t raw[300];
    if (len > LX_ENC_MAX) FAIL("sent %u bytes, LX_ENC_MAX is %u", len, (unsigned)LX_ENC_MAX);
    if (len < s + 2u || (s && d[0]) || d[len - 1]) FAIL("frame delimiters");
    for (size_t i = s; i + 1u < len; i++) if (!d[i]) FAIL("0x00 inside a COBS body");
    int n = cobs_dec(raw, d + s, len - 1u - s);
    if (n < (int)(4 + 2 * LX_BUS)) FAIL("sent a malformed or short frame");
    if (crc_ref(raw, (size_t)n - 2u) != (uint16_t)(raw[n - 2] << 8 | raw[n - 1])) FAIL("sent a bad CRC");
    n_sent++;
}

static uint8_t rand_size(void)
{
    return (uint8_t)((rb() & 7) ? 1u + rb() % 8u : 1u + rb() % LX_MAX_PAYLOAD);
}

static void reset_node(void)
{
    for (unsigned i = 0; i < LX_MAX_VARS; i++) { free(var[i]); var[i] = NULL; vsize[i] = 0; }
    lx_init(&A, tx_check, on_ev, NULL);
#if LX_BUS
    lx_set_addr(&A, ME, PEER);
#endif
    uint32_t want = rb32();
    for (uint8_t id = 0; id < LX_MAX_VARS; id++) {
        if (!(want & LX_BIT(id))) continue;
        vsize[id] = rand_size();
        var[id] = calloc(1, vsize[id]);
        if (lx_bind(&A, id, var[id], vsize[id]) != LX_OK) FAIL("lx_bind at setup");
    }
    txn_open = 0; chain_next = 0;
}

static uint8_t rand_id(void)
{
    uint8_t r = rb();
    return r < 160 ? (uint8_t)(r % 40u) : r < 200 ? 0xFFu : rb();
}

/* A random frame from the peer, biased toward ones that get past the cheap checks. */
static size_t make_frame(uint8_t *f)
{
    size_t n = 0;
    uint8_t cmd = (rb() & 15) ? (uint8_t)(rb() % 10u) : (uint8_t)(rb() & 15u);
    uint8_t seq = (rb() & 1) ? A.p_seq : (uint8_t)(rb() & 15u);
    f[n++] = (uint8_t)(cmd << 4 | seq);
#if LX_BUS
    uint8_t r = rb();
    f[n++] = r < 160 ? ME : r < 220 ? 0xFFu : rb();
    r = rb();
    f[n++] = r < 160 ? PEER : r < 200 ? ME : r < 220 ? 0xFFu : rb();
#endif
    uint8_t id = rand_id();
    f[n++] = id;
    switch (rb() & 3) {
    case 0:                                         /* the right size for the id, if bound */
        if (id < LX_MAX_VARS) for (uint8_t i = 0; i < vsize[id]; i++) f[n++] = rb();
        break;
    case 1: {                                       /* batch-shaped: (id, len, bytes)... */
        size_t left = LX_MAX_PAYLOAD;
        while (left >= 2 && (rb() & 3)) {
            uint8_t vid = rand_id();
            uint8_t vl = ((rb() & 3) && vid < LX_MAX_VARS && vsize[vid]) ? vsize[vid] : rb();
            if (!(rb() & 7)) vl = (uint8_t)(vl + 1u);
            f[n++] = vid; f[n++] = vl; left -= 2;
            size_t k = vl < left ? vl : left;
            if (k && !(rb() & 7)) k--;              /* sometimes short */
            for (size_t i = 0; i < k; i++) f[n++] = rb();
            left -= k;
        }
        break;
    }
    case 2:                                         /* NACK-shaped */
        f[n++] = (rb() & 1) ? (uint8_t)(LX_EV_NACK_ID + (rb() & 1)) : rb();
        if (!(rb() & 3)) f[n++] = rb();
        break;
    default: {                                      /* any length */
        size_t k = rb() % (LX_MAX_PAYLOAD + 2u);
        if (k > LX_MAX_PAYLOAD) k = LX_MAX_PAYLOAD;
        for (size_t i = 0; i < k; i++) f[n++] = rb();
    }
    }
    return n;
}

static void feed(const uint8_t *d, size_t n)
{
#if LX_ISR_SAFE    /* a queue's worth between polls, as an ISR + main loop would; now and then overflow it */
    size_t chunk = (rb() & 15) ? LX_RXQ_SIZE - 1u : n;
    while (n) {
        size_t k = n < chunk ? n : chunk;
        lx_rx_buf(&A, d, k); lx_poll(&A); d += k; n -= k;
    }
#else
    lx_rx_buf(&A, d, n);
#endif
}

static uint32_t now;
static void step(void)
{
    uint8_t op = (uint8_t)(rb() % 16u);
    if (op < 9) {
        uint8_t f[300], out[320];
        size_t m = encode_frame(out, f, make_frame(f));
        if (rb() & 15) feed(out, m);
        else { size_t cut = rb() % m; feed(out, cut); feed(out + cut, m - cut); }
    } else if (op < 12) {                           /* start a transaction */
        bool was_busy = lx_busy(&A);
        uint32_t mask;
        int rc;
        if (rb() & 1) {
            uint8_t id = rand_id();
            mask = id < 32 ? LX_BIT(id) : 0;
            rc = lx_read(&A, id);
        } else {
            mask = rb32();
            if (rb() & 1) mask &= A.bound;
            if ((rb() & 3) && mask) mask &= ~(mask - 1u);      /* mostly a single variable */
            rc = lx_write(&A, mask);
        }
        if (rc == LX_OK) {
            if (was_busy) FAIL("transaction started while busy");
            txn_open = 1; txn_events = 0; txn_mask = mask;
        }
    } else if (op == 12 && (rb() & 1)) {            /* rebind a random id */
        uint8_t id = (uint8_t)(rb() % LX_MAX_VARS), sz = rand_size();
        uint8_t *buf = calloc(1, sz);
        bool in_flight = lx_busy(&A) && (A.p_mask & LX_BIT(id));
        int rc = lx_bind(&A, id, buf, sz);
        if (rc == LX_OK && !in_flight) { free(var[id]); var[id] = buf; vsize[id] = sz; n_rebinds++; }
        else if (rc == LX_ERR_BUSY && in_flight) free(buf);
        else FAIL("lx_bind returned %d, id %s in flight", rc, in_flight ? "" : "not");
    } else if (op == 12) {                          /* the next event chains this write */
        chain_next = A.bound & rb32();
        if (chain_next && (rb() & 3)) chain_next &= ~(chain_next - 1u);
    } else if (op < 14) {
        (void)lx_publish(&A, rb32() & 0xFFFFu);
    } else {
        now += rb() % (2u * LX_TIMEOUT_MS);
        lx_tick(&A, now);
    }
    if (txn_open && !lx_busy(&A)) {
        if (txn_events != 1) FAIL("transaction ended with %d events", txn_events);
        txn_open = 0;
    }
    if (lx_take_updates(&A) & ~A.bound) FAIL("update flag for an unbound id");
#ifdef LX_PORT
    if (lx_cs_depth != 0 || lx_cs_max > 1) FAIL("critical sections: depth %d, max %d", lx_cs_depth, lx_cs_max);
#endif
}

#ifdef LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    in_p = d; in_n = n; use_rand = 0;
    reset_node();
    while (in_n) step();
    return 0;
}
#else
int main(int argc, char **argv)
{
    long steps = argc > 1 ? atol(argv[1]) : 300000;
    if (argc > 2) rs = strtoull(argv[2], NULL, 0) | 1u;
    use_rand = 1;
    for (long k = 0; k < steps; k++) {
        if (k % 5000 == 0) reset_node();            /* new bindings now and then */
        step();
    }
    printf("     fuzz: %ld CRC-valid steps, %ld events (%ld chained), %ld frames sent, %ld rebinds: invariants held\n",
           steps, n_events, n_chained, n_sent, n_rebinds);
    return 0;
}
#endif
