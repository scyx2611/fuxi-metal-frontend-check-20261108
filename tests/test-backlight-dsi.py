#!/usr/bin/env python3
"""fuxibacklight=4 host checks against a patched source tree (argument: the ios-fuxi source directory).

1. Structure: mode 4 dispatch, the AppleARMBacklight service publishes backlight-control and never touches hardware, the display
   owner sends the level right before a START, the build hook and the regenerated patches carry the new code.
2. The scanout's DCS path (extracted verbatim from fuxi_gpu_scanout.inc) runs against a simulated DSI controller: exact FIFO words
   and DMA configuration at the trigger, deferral without any register write, register restore after good, failed and uncertain
   commands, halting, a newer level that arrives during a send, MMIO failure and the static-screen retry timer.
3. The service's request/fade arithmetic (fuxi_dsi_backlight_logic.h).
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

src = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
scan = (src / 'xnu/fuxi-adreno/fuxi_gpu_scanout.inc').read_text()
service = (src / 'xnu/fuxi-backlight/FuxiDsiBacklight.cpp').read_text()
old = (src / 'xnu/fuxi-backlight/FuxiBacklight.cpp').read_text()
logic = src / 'xnu/fuxi-backlight/fuxi_dsi_backlight_logic.h'
p0098 = (src / 'xnu/patches/0098-fuxi-native-adreno-abi.patch').read_text()
p0109 = (src / 'xnu/patches/0109-fuxi-backlight.patch').read_text()


def body(code, signature):
    start = code.index(signature)
    brace = code.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (code[end] == '{') - (code[end] == '}')
        end += 1
    return code[start:end]


# 1. Structure.
publish = body(scan, 'static int fg_scanout_publish(void)\n{')
assert publish.index('fg_scanout_mid_allow()') < publish.index('fg_bl_flush(0)') < publish.index('fg_scanout_write(FG_SCAN_CTL+0x1c, 1)')
gate = body(old, 'void fuxi_backlight_publish(IOService *provider)\n{')
assert gate.index('fdbl_enabled(present, enable)') < gate.index('if (!fbl_enabled(present, enable)) return;') < gate.index('new FuxiBacklight')
start = body(service, 'bool AppleARMBacklight::start(IOService *provider)\n{')
assert start.index('fdbl_enabled(present, enable)') < start.index('IOService::start(provider)')
assert start.index('setProperty("backlight-control", kOSBooleanTrue)') < start.index('registerService();')
assert start.index('publishBrightness(FDBL_INITIAL)') < start.index('registerService();')
assert 'class AppleARMBacklight : public IOService' in service
assert 'OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);' in service
setprops = body(service, 'IOReturn AppleARMBacklight::setProperties(OSObject *properties)\n{')
assert 'runAction' not in service and 'setTimeoutUS(1)' in setprops  # never waits for the work loop
assert 'fuxi_gpu_scanout_backlight(fdbl_level(level))' in service
code = re.sub(r'/\*.*?\*/|//[^\n]*', '', service, flags=re.S)
for forbidden in ('ml_io_map', 'IOMemoryDescriptor', 'IOMemoryMap', 'IODeviceMemory', 'volatile uint32_t *', '__asm__', '0xae94000'):
    assert forbidden not in code, forbidden
assert '+iokit/Kernel/arm/FuxiDsiBacklight.cpp' in p0109 and '+#include "fuxi_dsi_backlight_logic.h"' in p0109
assert '+static int fg_bl_flush(unsigned via)' in p0098 and '+ if (!arm_on && fg_bl_flush(0) < 0) return -1;' in p0098

# 2. The DCS path in a simulated controller.
first = scan.index('#define FG_BL_PENDING')
last = scan.index('void fuxi_gpu_scanout_backlight(unsigned level)\n{')
block = scan[first:last] + body(scan, 'void fuxi_gpu_scanout_backlight(unsigned level)\n{') + '\n'
block = block.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FG_SCAN_CTL 0x0ae16000u
#define FG_SCAN_INTF 0x0ae36000u
#define FG_SCAN_DSI 0x0ae94000u
#define FG_SCAN_DSI_TIMEOUT_US 20000u
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct { thread_call_func_t fn; int pending; } thread_call_data_t;
#define THREAD_CALL_PRIORITY_KERNEL 0
#define THREAD_CALL_OPTIONS_ONCE 0
static int setup_calls, enter_calls, delayed_calls, guard_depth, guard_max;
static void thread_call_setup_with_options(thread_call_data_t *c, thread_call_func_t fn, void *p, int pri, int opt)
{ (void)p; (void)pri; (void)opt; c->fn = fn; setup_calls++; }
static int thread_call_enter(thread_call_data_t *c) { c->pending = 1; enter_calls++; return 0; }
static int thread_call_enter_delayed(thread_call_data_t *c, uint64_t d) { (void)d; c->pending = 1; delayed_calls++; return 0; }
static uint64_t mach_absolute_time(void) { return 0; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *r) { *r = ns; }
struct fg_guard { int unused; };
static void fg_guard_exit(struct fg_guard *g) { (void)g; guard_depth--; }
#define FG_GUARD struct fg_guard guard __attribute__((cleanup(fg_guard_exit))) = {0}; (void)guard; if (++guard_depth > guard_max) guard_max = guard_depth

/* Clock: 24 MHz counter; every register access costs 1 tick, delay() advances it. */
static uint64_t now_ticks;
#define rsr(name) (strcmp((name), "cntfrq_el0") == 0 ? 24000000ull : now_ticks)
static void delay(int us) { now_ticks += (uint64_t)us * 24u; }

/* Display state the block reads. */
static unsigned fg_scanout_requested, fg_scanout_panel120_requested, fg_scanout_failed, fg_owned, fg_scanout_panel120_attempted;
static unsigned fg_scanout_ar_state, fg_scanout_mid_dirty, fg_ar_on;
static int fuxi_gpu_scanout_owned(void) { return (int)fg_owned; }
static int fg_scanout_ar_on(void) { return (int)fg_ar_on; }
static int stops;
static int fg_scanout_stop(const char *reason) { (void)reason; stops++; fg_scanout_failed = 1; return -1; }

/* Simulated controller. DSI INT_CTRL 0x110: enables at odd bits, DMA_DONE status bit0 (W1C), error status bit24 (W1C). */
static unsigned dsi[0x400], ctl_start, intf_line, intf_ar;
static unsigned done_bit, err_bit, dma_running, mdp_busy_reads, sticky_done, wedged;
static int done_after_us = 3, raise_error, writes, fail_write_at = -1, reads;
static uint64_t dma_t;
static unsigned fifo[8], fifo_n, trig_fifo[8], trig_fifo_n, trig_tpg, trig_dma, trig_len, trig_trigger, triggers;
static unsigned newer_on_trigger;
static void hw_tick(void)
{
	if (dma_running && done_after_us >= 0 && now_ticks - dma_t >= (uint64_t)done_after_us * 24u) {
		dma_running = 0;
		if (raise_error) err_bit = 1; else done_bit = 1;
	}
}
static int fg_scanout_read(unsigned pa, unsigned *value)
{
	now_ticks++; reads++; hw_tick();
	if (pa == FG_SCAN_CTL + 0x1c) { *value = ctl_start; return 1; }
	if (pa == FG_SCAN_INTF + 0x2b0) { *value = intf_line; return 1; }
	if (pa == FG_SCAN_INTF + 0x2b4) { *value = intf_ar; return 1; }
	assert(pa >= FG_SCAN_DSI && pa < FG_SCAN_DSI + 0x1000 && !(pa & 3));
	unsigned off = pa - FG_SCAN_DSI;
	if (off == 0x110) { *value = (dsi[0x110/4] & 0xaaaaaaaau) | (done_bit || sticky_done) | (err_bit << 24); return 1; }
	if (off == 0x008) {
		unsigned busy = (dma_running || wedged) ? 2u : 0u;
		if (mdp_busy_reads) { mdp_busy_reads--; busy |= 4u; }
		*value = busy; return 1;
	}
	*value = dsi[off/4];
	return 1;
}
static void on_trigger(void);
static int fg_scanout_write(unsigned pa, unsigned value)
{
	now_ticks++; hw_tick();
	if (writes++ == fail_write_at) return 0;
	assert(pa >= FG_SCAN_DSI && pa < FG_SCAN_DSI + 0x1000 && !(pa & 3)); /* the block writes DSI registers only */
	unsigned off = pa - FG_SCAN_DSI;
	if (off == 0x110) {
		dsi[0x110/4] = value & 0xaaaaaaaau;
		if (value & 1u) done_bit = 0;
		if (value & (1u << 24)) err_bit = 0;
		return 1;
	}
	if (off == 0x1ec) { if (value & 1u) { fifo_n = 0; if (!wedged) dma_running = 0; } return 1; }
	if (off == 0x17c) { assert(fifo_n < 8); fifo[fifo_n++] = value; return 1; }
	if (off == 0x090) {
		if (value & 1u) {
			triggers++; dma_running = 1; dma_t = now_ticks;
			memcpy(trig_fifo, fifo, sizeof(fifo)); trig_fifo_n = fifo_n;
			trig_tpg = dsi[0x15c/4]; trig_dma = dsi[0x03c/4]; trig_len = dsi[0x04c/4]; trig_trigger = dsi[0x084/4];
			on_trigger();
		}
		return 1;
	}
	dsi[off/4] = value;
	return 1;
}
'''

MAIN = r'''
static void on_trigger(void)
{
	if (newer_on_trigger) __atomic_store_n(&fg_bl_want, FG_BL_PENDING | newer_on_trigger, __ATOMIC_RELEASE);
}
#define CTRL0 0x00000105u
#define DMA0 0xc4000300u
#define TRIG0 0x80011044u
#define IRQ0 0x0000000au
#define LEN0 0x00000124u
static void reset(void)
{
	memset(dsi, 0, sizeof(dsi));
	dsi[0x004/4] = CTRL0; dsi[0x03c/4] = DMA0; dsi[0x084/4] = TRIG0; dsi[0x110/4] = IRQ0; dsi[0x04c/4] = LEN0;
	ctl_start = 0; intf_line = 0; intf_ar = 0;
	done_bit = err_bit = dma_running = mdp_busy_reads = sticky_done = wedged = 0;
	done_after_us = 3; raise_error = 0; writes = 0; fail_write_at = -1; reads = 0;
	fifo_n = trig_fifo_n = 0; triggers = 0; newer_on_trigger = 0;
	fg_scanout_requested = fg_scanout_panel120_requested = fg_owned = fg_scanout_panel120_attempted = 1;
	fg_scanout_failed = fg_scanout_ar_state = fg_scanout_mid_dirty = fg_ar_on = 0;
	stops = 0;
	fg_bl_want = 0; fg_bl_tries = 0; fg_bl_halted = 0; fg_bl_sent = 0; fg_bl_said = 0;
	memset(fg_bl_defer, 0, sizeof(fg_bl_defer));
}
static void assert_restored(void)
{
	assert(dsi[0x03c/4] == DMA0 && dsi[0x04c/4] == LEN0 && dsi[0x084/4] == TRIG0);
	assert(dsi[0x15c/4] == 0 && (dsi[0x110/4] & 0xaaaaaaaau) == IRQ0 && fifo_n == 0);
}
static void request(unsigned level) { fuxi_gpu_scanout_backlight(level); }
static unsigned pending(void) { return __atomic_load_n(&fg_bl_want, __ATOMIC_ACQUIRE); }
int main(void)
{
	uint32_t w[2];
	fg_bl_words(0x5a3, w);
	assert(w[0] == 0xc0390003u && w[1] == 0xffa30551u); /* 0x51, high byte 0x05, low byte 0xa3, pad */
	fg_bl_words(2047, w);
	assert(w[1] == 0xffff0751u);

	/* Request: clamp, one-time thread call setup, an immediate timer kick. */
	reset();
	request(5000);
	assert(pending() == (FG_BL_PENDING | 2047u) && setup_calls == 1 && enter_calls == 1);
	request(300);
	assert(pending() == (FG_BL_PENDING | 300u) && setup_calls == 1 && enter_calls == 2);

	/* Nothing waiting: no register access. */
	reset();
	assert(fg_bl_flush(0) == 1 && reads == 0 && writes == 0);

	/* Deferrals touch nothing (and keep the level). */
	struct { unsigned *flag; unsigned value; unsigned bucket; } cases[] = {
		{&fg_owned, 0, FG_BL_D_OWNER}, {&fg_scanout_requested, 0, FG_BL_D_OWNER},
		{&fg_scanout_panel120_requested, 0, FG_BL_D_OWNER}, {&fg_scanout_failed, 1, FG_BL_D_OWNER},
		{&fg_scanout_panel120_attempted, 0, FG_BL_D_PANEL120}, {&fg_ar_on, 1, FG_BL_D_AR},
		{&fg_scanout_ar_state, 4, FG_BL_D_AR}, {&fg_scanout_mid_dirty, 1, FG_BL_D_AR},
		{&ctl_start, 1, FG_BL_D_XFER}, {&intf_line, 1200, FG_BL_D_XFER}, {&intf_ar, 0x80000000u, FG_BL_D_XFER},
	};
	for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		reset(); request(700); *cases[i].flag = cases[i].value;
		assert(fg_bl_flush(0) == 0 && writes == 0 && triggers == 0 && stops == 0);
		assert(fg_bl_defer[cases[i].bucket] == 1 && pending() == (FG_BL_PENDING | 700u));
	}
	/* Controller not ready: command mode off, TPG in use, pending error, MDP busy past the bound. */
	unsigned *dsi_flags[] = {&dsi[0x004/4], &dsi[0x15c/4], &err_bit, &mdp_busy_reads};
	unsigned dsi_values[] = {0x101u, 0x30006u, 1u, 100000u};
	for (unsigned i = 0; i < 4; i++) {
		reset(); request(700); *dsi_flags[i] = dsi_values[i];
		assert(fg_bl_flush(1) == 0 && writes == 0 && triggers == 0 && stops == 0);
		assert(fg_bl_defer[FG_BL_D_DSI] == 1 && pending() == (FG_BL_PENDING | 700u));
	}

	/* Good send: write line idle at 0 or past the frame, MDP still busy for a few reads. */
	unsigned lines[] = {0, 2400, 2683};
	for (unsigned i = 0; i < 3; i++) {
		reset(); request(0x5a3); intf_line = lines[i]; mdp_busy_reads = 3;
		assert(fg_bl_flush(0) == 1 && stops == 0 && triggers == 1 && !fg_bl_halted);
		assert(trig_fifo_n == 2 && trig_fifo[0] == 0xc0390003u && trig_fifo[1] == 0xffa30551u);
		assert(trig_tpg == 0x00030006u && trig_len == 8);
		assert(trig_dma == ((DMA0 & ~0xc4000000u) | 0x10000000u));
		assert(trig_trigger == ((TRIG0 & ~0x1100fu) | 4u) && (trig_trigger >> 31));
		assert_restored();
		assert(!(pending() & FG_BL_PENDING) && fg_bl_sent == 1 && !done_bit);
		assert(fg_bl_flush(0) == 1 && triggers == 1); /* sent once */
	}

	/* A newer level that arrives during the send stays pending. */
	reset(); request(400); newer_on_trigger = 900;
	assert(fg_bl_flush(0) == 1 && triggers == 1 && pending() == (FG_BL_PENDING | 900u));
	newer_on_trigger = 0;
	assert(fg_bl_flush(0) == 1 && triggers == 2 && trig_fifo[1] == 0xff840351u && !(pending() & FG_BL_PENDING));

	/* A stale DMA-done that will not clear: no trigger, registers restored, deferred. */
	reset(); request(400); sticky_done = 1;
	assert(fg_bl_flush(0) == 0 && triggers == 0 && stops == 0 && !fg_bl_halted);
	assert_restored(); assert(pending() == (FG_BL_PENDING | 400u) && fg_bl_defer[FG_BL_D_DSI] == 1);

	/* Error raised: restored, halted for the boot, later levels dropped without register access. */
	reset(); request(400); raise_error = 1;
	assert(fg_bl_flush(0) == 1 && triggers == 1 && fg_bl_halted && stops == 0);
	assert_restored(); assert(!(pending() & FG_BL_PENDING));
	writes = reads = 0; request(800);
	assert(fg_bl_flush(0) == 1 && writes == 0 && reads == 0 && !(pending() & FG_BL_PENDING));

	/* No completion, the FIFO reset idles the engine: restored and halted, scanout keeps running. */
	reset(); request(400); done_after_us = -1;
	assert(fg_bl_flush(0) == 1 && triggers == 1 && fg_bl_halted && stops == 0);
	assert_restored();

	/* No completion and the controller stays busy: scanout stops (panel held). */
	reset(); request(400); done_after_us = -1; wedged = 1;
	assert(fg_bl_flush(0) == -1 && fg_bl_halted && stops == 1);

	/* A failing MMIO write at any step of a send stops scanout. */
	reset(); request(400);
	assert(fg_bl_flush(0) == 1 && stops == 0);
	int send_writes = writes;
	assert(send_writes == 18); /* 10 to trigger, 8 to restore */
	for (int n = 0; n < send_writes; n++) {
		reset(); request(400); fail_write_at = n;
		assert(fg_bl_flush(0) == -1 && stops == 1);
	}

	/* Static-screen timer: retries while deferred, gives up after FG_BL_TRIES, sends when possible. */
	reset(); request(600); fg_owned = 0;
	delayed_calls = 0; guard_max = 0;
	for (unsigned i = 0; i < FG_BL_TRIES; i++) fg_bl_run(NULL, NULL);
	assert(delayed_calls == (int)FG_BL_TRIES - 1 && fg_bl_defer[FG_BL_D_GAVE_UP] == 1 && guard_depth == 0 && guard_max == 1);
	fg_owned = 1; delayed_calls = 0; request(600);
	fg_bl_run(NULL, NULL);
	assert(delayed_calls == 0 && triggers == 1 && !(pending() & FG_BL_PENDING) && guard_depth == 0);
	puts("PASS: fuxibacklight=4 DCS path (simulated controller)");
	return 0;
}
'''

LOGIC = r'''
#include <assert.h>
#include <stdio.h>
#include "fuxi_dsi_backlight_logic.h"
int main(void)
{
	for (int v = -2; v < 8; v++) assert(fdbl_enabled(1, v) == (v == 4) && !fdbl_enabled(0, v));
	assert(fdbl_goal(5, 32) == 5 && fdbl_goal(2047, 32) == 2047 && fdbl_goal(2048, 32) == 2047);
	assert(fdbl_goal(0xffffffffu, 32) == 0 && fdbl_goal(0x80000000u, 32) == 0 && fdbl_goal(0x7fffffffu, 32) == 2047);
	assert(fdbl_goal(1ull << 63, 64) == 0 && fdbl_goal(1000, 64) == 1000 && fdbl_goal(0xff, 8) == 0 && fdbl_goal(0x7f, 8) == 127);
	assert(fdbl_fade_ms(0) == 0 && fdbl_fade_ms(0x10000) == 1000 && fdbl_fade_ms(0x8000) == 500);
	assert(fdbl_fade_ms(5u << 16) == 5000 && fdbl_fade_ms(6u << 16) == 5000 && fdbl_fade_ms(~0ull) == 5000);
	assert(fdbl_level(0) == 8 && fdbl_level(7) == 8 && fdbl_level(8) == 8 && fdbl_level(2047) == 2047 && fdbl_level(4000) == 2047);
	for (uint32_t g = 0; g <= 2047; g += 89)
		for (uint32_t ms = 0; ms <= 5000; ms += 1250) {
			uint64_t r = fdbl_request(g, ms);
			assert((r & FDBL_PENDING) && fdbl_request_goal(r) == g && fdbl_request_ms(r) == ms);
		}
	uint32_t pairs[][2] = {{100, 2000}, {2000, 100}, {500, 500}, {0, 2047}};
	for (unsigned p = 0; p < 4; p++) {
		uint32_t a = pairs[p][0], b = pairs[p][1], prev = a;
		assert(fdbl_fade_at(a, b, 0, 1000) == a && fdbl_fade_at(a, b, 1000, 1000) == b && fdbl_fade_at(a, b, 5, 0) == b);
		for (uint64_t t = 0; t <= 1000; t += 16) {
			uint32_t v = fdbl_fade_at(a, b, t, 1000);
			assert(a <= b ? (v >= prev && v <= b) : (v <= prev && v >= b));
			prev = v;
		}
	}
	/* 5 s at a 24 MHz timebase cannot overflow the interpolation. */
	assert(fdbl_fade_at(0, 2047, 60000000ull, 120000000ull) == 1023);
	assert(fdbl_log_this(1) && fdbl_log_this(64) && !fdbl_log_this(65) && fdbl_log_this(256) && !fdbl_log_this(0));
	puts("PASS: fuxi_dsi_backlight_logic.h");
	return 0;
}
'''

# FUXI_TEST_CC, FUXI_TEST_SANFLAGS (e.g. trap-only UBSan on musl), FUXI_TEST_SYNTAX_ONLY=1 (with FUXI_TEST_CFLAGS, e.g. a
# cross --target): compile-only check on a machine
# without a native compiler. CI runs the executables.
cc = os.environ.get('FUXI_TEST_CC', 'clang')
syntax_only = os.environ.get('FUXI_TEST_SYNTAX_ONLY') == '1'
with tempfile.TemporaryDirectory(prefix='backlight-dsi-') as tmp:
    for name, text, extra in (('dcs.c', PRELUDE + block + MAIN, []), ('logic.c', LOGIC, ['-I', str(logic.parent)])):
        path, exe = Path(tmp) / name, Path(tmp) / (name + '.out')
        path.write_text(text, newline='\n')
        flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *extra]
        if syntax_only:
            subprocess.run([cc, *flags, *os.environ.get('FUXI_TEST_CFLAGS', '').split(), '-fsyntax-only', str(path)], check=True)
            continue
        sanitize = os.environ.get('FUXI_TEST_SANFLAGS', '-fsanitize=address,undefined -fno-sanitize-recover=all').split()
        subprocess.run([cc, *flags, *sanitize, str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('PASS: fuxibacklight=4 structure' + (', compile-only' if syntax_only else ', DCS path and service arithmetic'))
