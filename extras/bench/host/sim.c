/* Host simulation of the on-target benchmark: an initiator and a responder
 * bench_t joined by a simulated UART in virtual time. Each byte occupies the
 * line for 10 bits at the sender's current baud; a byte that arrives while
 * the receiver is at a different baud is delivered as garbage, which is what
 * a real UART does across a baud switch.
 *
 * This checks the harness, not performance: the simulated CPU is infinitely
 * fast, so every round trip must equal pure wire time. The run fails if one
 * doesn't, or if any transaction or publish is lost on a clean line.
 *
 *   sim [loss_probability]      e.g. sim 0.001 to exercise retries
 *
 * MIT License (c) 2026 Ashwath */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bench_core.h"

#define QBITS 17
#define QMASK ((1u << QBITS) - 1u)

typedef struct { uint8_t b; uint32_t baud; uint64_t t; } wbyte_t;
typedef struct { wbyte_t q[1u << QBITS]; uint32_t head, tail; uint64_t line_free; } wire_t;

static uint64_t vt;                 /* virtual time, ns */
static uint32_t baud[2];            /* current baud of side 0 (initiator), 1 (responder) */
static wire_t wire[2];              /* wire[s]: bytes sent by side s */
static double loss;
static int side_id[2] = { 0, 1 };

static void w_write(void *ctx, const uint8_t *d, uint16_t n)
{
    int s = *(int *)ctx;
    wire_t *w = &wire[s];
    uint64_t per_byte = 10000000000ull / baud[s];
    for (uint16_t i = 0; i < n; i++) {
        uint64_t start = w->line_free > vt ? w->line_free : vt;
        w->line_free = start + per_byte;
        w->q[w->tail++ & QMASK] = (wbyte_t){ d[i], baud[s], w->line_free };
    }
}
static bool w_idle(void *ctx)                 { return wire[*(int *)ctx].line_free <= vt; }
static void w_baud(void *ctx, uint32_t b)     { baud[*(int *)ctx] = b; }
static uint32_t w_now(void *ctx)              { (void)ctx; return (uint32_t)(vt / 1000u); }
static void w_print(void *ctx, const char *s) { if (*(int *)ctx == 0) puts(s); }
static uint32_t w_cycles(void *ctx)
{
    (void)ctx;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

static void deliver(int from, bench_t *to)
{
    wire_t *w = &wire[from];
    while (w->head != w->tail && w->q[w->head & QMASK].t <= vt) {
        wbyte_t x = w->q[w->head++ & QMASK];
        if (loss > 0 && (double)rand() / RAND_MAX < loss) continue;
        if (x.baud != baud[1 - from]) x.b = (uint8_t)rand();     /* baud mismatch: garbage */
        bench_rx(to, &x.b, 1);
    }
}

static int check(const char *what, uint32_t got, uint32_t lo, uint32_t hi)
{
    int ok = got >= lo && got <= hi;
    if (!ok) printf("CHECK FAILED: %s = %lu, expected %lu..%lu\n", what,
                    (unsigned long)got, (unsigned long)lo, (unsigned long)hi);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    static const uint32_t bauds[] = { 115200, 2000000 };
    static bench_t I, R;
    const bench_port_t pi = { w_write, w_idle, w_baud, w_now, w_cycles, w_print, &side_id[0], 1000000000u, "host, simulated UART" };
    const bench_port_t pr = { w_write, w_idle, w_baud, w_now, w_cycles, w_print, &side_id[1], 1000000000u, "host, simulated UART" };

    loss = argc > 1 ? atof(argv[1]) : 0.0;
    srand(1);
    baud[0] = baud[1] = bauds[0];
    bench_init(&R, &pr, BENCH_RESPONDER, bauds, 2, 3);
    bench_init(&I, &pi, BENCH_INITIATOR, bauds, 2, 2);

    while (bench_step(&I)) {
        bench_step(&R);
        deliver(0, &R);
        deliver(1, &I);
        vt += 1000u;                                    /* 1 µs */
        if (vt > 600ull * 1000000000u) { puts("CHECK FAILED: no report after 600 s virtual"); return 1; }
    }
    printf("<!-- simulated %.1f s of link time -->\n", (double)vt / 1e9);

    if (I.error) return 1;
    if (loss > 0) return 0;         /* lossy run: only has to finish */

    int bad = 0;
    for (int p = 0; p < 2; p++) {   /* 0 LYNX, 1 BAVA */
        const bench_cyc_t *c = &I.cyc[p];
        if (!c->have) continue;
        bad += check("loopback runs that did not complete", c->bad, 0, 0);
        for (int i = 0; i < 2; i++) {
            const bench_link_t *L = &I.link[p][i];
            uint32_t w1 = (uint32_t)(c->w1 * 10000000ull / bauds[i]), w4 = (uint32_t)(c->w4 * 10000000ull / bauds[i]);
            bad += check("4 B writes acknowledged", L->rtt1.ok, BENCH_N, BENCH_N);
            bad += check("4 B write median RTT vs wire time (µs)", L->rtt1.med, w1, w1 + 3);
            bad += check("4 B write max RTT vs wire time (µs)", L->rtt1.max, w1, w1 + 3);
            bad += check("4-var writes acknowledged", L->rtt4.ok, BENCH_N, BENCH_N);
            bad += check("4-var median RTT vs wire time (µs)", L->rtt4.med, w4, w4 + 3 * (p ? 4 : 1));
            if (p == 0) bad += check("publishes delivered", L->pub_got, BENCH_N, BENCH_N);
        }
    }
    puts(bad ? "SIMULATION CHECKS FAILED" : "SIMULATION CHECKS PASSED");
    return bad != 0;
}
