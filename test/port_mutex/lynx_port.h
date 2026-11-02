/* Test port for test/test_threads.c: critical sections are a pthread mutex. */
#ifndef LYNX_PORT_H
#define LYNX_PORT_H
#include <pthread.h>

extern pthread_mutex_t lx_test_lock;
#define LX_ENTER_CRITICAL() pthread_mutex_lock(&lx_test_lock)
#define LX_EXIT_CRITICAL()  pthread_mutex_unlock(&lx_test_lock)

#endif /* LYNX_PORT_H */
