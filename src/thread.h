/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* Private, single-coordinator worker interface. */
#ifndef AIN_THREAD_H
#define AIN_THREAD_H

typedef struct AinWorker AinWorker;
typedef void (*AinWorkFn)(void *arg);

/* Zero means the processor count could not be queried. */
unsigned ain_thread_cpu_count(void);

/* Create an idle worker; NULL on allocation or platform failure. */
AinWorker *ain_worker_create(AinWorkFn work, void *arg);
/* Submit after collecting the previous result. Returns 0 or -1 on failure. */
int ain_worker_submit(AinWorker *worker);
/* Returns 1 when complete, 0 when not ready, -1 on failure/worker exit.
 * A successful wait publishes all writes made by the callback.
 * Completion remains observable until the next submission. */
int ain_worker_wait(AinWorker *worker, int wait);
/* Finish any running callback, stop, join and release; NULL is allowed.
 * The callback and its argument must remain valid until this returns. */
void ain_worker_free(AinWorker *worker);

#endif
