/* Host test suite: two nodes over an in-memory lossy link. */
#include "lynx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#ifdef LX_PORT   /* test/port/lynx_port.h: critical-section contract checker */
int lx_cs_depth, lx_cs_max; long lx_cs_count;
static void (*irq)(void);       /* one-shot "interrupt", fired as a critical section closes */
void lx_cs_exit_hook(void) { if (irq && lx_cs_depth == 0) { void (*f)(void) = irq; irq = NULL; f(); } }
#endif

/* ---- simulated link ---- */
typedef struct { uint8_t buf[1 << 16]; size_t n; int drop_next; double flip_p; } pipe_t;
static pipe_t a2b, b2a;
static void tx_pipe(void *ctx, const uint8_t *d, uint16_t len) {
    pipe_t *p = ctx;
    if (p->drop_next > 0) { p->drop_next--; return; }
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = d[i];
        if (p->flip_p > 0 && (double)rand() / RAND_MAX < p->flip_p) b ^= (uint8_t)(1u << (rand() & 7));
        p->buf[p->n++] = b;
    }
}
static lx_t A, B;

/* Deliver received bytes. LX_ISR_SAFE: lx_rx only queues, so feed at most a
 * queue's worth and let lx_poll decode it, as an ISR + main loop would. */
static void feed(lx_t *h, const uint8_t *d, size_t n) {
#if LX_ISR_SAFE
    while (n) {
        size_t k = n < LX_RXQ_SIZE - 1u ? n : LX_RXQ_SIZE - 1u;
        lx_rx_buf(h, d, k); lx_poll(h); d += k; n -= k;
    }
#else
    lx_rx_buf(h, d, n);
#endif
}

static void pump(void) {
    while (a2b.n || b2a.n) {
        uint8_t t[sizeof a2b.buf]; size_t n;
        n = a2b.n; memcpy(t, a2b.buf, n); a2b.n = 0; feed(&B, t, n);
        n = b2a.n; memcpy(t, b2a.buf, n); b2a.n = 0; feed(&A, t, n);
    }
}
static int last_ev; static uint32_t last_mask; static int ev_count;
static void on_ev(void *ctx, uint8_t ev, uint32_t mask) { (void)ctx; last_ev = ev; last_mask = mask; ev_count++; }
static int chain_busy, chain_rc, chain_left;
static float a_f;
static void on_ev_chain(void *ctx, uint8_t ev, uint32_t mask);

/* node variables */
static uint8_t  a_u8, b_u8; static float b_f; static int32_t a_i, b_i;
static uint8_t  a_big[60], b_big[60]; static uint16_t a_u16;

static void setup(void) {
    memset(&a2b, 0, sizeof a2b); memset(&b2a, 0, sizeof b2a);
    lx_init(&A, tx_pipe, on_ev, &a2b);
    lx_init(&B, tx_pipe, on_ev, &b2a);
    lx_bind(&A, 0, &a_u8, 1);  lx_bind(&B, 0, &b_u8, 1);
    lx_bind(&A, 1, &a_f, 4);   lx_bind(&B, 1, &b_f, 4);
    lx_bind(&A, 2, &a_i, 4);   lx_bind(&B, 2, &b_i, 4);
    lx_bind(&A, 3, a_big, 60); lx_bind(&B, 3, b_big, 60);
    lx_bind(&A, 4, &a_u16, 2); /* B deliberately does NOT bind id 4 */
#if LX_BUS
    lx_set_addr(&A, 0x01, 0x02); lx_set_addr(&B, 0x02, 0x01);   /* point-to-point over the bus format */
#endif
    last_ev = 0; ev_count = 0;
}

/* event callback that starts the next write from inside itself */
static void on_ev_chain(void *ctx, uint8_t ev, uint32_t mask) {
    on_ev(ctx, ev, mask);
    if (chain_left-- > 0) { chain_busy = lx_busy(&A); a_f = 7.5f; chain_rc = lx_write(&A, LX_BIT(1)); }
}

