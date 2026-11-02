/* LYNX vs BAVA link benchmark: platform-neutral core. See bench_core.h.
 * MIT License (c) 2026 Ashwath */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_core.h"
#ifdef BENCH_WITH_BAVA
#include "bench_bava.h"
#endif

enum { ID_A, ID_Q1, ID_Q2, ID_Q3, ID_Q4, ID_PUB, ID_CTRL, ID_STATS };
#define MASK4       (LX_BIT(ID_Q1) | LX_BIT(ID_Q2) | LX_BIT(ID_Q3) | LX_BIT(ID_Q4))
#define BAVA_WAIT   250000u     /* µs before the harness gives up on a BAVA ACK */
#define TRIES       100u        /* control-transaction attempts before giving up */
#define SETTLE_US   20000u      /* after a publish burst has left the wire */

enum { S_SYNC, S_L1, S_L4, S_P0, S_PUB, S_PWAIT, S_P1, S_B1, S_B4, S_BAUD, S_REPORT, S_END };

static uint32_t now(const bench_t *b) { return b->port->now_us(b->port->ctx); }
static uint32_t cyc(const bench_t *b) { return b->port->cycles(b->port->ctx); }

static void out(const bench_t *b, const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    b->port->print(b->port->ctx, line);
}

static void link_tx(void *ctx, const uint8_t *d, uint16_t n)
{
    const bench_t *b = ctx;
    b->port->write(b->port->ctx, d, n);
}

static void link_ev(void *ctx, uint8_t ev, uint32_t mask)
{
    (void)mask;
    ((bench_t *)ctx)->ev = ev;
}

static void bind_vars(lx_t *h, bench_vars_t *v)
{
    lx_bind(h, ID_A,   &v->va,   sizeof v->va);
    lx_bind(h, ID_Q1,  &v->q1,   sizeof v->q1);
    lx_bind(h, ID_Q2,  &v->q2,   sizeof v->q2);
    lx_bind(h, ID_Q3,  &v->q3,   sizeof v->q3);
    lx_bind(h, ID_Q4,  &v->q4,   sizeof v->q4);
    lx_bind(h, ID_PUB, &v->pubv, sizeof v->pubv);
}

static void set_vars(bench_vars_t *v, uint32_t k)
{
    v->va = k; v->q1 = (int32_t)k; v->q2 = (float)k; v->q3 = (uint8_t)k; v->q4 = ~k;
}

/* ---------------- local loopback: cycles per encode / decode ----------- */
static struct { uint8_t buf[2][512]; uint16_t n[2]; } lb;
static int lb_dir[2] = { 0, 1 };
static uint32_t m[3][BENCH_CYC_RUNS];
static bench_vars_t lv[2];

