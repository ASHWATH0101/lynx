/*
 * LYNX compile-time configuration.
 *
 * Every option has a default here. Override it with -D on the command line,
 * or with a #define before including lynx.h. On ESP-IDF and Zephyr the
 * platform port (port/<platform>/lynx_port.h) sets them from Kconfig.
 * Both nodes of a link must be built with the same LX_BUS and LX_MAX_PAYLOAD.
 *
 * MIT License (c) 2026 Ashwath
 */
#ifndef LYNX_CONFIG_H
#define LYNX_CONFIG_H

/* The ESP-IDF / Zephyr build integration defines LX_PORT and puts the
 * platform's port directory on the include path. */
#ifdef LX_PORT
#include "lynx_port.h"
#endif

/* ------------------------------------------------------------------ */
/* Sizes and timing                                                    */
/* ------------------------------------------------------------------ */

#ifndef LX_MAX_VARS
#define LX_MAX_VARS     32u     /* variable ids 0..LX_MAX_VARS-1. At most 32,
                                   so a set of ids fits in one uint32_t mask */
#endif

#ifndef LX_MAX_PAYLOAD
#define LX_MAX_PAYLOAD  64u     /* payload bytes per frame, at most 250 (248 in
                                   bus mode). Sizes the three frame buffers in lx_t */
#endif

#ifndef LX_TIMEOUT_MS
#define LX_TIMEOUT_MS   20u     /* how long to wait for an ACK or read response */
#endif

#ifndef LX_RETRIES
#define LX_RETRIES      3u      /* retransmissions before giving up with LX_EV_TIMEOUT */
#endif

/* ------------------------------------------------------------------ */
/* Optional features                                                   */
/* ------------------------------------------------------------------ */

#ifndef LX_LEAD_DELIM
#define LX_LEAD_DELIM   0       /* 1 = also send 0x00 before each frame: +1 byte
                                   per frame, but line noise can never eat the
                                   next frame. 0 = noise loses at most one more. */
#endif

#ifndef LX_BUS
#define LX_BUS          0       /* 1 = multi-drop bus: +2 address bytes per frame,
                                   node addresses, broadcast. 0 = point-to-point;
                                   the bus code is then compiled out entirely. */
#endif

#ifndef LX_ISR_SAFE
#define LX_ISR_SAFE     0       /* 1 = lx_rx() only queues the byte (lock-free,
                                   safe in any ISR); lx_poll() decodes, replies
                                   and fires events in the caller's context. */
#endif

#ifndef LX_RXQ_SIZE
#define LX_RXQ_SIZE     128u    /* LX_ISR_SAFE queue: power of two, 2..256,
                                   holds LX_RXQ_SIZE - 1 bytes between polls */
#endif

/* ------------------------------------------------------------------ */
/* Critical sections                                                   */
/* ------------------------------------------------------------------ */

/* Needed only when the RX path (lx_rx) runs in a different task or ISR than
 * the rest of the API. Map them to your platform, e.g.
 *   ESP-IDF : portENTER_CRITICAL(&lx_mux) / portEXIT_CRITICAL(&lx_mux)
 *   Zephyr  : k_spinlock_key_t k = k_spin_lock(&l) / k_spin_unlock(&l, k)
 *   Cortex-M: uint32_t pm = __get_PRIMASK(); __disable_irq() / __set_PRIMASK(pm)
 * LX_ENTER_CRITICAL() may declare a local: LYNX always pairs it with one or
 * more LX_EXIT_CRITICAL() in the same or a nested block, and never nests.
 * The default is a no-op, which is right when everything runs in one thread. */
#ifndef LX_ENTER_CRITICAL
#define LX_ENTER_CRITICAL() ((void)0)
#define LX_EXIT_CRITICAL()  ((void)0)
#define LX_ONE_CONTEXT  1       /* no hooks: the app promises a single context */
#endif
#ifndef LX_ONE_CONTEXT
#define LX_ONE_CONTEXT  0
#endif

/* One-byte values handed from one context to the other: the transaction
 * state (h->p_cmd) and the LX_ISR_SAFE queue indices. Acquire/release, so
 * that once lx_busy() reads false, everything the event callback did is
 * visible. Override for compilers without the GNU __atomic builtins, or with
 * plain accesses on a single-core part. */
#ifndef LX_LOAD8
#if defined(__GNUC__)
#define LX_LOAD8(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LX_STORE8(p, v)  __atomic_store_n((p), (uint8_t)(v), __ATOMIC_RELEASE)
#else
#define LX_LOAD8(p)      (*(p))
#define LX_STORE8(p, v)  (*(p) = (uint8_t)(v))
#endif
#endif

/* ------------------------------------------------------------------ */
/* Sanity checks                                                       */
/* ------------------------------------------------------------------ */

#if LX_MAX_VARS > 32u
#error "LX_MAX_VARS must be <= 32"
#endif
#if LX_MAX_PAYLOAD > 250u
#error "LX_MAX_PAYLOAD must be <= 250"
#endif
#if LX_BUS && LX_MAX_PAYLOAD > 248u
#error "LX_MAX_PAYLOAD must be <= 248 with LX_BUS"
#endif
#if LX_ISR_SAFE && (LX_RXQ_SIZE < 2u || LX_RXQ_SIZE > 256u || (LX_RXQ_SIZE & (LX_RXQ_SIZE - 1u)))
#error "LX_RXQ_SIZE must be a power of two in 2..256"
#endif

#endif /* LYNX_CONFIG_H */
