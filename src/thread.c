/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

#include "thread.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>

struct AinWorker {
    HANDLE thread, work, done;
    volatile LONG stop;
    AinWorkFn fn;
    void *arg;
};

unsigned ain_thread_cpu_count(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
}

static unsigned __stdcall worker_main(void *arg)
{
    AinWorker *worker = (AinWorker *)arg;
    for (;;) {
        if (WaitForSingleObject(worker->work, INFINITE) != WAIT_OBJECT_0)
            return 1;
        if (InterlockedCompareExchange(&worker->stop, 0, 0))
            return 0;
        worker->fn(worker->arg);
        if (!SetEvent(worker->done))
            return 1;
    }
}

AinWorker *ain_worker_create(AinWorkFn work, void *arg)
{
    AinWorker *worker = (AinWorker *)calloc(1, sizeof *worker);
    if (!worker)
        return NULL;
    worker->fn = work;
    worker->arg = arg;
    worker->work = CreateEvent(NULL, FALSE, FALSE, NULL);
    worker->done = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (worker->work && worker->done)
        worker->thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, worker, 0, NULL);
    if (!worker->thread) {
        ain_worker_free(worker);
        return NULL;
    }
    return worker;
}

int ain_worker_submit(AinWorker *worker)
{
    return ResetEvent(worker->done) && SetEvent(worker->work) ? 0 : -1;
}

int ain_worker_wait(AinWorker *worker, int wait)
{
    HANDLE handles[2] = {worker->done, worker->thread};
    DWORD result = WaitForMultipleObjects(2, handles, FALSE, wait ? INFINITE : 0);
    if (result == WAIT_TIMEOUT)
        return 0;
    return result == WAIT_OBJECT_0 ? 1 : -1;
}

void ain_worker_free(AinWorker *worker)
{
    if (!worker)
        return;
    if (worker->thread) {
        InterlockedExchange(&worker->stop, 1);
        if (!SetEvent(worker->work) ||
            WaitForSingleObject(worker->thread, INFINITE) != WAIT_OBJECT_0) {
            fprintf(stderr, "aint: failed to stop/join worker\n");
            abort();
        }
        CloseHandle(worker->thread);
    }
    if (worker->work) CloseHandle(worker->work);
    if (worker->done) CloseHandle(worker->done);
    free(worker);
}

#else
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

struct AinWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int pending, done, stop, exited;
    AinWorkFn fn;
    void *arg;
};

unsigned ain_thread_cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count <= 0)
        return 0;
    return (unsigned long)count > UINT_MAX ? UINT_MAX : (unsigned)count;
}

/* A synchronization failure cannot safely release storage still in use. */
static void thread_check(int rc)
{
    if (rc) {
        fprintf(stderr, "aint: pthread synchronization failed: %s\n", strerror(rc));
        abort();
    }
}

static void worker_exited(void *arg)
{
    AinWorker *worker = (AinWorker *)arg;
    thread_check(pthread_mutex_lock(&worker->mutex));
    worker->exited = 1;
    thread_check(pthread_cond_signal(&worker->changed));
    thread_check(pthread_mutex_unlock(&worker->mutex));
}

static void *worker_main(void *arg)
{
    AinWorker *worker = (AinWorker *)arg;
    /* Also publish unexpected pthread_exit() from a callback. */
    pthread_cleanup_push(worker_exited, worker);
    thread_check(pthread_mutex_lock(&worker->mutex));
    for (;;) {
        while (!worker->pending && !worker->stop)
            thread_check(pthread_cond_wait(&worker->changed, &worker->mutex));
        if (worker->stop)
            break;
        worker->pending = 0;
        thread_check(pthread_mutex_unlock(&worker->mutex));
        worker->fn(worker->arg);
        thread_check(pthread_mutex_lock(&worker->mutex));
        worker->done = 1;
        thread_check(pthread_cond_signal(&worker->changed));
    }
    thread_check(pthread_mutex_unlock(&worker->mutex));
    pthread_cleanup_pop(1);
    return NULL;
}

AinWorker *ain_worker_create(AinWorkFn work, void *arg)
{
    AinWorker *worker = (AinWorker *)calloc(1, sizeof *worker);
    if (!worker)
        return NULL;
    worker->fn = work;
    worker->arg = arg;
    if (pthread_mutex_init(&worker->mutex, NULL)) {
        free(worker);
        return NULL;
    }
    if (pthread_cond_init(&worker->changed, NULL)) {
        thread_check(pthread_mutex_destroy(&worker->mutex));
        free(worker);
        return NULL;
    }
    if (pthread_create(&worker->thread, NULL, worker_main, worker)) {
        thread_check(pthread_cond_destroy(&worker->changed));
        thread_check(pthread_mutex_destroy(&worker->mutex));
        free(worker);
        return NULL;
    }
    return worker;
}

int ain_worker_submit(AinWorker *worker)
{
    int rc = 0;
    thread_check(pthread_mutex_lock(&worker->mutex));
    if (worker->exited)
        rc = -1;
    else {
        worker->done = 0;
        worker->pending = 1;
        thread_check(pthread_cond_signal(&worker->changed));
    }
    thread_check(pthread_mutex_unlock(&worker->mutex));
    return rc;
}

int ain_worker_wait(AinWorker *worker, int wait)
{
    int result;
    thread_check(pthread_mutex_lock(&worker->mutex));
    while (wait && !worker->done && !worker->exited)
        thread_check(pthread_cond_wait(&worker->changed, &worker->mutex));
    result = worker->done ? 1 : worker->exited ? -1 : 0;
    thread_check(pthread_mutex_unlock(&worker->mutex));
    return result;
}

void ain_worker_free(AinWorker *worker)
{
    if (!worker)
        return;
    thread_check(pthread_mutex_lock(&worker->mutex));
    worker->stop = 1;
    thread_check(pthread_cond_signal(&worker->changed));
    thread_check(pthread_mutex_unlock(&worker->mutex));
    thread_check(pthread_join(worker->thread, NULL));
    thread_check(pthread_cond_destroy(&worker->changed));
    thread_check(pthread_mutex_destroy(&worker->mutex));
    free(worker);
}
#endif