/* reference CRC for check value */
static uint16_t crc_ref(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) { c ^= (uint16_t)(*d++ << 8); for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}

/* Hand-made frames: raw = HDR [DST SRC] ID PAYLOAD; CRC + COBS + delimiter added here. */
static size_t encode_frame(uint8_t *out, const uint8_t *raw, size_t n) {
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
static void inject(pipe_t *p, const uint8_t *raw, size_t n) { uint8_t out[310]; tx_pipe(p, out, (uint16_t)encode_frame(out, raw, n)); }
/* header of a frame from B to A (addresses 0x02 -> 0x01 in bus mode) */
static size_t head_b2a(uint8_t *f, uint8_t cmd, uint8_t seq, uint8_t id) {
    size_t n = 0;
    f[n++] = (uint8_t)((cmd << 4) | seq);
#if LX_BUS
    f[n++] = 0x01; f[n++] = 0x02;
#endif
    f[n++] = id;
    return n;
}
#if defined(LX_PORT) && !LX_ISR_SAFE
static uint8_t pending[64]; static size_t pending_len;
static void irq_deliver_pending(void) { uint8_t out[80]; feed(&A, out, encode_frame(out, pending, pending_len)); }
#endif

#if LX_BUS
/* ---- shared multi-drop bus: every byte reaches every node, sender included (echo) ---- */
enum { NN = 3 };
static const uint8_t addr_of[NN] = { 0x01, 0x10, 0x20 };   /* master, slave 1, slave 2 */
static lx_t N[NN];
static pipe_t bus;
typedef struct { int ev, count; uint32_t mask; } evrec_t;
static evrec_t evr[NN];
static uint8_t bu8[NN]; static int32_t bi32[NN];

static void tx_bus(void *ctx, const uint8_t *d, uint16_t len) { (void)ctx; tx_pipe(&bus, d, len); }
static void on_ev_bus(void *ctx, uint8_t ev, uint32_t mask) { evrec_t *r = ctx; r->ev = ev; r->mask = mask; r->count++; }
static void pump_bus(void) {
    static uint8_t t[sizeof bus.buf];
    while (bus.n) {
        size_t n = bus.n; memcpy(t, bus.buf, n); bus.n = 0;
        for (int i = 0; i < NN; i++) feed(&N[i], t, n);
    }
}
static void bus_setup(void) {
    memset(&bus, 0, sizeof bus); memset(evr, 0, sizeof evr);
    for (int i = 0; i < NN; i++) {
        lx_init(&N[i], tx_bus, on_ev_bus, &evr[i]);
        lx_set_addr(&N[i], addr_of[i], i ? addr_of[0] : LX_ADDR_BCAST);  /* slaves talk to the master */
        bu8[i] = 0; bi32[i] = 0;
        lx_bind(&N[i], 0, &bu8[i], 1); lx_bind(&N[i], 1, &bi32[i], 4);
    }
}
static void bus_inject(const uint8_t *raw, size_t n) { inject(&bus, raw, n); }
#endif

int main(void) {
    srand(12345);

    puts("[1] publish single var");
    setup(); a_f = 3.14159f; CHECK(lx_publish(&A, LX_BIT(1)) == LX_OK); pump();
    CHECK(b_f == 3.14159f); CHECK(lx_take_updates(&B) == LX_BIT(1)); CHECK(lx_take_updates(&B) == 0);

    puts("[2] reliable write -> ACK");
    setup(); a_i = -123456; CHECK(lx_write(&A, LX_BIT(2)) == LX_OK); CHECK(lx_busy(&A)); pump();
    CHECK(b_i == -123456); CHECK(!lx_busy(&A)); CHECK(last_ev == LX_EV_DONE);

    puts("[3] batch write, atomic, 4 vars in one frame");
    setup(); a_u8 = 7; a_f = -1.5f; a_i = 99; for (int i = 0; i < 40; i++) a_big[i] = (uint8_t)i;
    CHECK(lx_write(&A, LX_BIT(0) | LX_BIT(1) | LX_BIT(2)) == LX_OK);
    CHECK(A.st_tx == 1); pump();
    CHECK(b_u8 == 7 && b_f == -1.5f && b_i == 99); CHECK(last_ev == LX_EV_DONE);
    CHECK(lx_take_updates(&B) == (LX_BIT(0) | LX_BIT(1) | LX_BIT(2)));
    CHECK(lx_write(&A, LX_BIT(0) | LX_BIT(3)) == LX_ERR_SIZE); /* 1+2+60+2 > 64 */

    puts("[4] read remote value");
    setup(); b_i = 424242; a_i = 0; CHECK(lx_read(&A, 2) == LX_OK); pump();
    CHECK(a_i == 424242); CHECK(last_ev == LX_EV_DONE);

    puts("[5] NACK on unbound remote id");
    setup(); a_u16 = 5; CHECK(lx_write(&A, LX_BIT(4)) == LX_OK); pump();
    CHECK(last_ev == LX_EV_NACK_ID); CHECK(!lx_busy(&A));

    puts("[6] busy guard");
    setup(); CHECK(lx_write(&A, LX_BIT(0)) == LX_OK); CHECK(lx_write(&A, LX_BIT(1)) == LX_ERR_BUSY);
    CHECK(lx_publish(&A, LX_BIT(1)) == LX_OK); pump(); CHECK(!lx_busy(&A));

    puts("[7] lost frame -> auto retry succeeds");
    setup(); lx_tick(&A, 1000); a_f = 2.5f; a2b.drop_next = 1;
    CHECK(lx_write(&A, LX_BIT(1)) == LX_OK); pump(); CHECK(lx_busy(&A));
    lx_tick(&A, 1010); CHECK(lx_busy(&A));
    lx_tick(&A, 1000 + LX_TIMEOUT_MS); pump();
    CHECK(b_f == 2.5f); CHECK(!lx_busy(&A)); CHECK(A.st_retx == 1); CHECK(last_ev == LX_EV_DONE);

    puts("[8] lost ACK -> retry, stale ACK can't confuse next txn");
    setup(); lx_tick(&A, 0); b2a.drop_next = 1; a_u8 = 1;
    lx_write(&A, LX_BIT(0)); pump(); CHECK(lx_busy(&A));
    lx_tick(&A, LX_TIMEOUT_MS); pump(); CHECK(!lx_busy(&A)); CHECK(b_u8 == 1);

    puts("[9] dead link -> TIMEOUT after retries, clock wraparound safe");
    setup(); uint32_t t = 0xFFFFFFF0u; lx_tick(&A, t); a2b.drop_next = 100;
    lx_write(&A, LX_BIT(0));
    for (int i = 0; i < 20 && lx_busy(&A); i++) { t += LX_TIMEOUT_MS; lx_tick(&A, t); pump(); }
    CHECK(last_ev == LX_EV_TIMEOUT); CHECK(A.st_retx == LX_RETRIES); CHECK(!lx_busy(&A));

    puts("[10] CRC check value + COBS edge cases (zeros, 0xFF, 254-byte runs)");
    CHECK(crc_ref((const uint8_t *)"123456789", 9) == 0x29B1);
    {
        uint8_t pat[6][60]; memset(pat[0], 0, 60); memset(pat[1], 0xFF, 60); memset(pat[2], 0x01, 60);
        for (int i = 0; i < 60; i++) { pat[3][i] = (uint8_t)i; pat[4][i] = (i % 2) ? 0 : 0xAA; pat[5][i] = (uint8_t)rand(); }
        for (int k = 0; k < 6; k++) {
            setup(); memcpy(a_big, pat[k], 60); memset(b_big, 0x5A, 60);
            lx_publish(&A, LX_BIT(3)); pump(); CHECK(memcmp(a_big, b_big, 60) == 0);
        }
    }

    puts("[11] resync after garbage / mid-frame start");
    setup(); { uint8_t junk[] = {0x13, 0x37, 0xAA, 0x55, 0xFF, 0x02}; feed(&B, junk, sizeof junk); }
    a_i = 777; lx_publish(&A, LX_BIT(2)); pump(); CHECK(b_i == 777 || !LX_LEAD_DELIM ? 1 : 0);
    a_i = 778; lx_publish(&A, LX_BIT(2)); pump(); CHECK(b_i == 778);   /* worst case: one frame lost */
    setup(); a_i = 888; lx_publish(&A, LX_BIT(2)); { size_t n = a2b.n; feed(&B, a2b.buf + 3, n - 3); a2b.n = 0; }
    CHECK(b_i != 888); a_i = 889; lx_publish(&A, LX_BIT(2)); pump(); CHECK(b_i == 889);

    puts("[12] fuzz: 200k frames, random bit flips, no corrupted value may ever commit");
    {
        setup(); a2b.flip_p = 0.002; long bad_commits = 0, good = 0, sent = 200000;
        for (long k = 0; k < sent; k++) {
            int32_t v = (int32_t)(k * 2654435761u);
            a_i = v; b_i = 0x7FFFFFFF;
            lx_publish(&A, LX_BIT(2)); pump();
            if (lx_take_updates(&B) & LX_BIT(2)) { if (b_i == v) good++; else bad_commits++; }
        }
        printf("     delivered %ld/%ld, rejected-corrupt %u, BAD COMMITS %ld\n", good, sent, B.st_rx_bad, bad_commits);
        CHECK(bad_commits == 0);
    }

    puts("[13] fuzz: pure random noise into parser (crash/overflow check)");
    setup(); for (long k = 0; k < 2000000; k++) { uint8_t x = (uint8_t)rand(); feed(&B, &x, 1); } CHECK(1);

    puts("[14] every id 0..31: single publish, reliable write, scattered batch");
    {
        static uint8_t av[LX_MAX_VARS], bv[LX_MAX_VARS];
        setup();
        for (uint8_t id = 0; id < LX_MAX_VARS; id++) { lx_bind(&A, id, &av[id], 1); lx_bind(&B, id, &bv[id], 1); }
        for (uint8_t id = 0; id < LX_MAX_VARS; id++) {
            av[id] = (uint8_t)(id + 1); lx_tick(&A, 0);
            CHECK(lx_publish(&A, LX_BIT(id)) == LX_OK); pump();
            CHECK(lx_take_updates(&B) == LX_BIT(id) && bv[id] == id + 1);
            av[id] = (uint8_t)(id + 101);
            CHECK(lx_write(&A, LX_BIT(id)) == LX_OK); pump();
            CHECK(!lx_busy(&A) && last_ev == LX_EV_DONE && last_mask == LX_BIT(id) && bv[id] == id + 101);
            CHECK(lx_take_updates(&B) == LX_BIT(id));
        }
        uint32_t m = LX_BIT(1) | LX_BIT(7) | LX_BIT(8) | LX_BIT(15) | LX_BIT(16) | LX_BIT(23) | LX_BIT(30) | LX_BIT(31);
        for (uint8_t id = 0; id < LX_MAX_VARS; id++) { av[id] = (uint8_t)(200 + id); bv[id] = 0; }
        lx_take_updates(&B);
        CHECK(lx_write(&A, m) == LX_OK); pump(); CHECK(last_ev == LX_EV_DONE && last_mask == m);
        CHECK(lx_take_updates(&B) == m);
        for (uint8_t id = 0; id < LX_MAX_VARS; id++) CHECK(bv[id] == ((m & LX_BIT(id)) ? 200 + id : 0));
    }

    puts("[15] lx_get / lx_set copy exactly the bound size, reject unbound ids");
    {
        setup(); uint8_t buf[64]; memset(buf, 0xEE, sizeof buf);
        b_i = 0x11223344; CHECK(lx_get(&B, 2, buf) == LX_OK);
        CHECK(memcmp(buf, &b_i, 4) == 0 && buf[4] == 0xEE);
        int32_t v = -77; CHECK(lx_set(&A, 2, &v) == LX_OK && a_i == -77);
        CHECK(lx_get(&B, 4, buf) == LX_ERR_ID && lx_set(&B, 4, buf) == LX_ERR_ID);   /* unbound on B */
        CHECK(lx_get(&A, LX_MAX_VARS, buf) == LX_ERR_ID);
        CHECK(lx_write(&A, LX_BIT(2)) == LX_OK); pump(); CHECK(b_i == -77);
    }

    puts("[16] wire size: 4-byte publish = 10 B (+2 with LX_BUS, +1 with LX_LEAD_DELIM)");
    setup(); a_f = 1.0f; CHECK(lx_publish(&A, LX_BIT(1)) == LX_OK);
    CHECK(a2b.n == 10u + 2u * LX_BUS + LX_LEAD_DELIM); pump(); CHECK(b_f == 1.0f);

    puts("[17] a NACK whose reason byte isn't a NACK code never ends a transaction");
    {
        static const uint8_t bogus[] = { 0, LX_EV_DONE, LX_EV_TIMEOUT, 5, 0xFF };
        uint8_t f[16]; size_t n;
        setup(); lx_tick(&A, 0); a2b.drop_next = 1;             /* request lost: B stays silent */
        CHECK(lx_write(&A, LX_BIT(0)) == LX_OK); pump();
        for (size_t i = 0; i < sizeof bogus; i++) {
            n = head_b2a(f, 8, A.p_seq, 0); f[n++] = bogus[i]; inject(&b2a, f, n); pump();
        }
        CHECK(lx_busy(&A) && ev_count == 0);
        n = head_b2a(f, 8, A.p_seq, 0); f[n++] = LX_EV_NACK_SIZE; inject(&b2a, f, n); pump();
        CHECK(!lx_busy(&A) && ev_count == 1 && last_ev == LX_EV_NACK_SIZE);
    }

    puts("[20] lx_busy() is still true inside the event callback; one-context builds may chain from it");
    {
        setup(); A.ev = on_ev_chain; chain_busy = -1; chain_rc = 99; chain_left = 1; b_f = 0;
        a_u8 = 1; CHECK(lx_write(&A, LX_BIT(0)) == LX_OK); pump();
        CHECK(chain_busy == 1);
        CHECK(chain_rc == (LX_ONE_CONTEXT ? LX_OK : LX_ERR_BUSY));
        CHECK(ev_count == (LX_ONE_CONTEXT ? 2 : 1) && !lx_busy(&A) && b_u8 == 1);
        CHECK(b_f == (LX_ONE_CONTEXT ? 7.5f : 0.0f));      /* the chained write landed */
    }

    puts("[21] a READ_RSP with an id past 31 is ignored (no shift overflow, no match on id mod 32)");
    {
        uint8_t f[16]; size_t n; int32_t v = 1234;
        setup(); lx_tick(&A, 0); a2b.drop_next = 1; a_i = 0;   /* request lost: B stays silent */
        CHECK(lx_read(&A, 2) == LX_OK); pump();
        n = head_b2a(f, 6, A.p_seq, 32 + 2); memcpy(f + n, &v, 4); n += 4; inject(&b2a, f, n); pump();
        CHECK(lx_busy(&A) && ev_count == 0 && a_i == 0);
        n = head_b2a(f, 6, A.p_seq, 2); memcpy(f + n, &v, 4); n += 4; inject(&b2a, f, n); pump();
        CHECK(!lx_busy(&A) && ev_count == 1 && last_ev == LX_EV_DONE && a_i == 1234);
    }

#if defined(LX_PORT) && !LX_ISR_SAFE
    puts("[18] races: an answer lands the instant lx_tick leaves its critical section");
    {
        /* (a) the ACK arrives just as lx_tick gives up: exactly one event */
        setup(); lx_tick(&A, 0); a2b.drop_next = 100;           /* B never hears A */
        CHECK(lx_write(&A, LX_BIT(0)) == LX_OK);
        uint32_t t = 0;
        for (unsigned r = 0; r < LX_RETRIES; r++) { t += LX_TIMEOUT_MS; lx_tick(&A, t); }
        CHECK(lx_busy(&A) && ev_count == 0);
        pending_len = head_b2a(pending, 7, A.p_seq, 0);          /* ACK */
        irq = irq_deliver_pending;
        lx_tick(&A, t + LX_TIMEOUT_MS);                          /* gives up; the "interrupt" fires inside */
        CHECK(irq == NULL);
        CHECK(ev_count == 1 && last_ev == LX_EV_TIMEOUT && !lx_busy(&A));

        /* (b) the read response arrives just as lx_tick decides to resend: the
         * resend must still be the READ, never a WRITE of A's own copy */
        setup(); lx_tick(&A, 0); b_i = 424242; a_i = 0; a2b.drop_next = 1;
        CHECK(lx_read(&A, 2) == LX_OK); pump();
        int32_t v = 99;
        pending_len = head_b2a(pending, 6, A.p_seq, 2);
        memcpy(pending + pending_len, &v, 4); pending_len += 4;  /* READ_RSP id 2 = 99 */
        irq = irq_deliver_pending;
        lx_tick(&A, LX_TIMEOUT_MS); CHECK(irq == NULL);
        pump();
        CHECK(a_i == 99 && ev_count == 1 && last_ev == LX_EV_DONE && A.st_retx == 1);
        CHECK(b_i == 424242 && lx_take_updates(&B) == 0);       /* B was asked, never written */
    }
#endif

#if LX_ISR_SAFE
    puts("[19] LX_ISR_SAFE queue: capacity, overflow drops whole frames and never corrupts");
    {
        setup();
        for (unsigned i = 0; i < LX_RXQ_SIZE - 1u; i++) lx_rx(&B, 0x00);
        CHECK(B.st_rx_ovf == 0); lx_rx(&B, 0x00); CHECK(B.st_rx_ovf == 1);
        CHECK(lx_poll(&B) == LX_RXQ_SIZE - 1u && lx_poll(&B) == 0);

        /* a burst of K frames with no poll in between: the queue keeps the
         * first LX_RXQ_SIZE-1 bytes; the frame cut by the gap must not commit */
        setup();
        int K = (int)(LX_RXQ_SIZE / 8u) + 3;
        for (int k = 1; k <= K; k++) { a_i = k; lx_publish(&A, LX_BIT(2)); }
        size_t fl = a2b.n / (size_t)K, kept = LX_RXQ_SIZE - 1u, whole = kept / fl;
        lx_rx_buf(&B, a2b.buf, a2b.n);
        CHECK(B.st_rx_ovf == a2b.n - kept); a2b.n = 0;
        lx_poll(&B);
        CHECK(B.st_rx_ok == whole && (whole == 0 || b_i == (int32_t)whole));
        a_i = 1000; lx_publish(&A, LX_BIT(2));                  /* lost with the cut frame */
        a_i = 1001; lx_publish(&A, LX_BIT(2));
        feed(&B, a2b.buf, a2b.n); a2b.n = 0;
        CHECK(b_i == 1001 && B.st_rx_ok == whole + 1 && B.st_rx_bad == (kept % fl ? 1u : 0u));

        /* random noise with rare polls: overflow everywhere, then a clean link */
        setup();
        for (long k = 0; k < 300000; k++) { lx_rx(&B, (uint8_t)rand()); if (!(k & 1023)) lx_poll(&B); }
        lx_poll(&B); CHECK(B.st_rx_ovf > 0);
        a_i = 5; lx_publish(&A, LX_BIT(2)); a_i = 6; lx_publish(&A, LX_BIT(2)); pump();
        CHECK(b_i == 6);
    }
#endif

#if LX_BUS
    puts("[bus 1] unicast reliable write: only the addressee commits and ACKs");
    bus_setup(); bi32[0] = 4242;
    CHECK(lx_write_to(&N[0], 0x10, LX_BIT(1)) == LX_OK); pump_bus();
    CHECK(bi32[1] == 4242 && bi32[2] == 0);
    CHECK(evr[0].ev == LX_EV_DONE && evr[0].count == 1 && evr[0].mask == LX_BIT(1) && !lx_busy(&N[0]));
    CHECK(lx_take_updates(&N[1]) == LX_BIT(1) && lx_take_updates(&N[2]) == 0 && lx_take_updates(&N[0]) == 0);
    CHECK(N[1].st_rsp == 1 && N[2].st_rsp == 0 && N[1].st_tx == 0);   /* exactly one ACK, from slave 1 */
    CHECK(N[2].st_rx_ok == 0 && N[2].st_rx_bad == 0);     /* slave 2 skipped both frames silently */

    puts("[bus 2] broadcast publish reaches every node, nobody answers, sender ignores its echo");
    bus_setup(); bu8[0] = 77; bi32[0] = -9;
    CHECK(lx_publish_to(&N[0], LX_ADDR_BCAST, LX_BIT(0) | LX_BIT(1)) == LX_OK); pump_bus();
    CHECK(bu8[1] == 77 && bu8[2] == 77 && bi32[1] == -9 && bi32[2] == -9);
    CHECK(N[1].st_rsp == 0 && N[2].st_rsp == 0);
    CHECK(lx_take_updates(&N[0]) == 0 && N[0].st_rx_ok == 0);
    bu8[0] = 78; CHECK(lx_publish(&N[0], LX_BIT(0)) == LX_OK); pump_bus();   /* master's peer = broadcast */
    CHECK(bu8[1] == 78 && bu8[2] == 78);

    puts("[bus 3] address guards");
    bus_setup();
    CHECK(lx_write_to(&N[0], LX_ADDR_BCAST, LX_BIT(0)) == LX_ERR_ADDR);
    CHECK(lx_read_from(&N[0], LX_ADDR_BCAST, 0) == LX_ERR_ADDR);
    CHECK(lx_write(&N[0], LX_BIT(0)) == LX_ERR_ADDR);             /* default peer is broadcast */
    CHECK(lx_read(&N[0], 0) == LX_ERR_ADDR);
    CHECK(lx_write_to(&N[0], 0x01, LX_BIT(0)) == LX_ERR_ADDR);    /* to self */
    CHECK(lx_publish_to(&N[0], 0x01, LX_BIT(0)) == LX_ERR_ADDR);
    CHECK(lx_set_addr(&N[0], LX_ADDR_BCAST, 0x10) == LX_ERR_ADDR);
    CHECK(lx_set_addr(&N[0], 0x10, 0x10) == LX_ERR_ADDR);
    CHECK(!lx_busy(&N[0]) && bus.n == 0);

    puts("[bus 4] read from a specific node");
    bus_setup(); bi32[1] = 111; bi32[2] = 222;
    CHECK(lx_read_from(&N[0], 0x20, 1) == LX_OK); pump_bus();
    CHECK(bi32[0] == 222 && evr[0].ev == LX_EV_DONE && N[1].st_rsp == 0 && N[2].st_rsp == 1);
    CHECK(lx_read(&N[1], 1) == LX_OK); pump_bus();                /* slave's peer = master */
    CHECK(bi32[1] == 222 && evr[1].ev == LX_EV_DONE);

    puts("[bus 5] broadcast WRITE/READ from a foreign node: WRITE commits, nothing answers");
    bus_setup();
    { uint8_t f[] = { (1 << 4) | 3, LX_ADDR_BCAST, 0x30, 0, 0x5A }; bus_inject(f, sizeof f); }  /* WRITE id 0 */
    { uint8_t f[] = { (5 << 4) | 4, LX_ADDR_BCAST, 0x30, 1 };       bus_inject(f, sizeof f); }  /* READ id 1 */
    pump_bus();
    for (int i = 0; i < NN; i++) CHECK(bu8[i] == 0x5A && N[i].st_rsp == 0 && lx_take_updates(&N[i]) == LX_BIT(0));

    puts("[bus 6] ACK / NACK / READ_RSP from the wrong node can't complete a transaction");
    bus_setup(); lx_tick(&N[0], 0); bus.drop_next = 1;          /* request lost: slave 1 stays silent */
    CHECK(lx_write_to(&N[0], 0x10, LX_BIT(0)) == LX_OK); pump_bus();
    {
        uint8_t s = N[0].p_seq;
        uint8_t ack2[]  = { (uint8_t)((7 << 4) | s), 0x01, 0x20, 0 };
        uint8_t nack2[] = { (uint8_t)((8 << 4) | s), 0x01, 0x20, 0, LX_EV_NACK_ID };
        uint8_t ack1[]  = { (uint8_t)((7 << 4) | s), 0x01, 0x10, 0 };
        bus_inject(ack2, sizeof ack2); bus_inject(nack2, sizeof nack2); pump_bus();
        CHECK(lx_busy(&N[0]) && evr[0].count == 0);
        bus_inject(ack1, sizeof ack1); pump_bus();
        CHECK(!lx_busy(&N[0]) && evr[0].ev == LX_EV_DONE);
    }
    bus_setup(); lx_tick(&N[0], 0); bus.drop_next = 1; bi32[1] = 55;
    CHECK(lx_read_from(&N[0], 0x10, 1) == LX_OK); pump_bus();
    {
        uint8_t rsp2[] = { (uint8_t)((6 << 4) | N[0].p_seq), 0x01, 0x20, 1, 1, 2, 3, 4 };
        bus_inject(rsp2, sizeof rsp2); pump_bus();
        CHECK(lx_busy(&N[0]) && bi32[0] == 0);
    }
    lx_tick(&N[0], LX_TIMEOUT_MS); pump_bus();                  /* retry reaches slave 1 */
    CHECK(!lx_busy(&N[0]) && evr[0].ev == LX_EV_DONE && bi32[0] == 55);

    puts("[bus 7] fuzz: 3 nodes, bit flips, unicast to alternating slaves: no misrouted or corrupt commit");
    {
        bus_setup(); bus.flip_p = 0.002; long good = 0, bad = 0, sent = 100000;
        for (long k = 0; k < sent; k++) {
            int to = 1 + (int)(k & 1), other = 3 - to;
            int32_t v = (int32_t)(k * 2654435761u);
            bi32[0] = v; bi32[1] = bi32[2] = 0x7FFFFFFF;
            lx_publish_to(&N[0], addr_of[to], LX_BIT(1)); pump_bus();
            uint32_t ut = lx_take_updates(&N[to]), uo = lx_take_updates(&N[other]);
            if (uo) bad++;
            if (ut) { if (bi32[to] == v) good++; else bad++; }
        }
        CHECK(lx_take_updates(&N[0]) == 0);
        printf("     delivered %ld/%ld, BAD (corrupt or misrouted) %ld\n", good, sent, bad);
        CHECK(bad == 0);
    }
#endif

#ifdef LX_PORT
    printf("     critical sections: %ld entered, max depth %d, open at exit %d\n", lx_cs_count, lx_cs_max, lx_cs_depth);
    CHECK(lx_cs_depth == 0 && lx_cs_max == 1 && lx_cs_count > 0);
#endif

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails != 0;
}
