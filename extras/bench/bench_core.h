/* LYNX vs BAVA link benchmark: platform-neutral core.
 *
 * Two nodes run this harness: an INITIATOR that drives every measurement and
 * prints a markdown table, and a RESPONDER that answers. The platform supplies
 * a bench_port_t (UART, clocks) and calls bench_rx() with received link bytes
 * and bench_step() in a tight loop. Nothing here blocks except port->write.
 *
 * Measured, per protocol (BAVA only if built with BENCH_WITH_BAVA):
 *  - CPU cycles to encode / decode a reliable 4-byte write, its ACK, a 4-var
 *    write and a 4-byte publish (local loopback, median of BENCH_CYC_RUNS);
 *  - round-trip latency of reliable writes over the link at each baud rate
 *    (min / median / p99 / max over BENCH_N transactions) and how many succeed;
 *  - delivery rate of back-to-back publishes (LYNX; BAVA has no publish).
 *
 * MIT License (c) 2026 Ashwath */
#ifndef BENCH_CORE_H
#define BENCH_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lynx.h"

#ifndef BENCH_N
#define BENCH_N         1000u   /* transactions per link measurement */
#endif
#ifndef BENCH_CYC_RUNS
#define BENCH_CYC_RUNS  201u    /* loopback repetitions (odd: exact median) */
#endif
#define BENCH_MAX_BAUDS 4u

typedef struct {
    void     (*write)(void *ctx, const uint8_t *d, uint16_t n); /* link TX; may block when full */
    bool     (*tx_idle)(void *ctx);         /* everything written has left the wire */
    void     (*set_baud)(void *ctx, uint32_t baud);  /* only called when tx_idle */
    uint32_t (*now_us)(void *ctx);
    uint32_t (*cycles)(void *ctx);          /* free-running CPU cycle counter */
    void     (*print)(void *ctx, const char *line);  /* console, one line, no '\n' */
    void     *ctx;
    uint32_t  cpu_hz;
    const char *cpu_name;
} bench_port_t;

enum { BENCH_INITIATOR = 0, BENCH_RESPONDER = 1 };

typedef struct { uint32_t min, med, p99, max, ok, fail; } bench_dist_t;

typedef struct {        /* local loopback: cycles (median) and bytes on the wire */
    bool     have;
    uint32_t enc1, dec1, ack1;      /* 4 B write: encode, decode + build ACK, decode ACK */
    uint32_t enc4, dec4, ack4;      /* 4 vars, 13 B (BAVA: 4 writes) */
    uint32_t encp, decp;            /* 4 B publish (LYNX only) */
    uint32_t w1, w4, wp;            /* wire bytes: write1+ACK, write4+ACK(s), publish */
    uint32_t bad;                   /* loopback runs that did not complete */
} bench_cyc_t;

typedef struct {
    bool         have;
    bench_dist_t rtt1, rtt4;        /* µs */
    uint32_t     pub_sent, pub_got, pub_us;
} bench_link_t;

typedef struct {                    /* test variables, same on both nodes */
    uint32_t va;                                    /* 4 B */
    int32_t  q1; float q2; uint8_t q3; uint32_t q4; /* 4 vars, 13 B */
    uint32_t pubv;                                  /* publish target */
} bench_vars_t;

typedef struct {
    const bench_port_t *port;
    int      role, state, sub;
    lx_t     lx;
    uint8_t  ev;                    /* last LYNX event, 0 = none yet */
    uint32_t us_last, ms, ms_frac;  /* wrap-safe ms clock for lx_tick */

    bench_vars_t v;
    struct { uint32_t baud; } ctrl;
    struct { uint32_t pub_count, rx_ok, rx_bad; } stats;

    int      bava_slot;             /* -1: BAVA not built */

    /* initiator */
    uint32_t bauds[BENCH_MAX_BAUDS];
    uint8_t  nbauds, bi;
    uint32_t k, j, t0, retries, pub_r0;
    uint32_t nok, nfail;
    uint32_t samples[BENCH_N];
    bench_cyc_t  cyc[2];                            /* [0] LYNX, [1] BAVA */
    bench_link_t link[2][BENCH_MAX_BAUDS];
    const char  *error;

    /* responder */
    bool     baud_pending;
    uint32_t new_baud;
} bench_t;

/* bauds[0] is the rate both nodes start at; the initiator moves the pair
 * through the list. bava_slot: 2 or 3 (0 and 1 are the initiator's local
 * loopback pair), distinct per bench_t in one process. */
void bench_init(bench_t *b, const bench_port_t *port, int role,
                const uint32_t *bauds, uint8_t nbauds, int bava_slot);
void bench_rx(bench_t *b, const uint8_t *d, size_t n);
bool bench_step(bench_t *b);        /* false once the initiator has printed its report */

#endif /* BENCH_CORE_H */
