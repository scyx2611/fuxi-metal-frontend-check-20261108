import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "userland/fuxi-shape/FuxiVibrate.c").read_text(encoding="utf-8")
start = source.index("static pthread_mutex_t vb_lock")
end = source.index("/* ---- VibePattern", start)
worker = source[start:end]
prefix = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "FuxiVibePattern.h"
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define DISPATCH_QUEUE_PRIORITY_DEFAULT 0
static atomic_uint starts, stops, motor, fail_thread, fail_copy, completed;
static uint64_t clock_gettime_nsec_np(int clock_id)
{
    struct timespec now;
    assert(clock_gettime(clock_id, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}
static int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex,
    const struct timespec *relative)
{
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += relative->tv_sec;
    until.tv_nsec += relative->tv_nsec;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &until);
}
static unsigned vb_amp_max;
enum { LEVEL_LOG = 64 };
static atomic_uint levels_logged, levels[LEVEL_LOG];
static bool vb_write(unsigned ms, unsigned level)
{
    assert(ms <= FX_VIBE_REQUEST_MS && level <= 255);
    assert(ms || !level);
    atomic_store(&motor, ms != 0);
    atomic_fetch_add(ms ? &starts : &stops, 1);
    unsigned slot = atomic_fetch_add(&levels_logged, 1);
    if (slot < LEVEL_LOG) atomic_store(&levels[slot], ms ? level + 1000 : 0);
    return true;
}
static void vb_say(const char *format, ...) { (void)format; }
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*body)(void *), void *argument)
{
    if (atomic_exchange(&fail_thread, 0)) return EAGAIN;
    return pthread_create(thread, attributes, body, argument);
}
typedef struct { void (*body)(void *); void *argument; } test_async;
static void *test_async_run(void *argument)
{
    test_async call = *(test_async *)argument;
    free(argument);
    call.body(call.argument);
    return NULL;
}
static void *dispatch_get_global_queue(long priority, unsigned long flags)
{
    (void)priority;
    (void)flags;
    return NULL;
}
static void dispatch_async_f(void *queue, void *argument, void (*body)(void *))
{
    (void)queue;
    test_async *call = malloc(sizeof(*call));
    assert(call);
    *call = (test_async){body, argument};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, test_async_run, call) == 0);
    assert(pthread_detach(thread) == 0);
}
#define pthread_create test_pthread_create
"""
suffix = r"""
#undef pthread_create
#include <stdio.h>
typedef struct {
    vb_completion block;
    atomic_uint copies, calls, releases;
    unsigned reenter;
} test_completion;
static void test_pause(unsigned ms)
{
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000L};
    assert(nanosleep(&delay, NULL) == 0);
}
static void test_invoke(void *argument)
{
    test_completion *block = argument;
    assert(atomic_fetch_add(&block->calls, 1) == 0);
    if (block->reenter) vb_pulse(10);
    atomic_fetch_add(&completed, 1);
}
void *_Block_copy(const void *argument)
{
    if (atomic_exchange(&fail_copy, 0)) return NULL;
    test_completion *block = (test_completion *)argument;
    assert(atomic_fetch_add(&block->copies, 1) == 0);
    return block;
}
void _Block_release(const void *argument)
{
    test_completion *block = (test_completion *)argument;
    assert(atomic_fetch_add(&block->releases, 1) == 0);
}
static void test_wait(atomic_uint *value, unsigned minimum)
{
    for (unsigned attempt = 0; atomic_load(value) < minimum && attempt < 3000; attempt++) test_pause(1);
    assert(atomic_load(value) >= minimum);
}
static void test_done(test_completion *block)
{
    test_wait(&block->releases, 1);
    assert(atomic_load(&block->copies) == 1);
    assert(atomic_load(&block->calls) == 1);
    assert(atomic_load(&block->releases) == 1);
}
int main(void)
{
    fx_vibe_pattern pattern;
    fx_vibe_periodic(&pattern, 30, 20, 75);
    test_completion refused = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    atomic_store(&fail_copy, 1);
    assert(!vb_play(&pattern, &refused));
    assert(!atomic_load(&refused.copies));
    atomic_store(&fail_thread, 1);
    assert(!vb_play(&pattern, &refused));
    assert(atomic_load(&refused.copies) == 1 && atomic_load(&refused.releases) == 1);
    assert(!atomic_load(&refused.calls) && !atomic_load(&starts));
    test_completion normal = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    assert(vb_play(&pattern, &normal));
    test_done(&normal);
    assert(atomic_load(&starts) == 2 && !atomic_load(&motor));
    fx_vibe_periodic(&pattern, 10000, 0, 0);
    test_completion canceled = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    assert(vb_play(&pattern, &canceled));
    test_wait(&starts, 3);
    vb_cancel();
    test_done(&canceled);
    assert(!atomic_load(&motor));
    unsigned after_cancel = atomic_load(&starts);
    test_pause(30);
    assert(atomic_load(&starts) == after_cancel);
    test_completion replaced = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    test_completion next = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 1};
    assert(vb_play(&pattern, &replaced));
    test_wait(&starts, after_cancel + 1);
    fx_vibe_periodic(&pattern, 20, 0, 0);
    assert(vb_play(&pattern, &next));
    test_done(&replaced);
    test_done(&next);
    test_pause(30);
    enum { REQUESTS = 128 };
    test_completion rapid[REQUESTS] = {0};
    fx_vibe_periodic(&pattern, 1000, 0, 0);
    for (unsigned index = 0; index < REQUESTS; index++) {
        rapid[index].block.invoke = test_invoke;
        assert(vb_play(&pattern, &rapid[index]));
    }
    vb_cancel();
    for (unsigned index = 0; index < REQUESTS; index++) test_done(&rapid[index]);
    assert(atomic_load(&completed) == REQUESTS + 4 && !atomic_load(&motor));
    fx_vibe_reset(&pattern);
    test_completion silent = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    after_cancel = atomic_load(&starts);
    assert(vb_play(&pattern, &silent));
    test_done(&silent);
    assert(atomic_load(&starts) == after_cancel);

    /* 振幅分階：相鄰開段只換振幅、中間不停，最後一段結束才停；關段之後重新開 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append_level(&pattern, true, 20, 40) && fx_vibe_append_level(&pattern, true, 20, 255));
    assert(fx_vibe_append_level(&pattern, true, 20, 90) && fx_vibe_append(&pattern, false, 20));
    assert(fx_vibe_append_level(&pattern, true, 20, 60));
    assert(fx_vibe_finish(&pattern) == 80 && pattern.count == 5);
    fx_vibe_scale(&pattern, 200);
    test_completion stepped = {{NULL, 0, 0, test_invoke}, 0, 0, 0, 0};
    unsigned stops_before = atomic_load(&stops);
    atomic_store(&levels_logged, 0);
    assert(vb_play(&pattern, &stepped));
    test_done(&stepped);
    unsigned logged = atomic_load(&levels_logged);
    assert(logged >= 6 && logged < LEVEL_LOG && atomic_load(&stops) - stops_before == 2 && !atomic_load(&motor));
    unsigned expected[] = {1031, 1200, 1071, 0, 1047, 0}, seen = 0;
    for (unsigned index = 0; index < logged; index++) {
        unsigned value = atomic_load(&levels[index]);
        if (seen && value == expected[seen - 1]) continue; /* 同一段的續約 */
        assert(seen < 6 && value == expected[seen]);
        seen++;
    }
    assert(seen == 6);

    /* SpringBoard 輕觸：有上限就帶上限，沒有帶 0 */
    atomic_store(&levels_logged, 0);
    vb_amp_max = 120;
    vb_pulse(15);
    test_wait(&levels_logged, 2);
    assert(atomic_load(&levels[0]) == 1120 && atomic_load(&levels[1]) == 0);
    vb_amp_max = 0;
    test_pause(5);
    atomic_store(&levels_logged, 0);
    vb_pulse(15);
    test_wait(&levels_logged, 2);
    assert(atomic_load(&levels[0]) == 1000 && atomic_load(&levels[1]) == 0);
    puts("PASS: production vibration worker completion ownership, cancel, replacement, reentry, 128 rapid requests and stepped amplitude");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="fuxi-vibe-worker-") as directory:
    test_source = Path(directory) / "worker.c"
    executable = Path(directory) / "worker"
    test_source.write_text(prefix + worker + suffix, encoding="utf-8")
    subprocess.run(
        [os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-pthread", "-I" + str(root / "userland/fuxi-shape"),
         str(test_source), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True, timeout=15)
