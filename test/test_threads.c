/* Concurrency stress test, meant to run under ThreadSanitizer.
 *
 * Node A's RX path runs in its own thread while the main thread drives A's
 * app path (lx_set, lx_write, lx_tick, lx_get). Node B runs in a third
 * thread and publishes a counter to A the whole time. 5 % of frames are
 * dropped, so retransmits, timeouts and late ACKs collide constantly.
 *
 *   default build : A's RX thread decodes; tools/run_tests.sh builds with
 *                   -DLX_PORT -Itest/port_mutex (critical sections = mutex)
 *   LX_ISR_SAFE=1 : A's RX thread only queues bytes (as a UART ISR would),
 *                   the main thread decodes with lx_poll; no lock at all
 *
 * Checks: every transaction ends with exactly one event, a DONE write really
 * landed on B, values read with lx_get are never torn, no TSan reports.
 * MIT License (c) 2026 Ashwath */
#define _POSIX_C_SOURCE 200809L
#include "lynx.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef LX_PORT
pthread_mutex_t lx_test_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#define TXNS 2000
#define DROP_PERCENT 5

/* ---- thread-safe byte pipes that drop whole frames now and then ---- */
typedef struct { pthread_mutex_t m; uint8_t buf[1 << 16]; size_t n; unsigned seed; } tq_t;
static tq_t a2b = { PTHREAD_MUTEX_INITIALIZER, {0}, 0, 1 }, b2a = { PTHREAD_MUTEX_INITIALIZER, {0}, 0, 2 };

static void tq_push(void *ctx, const uint8_t *d, uint16_t n)
{
    tq_t *q = ctx;
    pthread_mutex_lock(&q->m);
    if ((unsigned)rand_r(&q->seed) % 100u >= DROP_PERCENT && q->n + n <= sizeof q->buf) {
        memcpy(q->buf + q->n, d, n); q->n += n;
    }
    pthread_mutex_unlock(&q->m);
}

static size_t tq_pop(tq_t *q, uint8_t *out, size_t max)
{
    pthread_mutex_lock(&q->m);
    size_t n = q->n < max ? q->n : max;
    memcpy(out, q->buf, n);
    memmove(q->buf, q->buf + n, q->n - n); q->n -= n;
    pthread_mutex_unlock(&q->m);
    return n;
}

static uint32_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

/* ---- nodes ---- */
typedef struct { uint32_t lo, hi; } pair_t;  /* hi == ~lo always: a torn copy shows */

static lx_t A, B;
static int32_t a_w, b_w;        /* id 2: written by A's app, reliable */
static pair_t a_cnt = { 0, ~0u }, b_cnt;   /* id 3: B's counter, published to A (valid from the start) */
static int stop;
static long ev_done, ev_timeout, ev_other;

static void on_ev_a(void *ctx, uint8_t ev, uint32_t mask)
{
    (void)ctx; (void)mask;
    __atomic_fetch_add(ev == LX_EV_DONE ? &ev_done : ev == LX_EV_TIMEOUT ? &ev_timeout : &ev_other,
                       1, __ATOMIC_RELAXED);
}

static void *a_rx_thread(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        size_t n = tq_pop(&b2a, buf, sizeof buf);
        if (n) lx_rx_buf(&A, buf, n); else sched_yield();   /* ISR-safe build: only queues */
    }
    return NULL;
}

static uint64_t now_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void *b_thread(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    uint32_t c = 0;
    uint64_t last = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        size_t n = tq_pop(&a2b, buf, sizeof buf);
        if (n) { lx_rx_buf(&B, buf, n); lx_poll(&B); }
        if (now_us() - last >= 100) {                   /* ~10k frames/s, like a fast UART */
            last = now_us();
            c++; b_cnt.lo = c; b_cnt.hi = ~c;           /* B's own thread: plain access */
            lx_publish(&B, LX_BIT(3));
        }
        if (!n) sched_yield();
    }
    return NULL;
}

int main(void)
{
    lx_init(&A, tq_push, on_ev_a, &a2b);
    lx_init(&B, tq_push, NULL, &b2a);
    lx_bind(&A, 2, &a_w, sizeof a_w);   lx_bind(&B, 2, &b_w, sizeof b_w);
    lx_bind(&A, 3, &a_cnt, sizeof a_cnt); lx_bind(&B, 3, &b_cnt, sizeof b_cnt);
#if LX_BUS
    lx_set_addr(&A, 1, 2); lx_set_addr(&B, 2, 1);
#endif

    pthread_t ta, tb;
    pthread_create(&ta, NULL, a_rx_thread, NULL);
    pthread_create(&tb, NULL, b_thread, NULL);

    long torn = 0, wrong = 0, reads = 0;
    uint32_t last_cnt = 0;
    for (int32_t k = 1; k <= TXNS; k++) {
        long before = __atomic_load_n(&ev_done, __ATOMIC_RELAXED) + __atomic_load_n(&ev_timeout, __ATOMIC_RELAXED);
        lx_set(&A, 2, &k);
        if (lx_write(&A, LX_BIT(2)) != LX_OK) { wrong++; continue; }
        while (lx_busy(&A)) {
            lx_poll(&A);
            lx_tick(&A, now_ms());
            pair_t p;
            lx_get(&A, 3, &p);
            reads++;
            if (p.hi != ~p.lo) torn++;
            else if (p.lo < last_cnt) wrong++;          /* B's counter only grows */
            else last_cnt = p.lo;
            sched_yield();
        }
        long after = __atomic_load_n(&ev_done, __ATOMIC_RELAXED) + __atomic_load_n(&ev_timeout, __ATOMIC_RELAXED);
        if (after != before + 1) wrong++;               /* exactly one event per transaction */
    }

    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    lx_poll(&B);

    long done = ev_done, timeout = ev_timeout;
    int ok = torn == 0 && wrong == 0 && ev_other == 0 && done + timeout == TXNS && done > TXNS / 2;
    /* the last DONE write must be on B; a final TIMEOUT may or may not have landed */
    printf("%s: %d writes: %ld done, %ld timeout | %ld lx_get reads, torn %ld | wrong %ld | A retx %u%s\n",
           LX_ISR_SAFE ? "ISR-safe queue" : "locked RX thread", TXNS, done, timeout, reads, torn, wrong,
           (unsigned)A.st_retx, ok ? "" : "  <-- FAILED");
    if (b_w < TXNS - 1) { printf("B ended at %d\n", b_w); ok = 0; }
    return ok ? 0 : 1;
}
