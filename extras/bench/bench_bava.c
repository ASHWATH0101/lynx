/* BAVA adapter for the benchmark. Public bava.h API only (black box).
 * MIT License (c) 2026 Ashwath */
#include <string.h>
#include "bava.h"
#include "bench_bava.h"

static bava_handle_t H[BENCH_BAVA_SLOTS];
static struct { int32_t v1; float v2; uint8_t v3; uint32_t v4; } V[BENCH_BAVA_SLOTS];
static bench_bava_tx_fn tx_fn[BENCH_BAVA_SLOTS];
static void *tx_ctx[BENCH_BAVA_SLOTS];
static bool err[BENCH_BAVA_SLOTS];
static int cur = -1;    /* slot inside a BAVA call: its callbacks carry no context */

static void tx0(const uint8_t *d, uint16_t n) { tx_fn[0](tx_ctx[0], d, n); }
static void tx1(const uint8_t *d, uint16_t n) { tx_fn[1](tx_ctx[1], d, n); }
static void tx2(const uint8_t *d, uint16_t n) { tx_fn[2](tx_ctx[2], d, n); }
static void tx3(const uint8_t *d, uint16_t n) { tx_fn[3](tx_ctx[3], d, n); }
static const bava_tx_cb_t TX[BENCH_BAVA_SLOTS] = { tx0, tx1, tx2, tx3 };

static void on_error(uint8_t id, uint8_t code)
{
    (void)id; (void)code;
    if (cur >= 0) err[cur] = true;
}

void bench_bava_init(int s, bench_bava_tx_fn tx, void *ctx)
{
    tx_fn[s] = tx; tx_ctx[s] = ctx;
    bava_init(&H[s], TX[s]);
    H[s].error_callback = on_error;
    bava_register_var(&H[s], 1, &V[s].v1, sizeof V[s].v1);
    bava_register_var(&H[s], 2, &V[s].v2, sizeof V[s].v2);
    bava_register_var(&H[s], 3, &V[s].v3, sizeof V[s].v3);
    bava_register_var(&H[s], 4, &V[s].v4, sizeof V[s].v4);
}

void bench_bava_set(int s, int32_t v1, float v2, uint8_t v3, uint32_t v4)
{
    V[s].v1 = v1; V[s].v2 = v2; V[s].v3 = v3; V[s].v4 = v4;
}

void bench_bava_write(int s, uint8_t id)
{
    cur = s; err[s] = false;
    bava_send_write(&H[s], id);
    cur = -1;
}

bool bench_bava_waiting(int s) { return H[s].is_waiting_ack; }
bool bench_bava_failed(int s)  { return err[s]; }

void bench_bava_rx(int s, const uint8_t *d, size_t n)
{
    cur = s;
    while (n--) bava_process_byte(&H[s], *d++);
    cur = -1;
}

void bench_bava_tick(int s, uint32_t ms)
{
    cur = s;
    bava_tick(&H[s], ms);
    cur = -1;
}

size_t bench_bava_handle_size(void) { return sizeof(bava_handle_t); }
