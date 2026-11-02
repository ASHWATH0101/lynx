/* LYNX port: Zephyr. Included by lynx.h when the module sets -DLX_PORT.
 * MIT License (c) 2026 Ashwath */
#ifndef LYNX_PORT_H
#define LYNX_PORT_H

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#define LX_MAX_VARS     CONFIG_LYNX_MAX_VARS
#define LX_MAX_PAYLOAD  CONFIG_LYNX_MAX_PAYLOAD
#define LX_TIMEOUT_MS   CONFIG_LYNX_TIMEOUT_MS
#define LX_RETRIES      CONFIG_LYNX_RETRIES
#ifdef CONFIG_LYNX_LEAD_DELIM
#define LX_LEAD_DELIM   1
#endif
#ifdef CONFIG_LYNX_BUS
#define LX_BUS          1
#endif
#ifdef CONFIG_LYNX_ISR_SAFE
#define LX_ISR_SAFE     1
#define LX_RXQ_SIZE     CONFIG_LYNX_RXQ_SIZE
#endif

/* k_spinlock: masks local IRQs and, on SMP, spins against the other CPUs.
 * Usable from threads and ISRs. The key lives in a local of the caller. */
extern struct k_spinlock lx_port_lock;
#define LX_ENTER_CRITICAL() k_spinlock_key_t lx_port_key_ = k_spin_lock(&lx_port_lock)
#define LX_EXIT_CRITICAL()  k_spin_unlock(&lx_port_lock, lx_port_key_)

#endif /* LYNX_PORT_H */
