/* Test port (tools/run_tests.sh builds with -DLX_PORT -Itest/port).
 * Enforces the critical-section contract documented in lynx.h:
 *  - LX_ENTER_CRITICAL() declares a local, like real ports may, so lynx.c
 *    fails to compile if a section is opened where that is illegal;
 *  - sections never nest and every path leaves them balanced
 *    (test_lynx.c checks lx_cs_depth == 0 and lx_cs_max == 1);
 *  - lx_cs_exit_hook() runs as each section closes, so a test can fire a
 *    one-shot "interrupt" at exactly that point to provoke races. */
#ifndef LYNX_PORT_H
#define LYNX_PORT_H

extern int lx_cs_depth, lx_cs_max;
extern long lx_cs_count;
void lx_cs_exit_hook(void);

#define LX_ENTER_CRITICAL() int lx_cs_tok_ = ++lx_cs_depth; lx_cs_count++; \
                            if (lx_cs_tok_ > lx_cs_max) lx_cs_max = lx_cs_tok_
#define LX_EXIT_CRITICAL()  ((void)lx_cs_tok_, lx_cs_depth--, lx_cs_exit_hook())

#endif /* LYNX_PORT_H */
