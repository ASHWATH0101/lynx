/* LYNX port: ESP-IDF. Included by lynx_config.h when the component sets -DLX_PORT.
 * MIT License (c) 2026 Ashwath */
#ifndef LYNX_PORT_H
#define LYNX_PORT_H

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"

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

/* One spinlock for all LYNX handles: sections are a few dozen cycles.
 * The _SAFE variants pick the task or ISR form at run time, so lx_rx may be
 * fed from a UART ISR as well as from a task, on either core. */
extern portMUX_TYPE lx_port_mux;
#define LX_ENTER_CRITICAL() portENTER_CRITICAL_SAFE(&lx_port_mux)
#define LX_EXIT_CRITICAL()  portEXIT_CRITICAL_SAFE(&lx_port_mux)

#endif /* LYNX_PORT_H */