static void lb_tx(void *ctx, const uint8_t *d, uint16_t n)
{
    int i = *(const int *)ctx;
    if ((size_t)lb.n[i] + n <= sizeof lb.buf[i]) { memcpy(lb.buf[i] + lb.n[i], d, n); lb.n[i] = (uint16_t)(lb.n[i] + n); }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t median(uint32_t *v, uint32_t n)
{
    qsort(v, n, sizeof *v, cmp_u32);
    return v[n / 2];
}

static uint32_t span(uint32_t c0, uint32_t c1, uint32_t overhead)
{
    uint32_t d = c1 - c0;
    return d > overhead ? d - overhead : 0;
}

static uint32_t cycle_overhead(const bench_t *b)
{
    for (uint32_t r = 0; r < BENCH_CYC_RUNS; r++) { uint32_t c0 = cyc(b); m[0][r] = cyc(b) - c0; }
    return median(m[0], BENCH_CYC_RUNS);
}

static void measure_lynx(const bench_t *b, bench_cyc_t *c)
{
    static lx_t A, B;
    lx_init(&A, lb_tx, NULL, &lb_dir[0]); bind_vars(&A, &lv[0]);
    lx_init(&B, lb_tx, NULL, &lb_dir[1]); bind_vars(&B, &lv[1]);
    uint32_t oh = cycle_overhead(b);

    for (int pass = 0; pass < 2; pass++) {          /* 0: 4 B write, 1: 4 vars */
        uint32_t mask = pass ? MASK4 : LX_BIT(ID_A);
        for (uint32_t r = 0; r < BENCH_CYC_RUNS; r++) {
            set_vars(&lv[0], r); lb.n[0] = lb.n[1] = 0;
            uint32_t c0 = cyc(b); lx_write(&A, mask);
            uint32_t c1 = cyc(b); lx_rx_buf(&B, lb.buf[0], lb.n[0]);
            uint32_t c2 = cyc(b); lx_rx_buf(&A, lb.buf[1], lb.n[1]);
            uint32_t c3 = cyc(b);
            m[0][r] = span(c0, c1, oh); m[1][r] = span(c1, c2, oh); m[2][r] = span(c2, c3, oh);
            if (lx_busy(&A) || (pass ? lv[1].q4 != lv[0].q4 : lv[1].va != lv[0].va)) c->bad++;
        }
        uint32_t w = (uint32_t)lb.n[0] + lb.n[1];
        if (pass) { c->enc4 = median(m[0], BENCH_CYC_RUNS); c->dec4 = median(m[1], BENCH_CYC_RUNS); c->ack4 = median(m[2], BENCH_CYC_RUNS); c->w4 = w; }
        else      { c->enc1 = median(m[0], BENCH_CYC_RUNS); c->dec1 = median(m[1], BENCH_CYC_RUNS); c->ack1 = median(m[2], BENCH_CYC_RUNS); c->w1 = w; }
    }
    for (uint32_t r = 0; r < BENCH_CYC_RUNS; r++) {
        lv[0].pubv = r; lb.n[0] = 0;
        uint32_t c0 = cyc(b); lx_publish(&A, LX_BIT(ID_PUB));
        uint32_t c1 = cyc(b); lx_rx_buf(&B, lb.buf[0], lb.n[0]);
        uint32_t c2 = cyc(b);
        m[0][r] = span(c0, c1, oh); m[1][r] = span(c1, c2, oh);
        if (lv[1].pubv != r) c->bad++;
    }
    c->wp = lb.n[0];
    c->encp = median(m[0], BENCH_CYC_RUNS); c->decp = median(m[1], BENCH_CYC_RUNS);
    c->have = true;
}

#ifdef BENCH_WITH_BAVA
static void measure_bava(const bench_t *b, bench_cyc_t *c)
{
    bench_bava_init(0, lb_tx, &lb_dir[0]);
    bench_bava_init(1, lb_tx, &lb_dir[1]);
    uint32_t oh = cycle_overhead(b), ms = 0;

    for (int pass = 0; pass < 2; pass++) {          /* 0: id 1 (4 B), 1: ids 1..4 */
        uint8_t last = pass ? 4 : 1;
        uint32_t w = 0;
        for (uint32_t r = 0; r < BENCH_CYC_RUNS; r++) {
            bench_bava_set(0, (int32_t)r, (float)r, (uint8_t)r, ~r);
            m[0][r] = m[1][r] = m[2][r] = 0; w = 0;
            for (uint8_t id = 1; id <= last; id++) {
                bench_bava_tick(0, ++ms); bench_bava_tick(1, ms);
                lb.n[0] = lb.n[1] = 0;
                uint32_t c0 = cyc(b); bench_bava_write(0, id);
                uint32_t c1 = cyc(b); bench_bava_rx(1, lb.buf[0], lb.n[0]);
                uint32_t c2 = cyc(b); bench_bava_rx(0, lb.buf[1], lb.n[1]);
                uint32_t c3 = cyc(b);
                m[0][r] += span(c0, c1, oh); m[1][r] += span(c1, c2, oh); m[2][r] += span(c2, c3, oh);
                w += (uint32_t)lb.n[0] + lb.n[1];
                if (bench_bava_waiting(0) || bench_bava_failed(0)) c->bad++;
            }
        }
        if (pass) { c->enc4 = median(m[0], BENCH_CYC_RUNS); c->dec4 = median(m[1], BENCH_CYC_RUNS); c->ack4 = median(m[2], BENCH_CYC_RUNS); c->w4 = w; }
        else      { c->enc1 = median(m[0], BENCH_CYC_RUNS); c->dec1 = median(m[1], BENCH_CYC_RUNS); c->ack1 = median(m[2], BENCH_CYC_RUNS); c->w1 = w; }
    }
    c->have = true;
}
#endif

/* ---------------- link measurements ----------------------------------- */
static void go(bench_t *b, int state)
{
    b->state = state; b->sub = 0;
    b->k = b->j = b->nok = b->nfail = b->retries = 0;
}

static void finish_dist(bench_t *b, bench_dist_t *d)
{
    d->ok = b->nok; d->fail = b->nfail;
    if (!b->nok) return;
    qsort(b->samples, b->nok, sizeof b->samples[0], cmp_u32);
    d->min = b->samples[0];
    d->med = b->samples[b->nok / 2];
    d->p99 = b->samples[(b->nok * 99u) / 100u];
    d->max = b->samples[b->nok - 1];
}

/* Drive one LYNX control transaction (read stats / write ctrl).
 * Returns 1 when DONE, 0 while pending, -1 after TRIES failed attempts. */
static int control_txn(bench_t *b, bool write, uint8_t id)
{
    if (b->sub == 0) {
        b->ev = 0;
        if ((write ? lx_write(&b->lx, LX_BIT(id)) : lx_read(&b->lx, id)) == LX_OK) b->sub = 1;
        return 0;
    }
    if (!b->ev) return 0;
    if (b->ev == LX_EV_DONE) return 1;
    b->sub = 0;
    return ++b->retries >= TRIES ? -1 : 0;
}

static int after_link_tests(void)
{
#ifdef BENCH_WITH_BAVA
    return S_B1;
#else
    return S_BAUD;
#endif
}

static void report(bench_t *b);

static bool step_initiator(bench_t *b)
{
    bench_link_t *L = &b->link[0][b->bi];
    int r;

    switch (b->state) {
    case S_SYNC:
        if ((r = control_txn(b, false, ID_STATS)) > 0) {
            out(b, "<!-- link up at %lu baud -->", (unsigned long)b->bauds[b->bi]);
            go(b, S_L1);
        } else if (r < 0) {
            b->error = "responder not answering"; go(b, S_REPORT);
        }
        break;

    case S_L1: case S_L4:
        if (b->k == BENCH_N) {
            finish_dist(b, b->state == S_L1 ? &L->rtt1 : &L->rtt4);
            L->have = true;
            go(b, b->state == S_L1 ? S_L4 : S_P0);
        } else if (b->sub == 0) {
            set_vars(&b->v, b->k);
            b->ev = 0; b->t0 = now(b);
            if (lx_write(&b->lx, b->state == S_L1 ? LX_BIT(ID_A) : MASK4) == LX_OK) b->sub = 1;
        } else if (b->ev) {
            if (b->ev == LX_EV_DONE) b->samples[b->nok++] = now(b) - b->t0;
            else b->nfail++;
            b->k++; b->sub = 0;
        }
        break;

    case S_P0: case S_P1:       /* responder's publish counter, before / after */
        if ((r = control_txn(b, false, ID_STATS)) > 0) {
            if (b->state == S_P0) { b->pub_r0 = b->stats.pub_count; go(b, S_PUB); }
            else { L->pub_got = b->stats.pub_count - b->pub_r0; go(b, after_link_tests()); }
        } else if (r < 0) {
            b->error = "lost the responder during the publish test"; go(b, S_REPORT);
        }
        break;

    case S_PUB:                 /* back to back, as fast as the UART takes them */
        b->t0 = now(b);
        for (uint32_t i = 0; i < BENCH_N; i++) { b->v.pubv = i; lx_publish(&b->lx, LX_BIT(ID_PUB)); }
        L->pub_sent = BENCH_N;
        go(b, S_PWAIT);
        break;

    case S_PWAIT:
        if (b->sub == 0) {
            if (b->port->tx_idle(b->port->ctx)) { L->pub_us = now(b) - b->t0; b->t0 = now(b); b->sub = 1; }
        } else if (now(b) - b->t0 >= SETTLE_US) {
            go(b, S_P1);
        }
        break;

#ifdef BENCH_WITH_BAVA
    case S_B1: case S_B4: {
        bench_link_t *LB = &b->link[1][b->bi];
        int s = b->bava_slot;
        if (b->k == BENCH_N) {
            finish_dist(b, b->state == S_B1 ? &LB->rtt1 : &LB->rtt4);
            LB->have = true;
            go(b, b->state == S_B1 ? S_B4 : S_BAUD);
        } else if (b->sub == 0) {
            bench_bava_set(s, (int32_t)b->k, (float)b->k, (uint8_t)b->k, ~b->k);
            b->j = 0; b->t0 = now(b);
            bench_bava_write(s, 1);
            b->sub = 1;
        } else if (bench_bava_failed(s) || (bench_bava_waiting(s) && now(b) - b->t0 > BAVA_WAIT)) {
            b->nfail++; b->k++; b->sub = 0;
        } else if (!bench_bava_waiting(s)) {
            if (b->state == S_B4 && ++b->j < 4) {
                bench_bava_write(s, (uint8_t)(1 + b->j));       /* BAVA: one variable per write */
            } else {
                b->samples[b->nok++] = now(b) - b->t0; b->k++; b->sub = 0;
            }
        }
        break;
    }
#endif

    case S_BAUD:
        if (b->bi + 1u >= b->nbauds) { go(b, S_REPORT); break; }
        if (b->sub == 0) {
            b->ctrl.baud = b->bauds[b->bi + 1];
            b->ev = 0;
            if (lx_write(&b->lx, LX_BIT(ID_CTRL)) == LX_OK) b->sub = 1;
        } else if (b->sub == 1) {
            if (b->ev) b->sub = 2;  /* even on TIMEOUT: only the ACK may have been lost */
        } else if (b->port->tx_idle(b->port->ctx)) {
            b->bi++;
            b->port->set_baud(b->port->ctx, b->bauds[b->bi]);
            go(b, S_SYNC);
        }
        break;

    case S_REPORT:
        report(b);
        go(b, S_END);
        return false;

    default:
        return false;
    }
    return true;
}

static bool step_responder(bench_t *b)
{
    b->stats.rx_ok = b->lx.st_rx_ok;
    b->stats.rx_bad = b->lx.st_rx_bad;
    if (b->baud_pending && b->port->tx_idle(b->port->ctx)) {   /* our ACK is out */
        b->baud_pending = false;
        b->port->set_baud(b->port->ctx, b->new_baud);
    }
    return true;
}

/* ---------------- public ---------------------------------------------- */
void bench_init(bench_t *b, const bench_port_t *port, int role,
                const uint32_t *bauds, uint8_t nbauds, int bava_slot)
{
    memset(b, 0, sizeof *b);
    b->port = port; b->role = role; b->bava_slot = -1;
    b->nbauds = nbauds < BENCH_MAX_BAUDS ? nbauds : (uint8_t)BENCH_MAX_BAUDS;
    memcpy(b->bauds, bauds, b->nbauds * sizeof bauds[0]);

    lx_init(&b->lx, link_tx, link_ev, b);
    bind_vars(&b->lx, &b->v);
    lx_bind(&b->lx, ID_CTRL, &b->ctrl, sizeof b->ctrl);
    lx_bind(&b->lx, ID_STATS, &b->stats, sizeof b->stats);
#ifdef BENCH_WITH_BAVA
    b->bava_slot = bava_slot;
    bench_bava_init(bava_slot, link_tx, b);
#else
    (void)bava_slot;
#endif
    b->us_last = now(b);

    if (role == BENCH_INITIATOR) {
        out(b, "<!-- measuring local encode/decode cycles -->");
        measure_lynx(b, &b->cyc[0]);
#ifdef BENCH_WITH_BAVA
        measure_bava(b, &b->cyc[1]);
#endif
        go(b, S_SYNC);
    }
}

void bench_rx(bench_t *b, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {        /* byte-interleaved: each protocol sees */
        lx_rx(&b->lx, d[i]);                /* its frame end at the same moment     */
        if (b->role == BENCH_RESPONDER) {
            uint32_t u = lx_take_updates(&b->lx);
            if (u & LX_BIT(ID_PUB)) b->stats.pub_count++;   /* per frame, never coalesced */
            if (u & LX_BIT(ID_CTRL)) { b->baud_pending = true; b->new_baud = b->ctrl.baud; }
        }
#ifdef BENCH_WITH_BAVA
        bench_bava_rx(b->bava_slot, &d[i], 1);
#endif
    }
}

bool bench_step(bench_t *b)
{
    uint32_t t = now(b);
    b->ms_frac += t - b->us_last; b->us_last = t;
    b->ms += b->ms_frac / 1000u;   b->ms_frac %= 1000u;
    lx_tick(&b->lx, b->ms);
#ifdef BENCH_WITH_BAVA
    bench_bava_tick(b->bava_slot, b->ms);
#endif
    return b->role == BENCH_RESPONDER ? step_responder(b) : step_initiator(b);
}

/* ---------------- report --------------------------------------------- */
static const char *num(char *s, size_t n, bool have, uint32_t v)
{
    if (have) snprintf(s, n, "%lu", (unsigned long)v); else snprintf(s, n, "n/a");
    return s;
}

static void row(const bench_t *b, const char *what, bool h0, uint32_t v0, bool h1, uint32_t v1)
{
    char x[16], y[16];
    out(b, "| %s | %s | %s |", what, num(x, sizeof x, h0, v0), num(y, sizeof y, h1, v1));
}

static const char *dist(char *s, size_t n, bool have, const bench_dist_t *d)
{
    if (!have) snprintf(s, n, "n/a");
    else if (!d->ok) snprintf(s, n, "all failed");
    else snprintf(s, n, "%lu / %lu / %lu / %lu", (unsigned long)d->min, (unsigned long)d->med,
                  (unsigned long)d->p99, (unsigned long)d->max);
    return s;
}

static void report(bench_t *b)
{
    const bench_cyc_t *cl = &b->cyc[0], *cb = &b->cyc[1];
    char x[64], y[64];

    out(b, "");
    out(b, "### LYNX vs BAVA on %s @ %lu MHz", b->port->cpu_name, (unsigned long)(b->port->cpu_hz / 1000000u));
    if (b->error) out(b, "**Run incomplete: %s.**", b->error);
    out(b, "");
    out(b, "| Local loopback, CPU cycles (median of %u) | LYNX | BAVA |", (unsigned)BENCH_CYC_RUNS);
    out(b, "|---|---:|---:|");
    row(b, "Encode reliable 4 B write", cl->have, cl->enc1, cb->have, cb->enc1);
    row(b, "Decode it + build the ACK (responder)", cl->have, cl->dec1, cb->have, cb->dec1);
    row(b, "Decode the ACK (initiator)", cl->have, cl->ack1, cb->have, cb->ack1);
    row(b, "Encode 4 variables, 13 B (BAVA: 4 writes)", cl->have, cl->enc4, cb->have, cb->enc4);
    row(b, "Decode them + build ACK(s)", cl->have, cl->dec4, cb->have, cb->dec4);
    row(b, "Decode the ACK(s)", cl->have, cl->ack4, cb->have, cb->ack4);
    row(b, "Encode 4 B publish (no ACK)", cl->have, cl->encp, false, 0);
    row(b, "Decode 4 B publish", cl->have, cl->decp, false, 0);
    row(b, "Bytes on wire: 4 B write + ACK", cl->have, cl->w1, cb->have, cb->w1);
    row(b, "Bytes on wire: 4 variables + ACK(s)", cl->have, cl->w4, cb->have, cb->w4);
    row(b, "Bytes on wire: 4 B publish", cl->have, cl->wp, false, 0);
    if (cl->bad || cb->bad)
        out(b, "| Loopback runs that did not complete | %lu | %lu |", (unsigned long)cl->bad, (unsigned long)cb->bad);

    for (uint8_t i = 0; i < b->nbauds; i++) {
        const bench_link_t *L = &b->link[0][i], *B = &b->link[1][i];
        if (!L->have && !B->have) continue;
        uint32_t baud = b->bauds[i];
        out(b, "");
        out(b, "| Link @ %lu baud, %u transactions | LYNX | BAVA |", (unsigned long)baud, (unsigned)BENCH_N);
        out(b, "|---|---:|---:|");
        out(b, "| Wire time of 4 B write + ACK (µs, 10 bits/byte) | %lu | %s |",
            (unsigned long)(cl->w1 * 10000000ull / baud),
            num(y, sizeof y, cb->have, (uint32_t)(cb->w1 * 10000000ull / baud)));
        out(b, "| 4 B write round trip, µs: min / median / p99 / max | %s | %s |",
            dist(x, sizeof x, L->have, &L->rtt1), dist(y, sizeof y, B->have, &B->rtt1));
        out(b, "| 4 B writes acknowledged | %lu/%u | %s |", (unsigned long)L->rtt1.ok, (unsigned)BENCH_N,
            B->have ? (snprintf(y, sizeof y, "%lu/%u", (unsigned long)B->rtt1.ok, (unsigned)BENCH_N), y) : "n/a");
        out(b, "| 4 variables round trip, µs: min / median / p99 / max | %s | %s |",
            dist(x, sizeof x, L->have, &L->rtt4), dist(y, sizeof y, B->have, &B->rtt4));
        out(b, "| 4-variable updates acknowledged | %lu/%u | %s |", (unsigned long)L->rtt4.ok, (unsigned)BENCH_N,
            B->have ? (snprintf(y, sizeof y, "%lu/%u", (unsigned long)B->rtt4.ok, (unsigned)BENCH_N), y) : "n/a");
        if (L->pub_sent)
            out(b, "| Back-to-back 4 B publishes delivered | %lu/%lu (%lu frames/s) | n/a |",
                (unsigned long)L->pub_got, (unsigned long)L->pub_sent,
                (unsigned long)(L->pub_us ? (uint64_t)L->pub_sent * 1000000u / L->pub_us : 0));
    }
    out(b, "");
    out(b, "<!-- sizeof(lx_t) = %u -->", (unsigned)sizeof(lx_t));
}
