/* BAVA behind a type-free adapter, so its build mode (ESP_PLATFORM or bare
 * metal) never leaks into the rest of the benchmark. Uses only the public API
 * in bava.h; BAVA itself is GPLv3 and is not part of this repository.
 * MIT License (c) 2026 Ashwath */
#ifndef BENCH_BAVA_H
#define BENCH_BAVA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BENCH_BAVA_SLOTS 4

typedef void (*bench_bava_tx_fn)(void *ctx, const uint8_t *d, uint16_t n);

/* Each slot binds ids 1..4 as int32, float, uint8, uint32 (13 B). */
void   bench_bava_init(int slot, bench_bava_tx_fn tx, void *ctx);
void   bench_bava_set(int slot, int32_t v1, float v2, uint8_t v3, uint32_t v4);
void   bench_bava_write(int slot, uint8_t id);      /* reliable write, ACK expected */
bool   bench_bava_waiting(int slot);                /* ACK still outstanding */
bool   bench_bava_failed(int slot);                 /* error callback since last write */
void   bench_bava_rx(int slot, const uint8_t *d, size_t n);
void   bench_bava_tick(int slot, uint32_t ms);
size_t bench_bava_handle_size(void);

#endif /* BENCH_BAVA_H */
