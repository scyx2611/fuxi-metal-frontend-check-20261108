/*
 * Plan B iOS-side RTC offset persistence inside /usr/libexec/timed (libfuxi-sb.dylib).
 *
 * Design & references (AGENTS.md section 0):
 *  - Reuses the libfuxi-sb.dylib constructor, ADT /chosen reader, DYLD_INSERT_LIBRARIES
 *    unset, and stderr logging conventions from FuxiShape.c / FuxiCaptureStub.c, staying
 *    strictly within the 4-library link set verified by verify-sb-shape.py
 *    (libSystem, libobjc, CoreFoundation, IOKit).
 *  - Reads live PMIC PMK8550 rtc@6100 seconds exclusively through the read-only kernel
 *    sysctls kern.fuxirtc_info and kern.fuxirtc_raw (xnu/fuxi-h1/fuxi_keys.c).
 *    Never writes Android /persist or PMIC RTC hardware registers.
 *  - Watches XNU's native HOST_NOTIFY_CALENDAR_SET Mach notification fired by
 *    clock_set_calendar_microtime() (osfmk/kern/clock.c -> host_notify.c) on every
 *    settimeofday(), with a 5 s poll fallback for gradual slew.
 *  - Gated on ADT /chosen u32 "fuxi,rtc-user" (0 = off, 1 = Plan B, 3 = Plan B +
 *    one-shot non-destructive self-test). Kernel injection requires ADT fuxi,inject
 *    bit 8 (FUXI_INJECT_TIMED = 256).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/host_notify.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "FuxiRtcPersistLogic.h"

typedef mach_port_t fx_io_object_t;
extern fx_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_io_object_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(fx_io_object_t object);

#define RTC_SELF_LIB         "/usr/local/lib/fuxi/libfuxi-sb.dylib"
#define RTC_POLL_TIMEOUT_MS  5000u

static const char *const rtc_store_paths[] = {
	"/private/var/db/timed/fuxi-rtc-offset.bin",
	"/private/var/db/timed/Library/Preferences/fuxi-rtc-offset.bin",
	"/private/var/db/fuxi-rtc-offset.bin",
};
#define RTC_STORE_PATH_COUNT (sizeof(rtc_store_paths) / sizeof(rtc_store_paths[0]))

static pthread_mutex_t rtc_state_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t rtc_mode;
static uint32_t rtc_apid = FUXI_RTC_MAX_APID;
static bool rtc_has_anchor;
static bool rtc_raw_reset_pending;
static struct fuxi_rtc_store_rec rtc_anchor;
static char rtc_active_path[160];
static uint32_t rtc_cal_notify_count;
static int rtc_watcher_armed;

static void
rtc_say(const char *format, ...)
{
	char line[320];
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (length > (int)sizeof(line) - 1) {
		length = (int)sizeof(line) - 1;
	}
	if (length > 0) {
		(void)write(2, line, (size_t)length);
	}
}

static void
rtc_sleep_ms(unsigned millis)
{
	struct timespec delay;
	delay.tv_sec = (time_t)(millis / 1000u);
	delay.tv_nsec = (long)((millis % 1000u) * 1000000u);
	(void)nanosleep(&delay, NULL);
}

static int64_t
rtc_now_utc_ms(void)
{
	struct timeval now_tv;
	if (gettimeofday(&now_tv, NULL) != 0) {
		return 0;
	}
	return (int64_t)now_tv.tv_sec * 1000LL + (int64_t)(now_tv.tv_usec / 1000);
}

static bool
rtc_sysctl_read_info(struct fuxi_rtc_info *out_info, int *errno_out)
{
	size_t size = sizeof(*out_info);
	memset(out_info, 0, sizeof(*out_info));
	if (sysctlbyname("kern.fuxirtc_info", out_info, &size, NULL, 0) != 0) {
		if (errno_out != NULL) {
			*errno_out = errno;
		}
		return false;
	}
	if (size != sizeof(*out_info)) {
		if (errno_out != NULL) {
			*errno_out = EINVAL;
		}
		return false;
	}
	return true;
}

static bool
rtc_sysctl_read_raw(uint32_t *out_raw, int *errno_out)
{
	uint32_t raw_val = 0u;
	size_t size = sizeof(raw_val);
	if (sysctlbyname("kern.fuxirtc_raw", &raw_val, &size, NULL, 0) != 0) {
		if (errno_out != NULL) {
			*errno_out = errno;
		}
		return false;
	}
	if (size != sizeof(raw_val)) {
		if (errno_out != NULL) {
			*errno_out = EINVAL;
		}
		return false;
	}
	*out_raw = raw_val;
	return true;
}

/*
 * Reads ADT /chosen u32 "fuxi,rtc-user".
 * Returns:
 *   1 if property found (value in *out_val),
 *   0 if IORegistry readable and property absent,
 *  -1 if IORegistry:/chosen itself was inaccessible (e.g. sandbox).
 */
static int
rtc_read_adt_mode(uint32_t *out_val)
{
	fx_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	CFTypeRef prop;
	uint32_t val = 0u;
	bool got = false;

	if (chosen == MACH_PORT_NULL) {
		return -1;
	}
	prop = IORegistryEntryCreateCFProperty(chosen, CFSTR("fuxi,rtc-user"), kCFAllocatorDefault, 0);
	(void)IOObjectRelease(chosen);
	if (prop == NULL) {
		return 0;
	}
	if (CFGetTypeID(prop) == CFDataGetTypeID() &&
	    CFDataGetLength((CFDataRef)prop) == (CFIndex)sizeof(val)) {
		memcpy(&val, CFDataGetBytePtr((CFDataRef)prop), sizeof(val));
		got = true;
	}
	CFRelease(prop);
	if (!got) {
		return 0;
	}
	*out_val = val;
	return 1;
}

static bool
rtc_store_read_file(const char *path, struct fuxi_rtc_store_rec *out_rec,
    enum fuxi_rtc_store_status *status_out, int *errno_out)
{
	uint8_t buf[FUXI_RTC_STORE_SIZE + 1u];
	size_t total = 0u;
	int fd = open(path, O_RDONLY | O_NOFOLLOW);

	if (fd < 0) {
		if (errno_out != NULL) {
			*errno_out = errno;
		}
		return false;
	}
	while (total < sizeof(buf)) {
		ssize_t got = read(fd, buf + total, sizeof(buf) - total);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			int saved_err = errno;
			(void)close(fd);
			if (errno_out != NULL) {
				*errno_out = saved_err;
			}
			return false;
		}
		if (got == 0) {
			break;
		}
		total += (size_t)got;
	}
	(void)close(fd);
	enum fuxi_rtc_store_status st = fuxi_rtc_store_decode(buf, total, out_rec);
	if (status_out != NULL) {
		*status_out = st;
	}
	if (errno_out != NULL) {
		*errno_out = 0;
	}
	return st == FUXI_RTC_STORE_OK;
}

static bool
rtc_store_read_best(struct fuxi_rtc_store_rec *out_rec, char *path_out, size_t path_cap)
{
	bool found = false;
	struct fuxi_rtc_store_rec best;
	const char *best_path = NULL;

	memset(&best, 0, sizeof(best));
	for (size_t idx = 0; idx < RTC_STORE_PATH_COUNT; idx++) {
		struct fuxi_rtc_store_rec candidate;
		enum fuxi_rtc_store_status st = FUXI_RTC_STORE_OK;
		int err = 0;
		if (rtc_store_read_file(rtc_store_paths[idx], &candidate, &st, &err)) {
			if (!found || candidate.generation > best.generation) {
				best = candidate;
				best_path = rtc_store_paths[idx];
				found = true;
			}
		} else if (err == 0 && st != FUXI_RTC_STORE_OK) {
			rtc_say("fuxi: timed: rtc-b corrupt store ignored path=%s status=%d\n",
			    rtc_store_paths[idx], (int)st);
		}
	}
	if (found) {
		*out_rec = best;
		if (path_out != NULL && path_cap > 0u) {
			(void)snprintf(path_out, path_cap, "%s", best_path);
		}
	}
	return found;
}

static bool
rtc_write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t written = 0u;
	while (written < len) {
		ssize_t chunk = write(fd, buf + written, len - written);
		if (chunk < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		written += (size_t)chunk;
	}
	return true;
}

static bool
rtc_store_write_one(const char *path, const struct fuxi_rtc_store_rec *rec, int *errno_out)
{
	uint8_t raw_buf[FUXI_RTC_STORE_SIZE];
	char dir_path[160];
	char tmp_path[192];
	const char *last_slash;
	int fd;
	int dir_fd;

	if (!fuxi_rtc_store_encode(rec, raw_buf)) {
		if (errno_out != NULL) {
			*errno_out = EINVAL;
		}
		return false;
	}
	last_slash = strrchr(path, '/');
	if (last_slash == NULL || (size_t)(last_slash - path) >= sizeof(dir_path)) {
		if (errno_out != NULL) {
			*errno_out = EINVAL;
		}
		return false;
	}
	memcpy(dir_path, path, (size_t)(last_slash - path));
	dir_path[last_slash - path] = '\0';
	(void)mkdir(dir_path, 0755);

	int tmp_len = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
	if (tmp_len <= 0 || (size_t)tmp_len >= sizeof(tmp_path)) {
		if (errno_out != NULL) {
			*errno_out = ENAMETOOLONG;
		}
		return false;
	}

	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0) {
		if (errno_out != NULL) {
			*errno_out = errno;
		}
		return false;
	}
	if (!rtc_write_all(fd, raw_buf, sizeof(raw_buf))) {
		int saved_err = errno;
		(void)close(fd);
		(void)unlink(tmp_path);
		if (errno_out != NULL) {
			*errno_out = saved_err;
		}
		return false;
	}
	if (fsync(fd) != 0) {
		int saved_err = errno;
		(void)close(fd);
		(void)unlink(tmp_path);
		if (errno_out != NULL) {
			*errno_out = saved_err;
		}
		return false;
	}
	(void)fcntl(fd, F_FULLFSYNC);
	if (close(fd) != 0) {
		int saved_err = errno;
		(void)unlink(tmp_path);
		if (errno_out != NULL) {
			*errno_out = saved_err;
		}
		return false;
	}
	if (rename(tmp_path, path) != 0) {
		int saved_err = errno;
		(void)unlink(tmp_path);
		if (errno_out != NULL) {
			*errno_out = saved_err;
		}
		return false;
	}
	dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
	if (dir_fd >= 0) {
		(void)fsync(dir_fd);
		(void)fcntl(dir_fd, F_FULLFSYNC);
		(void)close(dir_fd);
	}
	sync();
	if (errno_out != NULL) {
		*errno_out = 0;
	}
	return true;
}

static bool
rtc_store_persist(const struct fuxi_rtc_store_rec *rec, char *used_path, size_t used_cap)
{
	char preferred[160];
	int last_err = 0;

	pthread_mutex_lock(&rtc_state_lock);
	(void)snprintf(preferred, sizeof(preferred), "%s", rtc_active_path);
	pthread_mutex_unlock(&rtc_state_lock);

	if (preferred[0] != '\0') {
		if (rtc_store_write_one(preferred, rec, &last_err)) {
			if (used_path != NULL && used_cap > 0u) {
				(void)snprintf(used_path, used_cap, "%s", preferred);
			}
			return true;
		}
	}

	for (size_t idx = 0; idx < RTC_STORE_PATH_COUNT; idx++) {
		if (preferred[0] != '\0' && strcmp(preferred, rtc_store_paths[idx]) == 0) {
			continue;
		}
		if (rtc_store_write_one(rtc_store_paths[idx], rec, &last_err)) {
			pthread_mutex_lock(&rtc_state_lock);
			(void)snprintf(rtc_active_path, sizeof(rtc_active_path), "%s", rtc_store_paths[idx]);
			pthread_mutex_unlock(&rtc_state_lock);
			if (used_path != NULL && used_cap > 0u) {
				(void)snprintf(used_path, used_cap, "%s", rtc_store_paths[idx]);
			}
			return true;
		}
		rtc_say("fuxi: timed: rtc-b write candidate failed path=%s errno=%d\n",
		    rtc_store_paths[idx], last_err);
	}
	return false;
}

static void
rtc_handle_sample(bool is_calendar_event)
{
	uint32_t raw_now = 0u;
	int sysctl_err = 0;
	int64_t now_utc_ms;
	struct fuxi_rtc_store_rec snapshot;
	struct fuxi_rtc_store_rec next_rec;
	bool had_anchor;
	bool reset_pending;
	uint32_t apid_local;
	int64_t delta_ms = 0;
	enum fuxi_rtc_change_action action;

	if (!rtc_sysctl_read_raw(&raw_now, &sysctl_err)) {
		if (is_calendar_event) {
			rtc_say("fuxi: timed: rtc-b event=calendar_set raw-read failed errno=%d\n",
			    sysctl_err);
		}
		return;
	}
	now_utc_ms = rtc_now_utc_ms();

	pthread_mutex_lock(&rtc_state_lock);
	had_anchor = rtc_has_anchor;
	reset_pending = rtc_raw_reset_pending;
	apid_local = rtc_apid;
	snapshot = rtc_anchor;
	pthread_mutex_unlock(&rtc_state_lock);

	action = fuxi_rtc_change_decide(is_calendar_event, apid_local, raw_now, now_utc_ms,
	    had_anchor, reset_pending, had_anchor ? &snapshot : NULL, &next_rec, &delta_ms);

	if (action == FUXI_RTC_CHANGE_PERSIST) {
		char saved_path[160];
		saved_path[0] = '\0';
		if (rtc_store_persist(&next_rec, saved_path, sizeof(saved_path))) {
			pthread_mutex_lock(&rtc_state_lock);
			rtc_anchor = next_rec;
			rtc_has_anchor = true;
			rtc_raw_reset_pending = false;
			if (is_calendar_event) {
				rtc_cal_notify_count++;
			}
			pthread_mutex_unlock(&rtc_state_lock);
			rtc_say("fuxi: timed: rtc-b event=%s persisted gen=%llu src=%u raw=%u offset_ms=%lld utc=%lld.%03lld delta_ms=%lld path=%s\n",
			    is_calendar_event ? "calendar_set" : "poll",
			    (unsigned long long)next_rec.generation,
			    (unsigned)next_rec.source,
			    (unsigned)next_rec.saved_raw,
			    (long long)next_rec.saved_offset_ms,
			    (long long)(next_rec.saved_utc_ms / 1000LL),
			    (long long)(next_rec.saved_utc_ms % 1000LL),
			    (long long)delta_ms,
			    saved_path);
		} else {
			pthread_mutex_lock(&rtc_state_lock);
			if (is_calendar_event) {
				rtc_cal_notify_count++;
			}
			pthread_mutex_unlock(&rtc_state_lock);
			rtc_say("fuxi: timed: rtc-b event=%s persist FAILED gen=%llu\n",
			    is_calendar_event ? "calendar_set" : "poll",
			    (unsigned long long)next_rec.generation);
		}
		return;
	}

	if (is_calendar_event) {
		pthread_mutex_lock(&rtc_state_lock);
		rtc_cal_notify_count++;
		pthread_mutex_unlock(&rtc_state_lock);
		rtc_say("fuxi: timed: rtc-b event=calendar_set action=%s raw=%u utc_ms=%lld delta_ms=%lld\n",
		    action == FUXI_RTC_CHANGE_SKIP_DEADBAND ? "skip-deadband" : "ignore-unsafe",
		    (unsigned)raw_now, (long long)now_utc_ms, (long long)delta_ms);
	}
}

static void *
rtc_watcher_thread(void *context)
{
	mach_port_t notify_port = MACH_PORT_NULL;
	host_t host_port = mach_host_self();
	kern_return_t kr;
	unsigned poll_ticks = 0u;
	bool logged_verify10s = false;

	(void)context;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &notify_port);
	if (kr != KERN_SUCCESS) {
		rtc_say("fuxi: timed: rtc-b mach_port_allocate failed kr=0x%x\n", (unsigned)kr);
		(void)mach_port_deallocate(mach_task_self(), host_port);
		return NULL;
	}
	kr = host_request_notification(host_port, HOST_NOTIFY_CALENDAR_SET, notify_port);
	if (kr != KERN_SUCCESS) {
		rtc_say("fuxi: timed: rtc-b host_request_notification failed kr=0x%x\n", (unsigned)kr);
	} else {
		rtc_say("fuxi: timed: rtc-b watcher armed (HOST_NOTIFY_CALENDAR_SET + 5s poll)\n");
	}
	__atomic_store_n(&rtc_watcher_armed, 1, __ATOMIC_RELEASE);

	for (;;) {
		struct {
			mach_msg_header_t hdr;
			uint8_t pad[128];
		} msg;

		memset(&msg, 0, sizeof(msg));
		kr = mach_msg(&msg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, (mach_msg_size_t)sizeof(msg), notify_port,
		    RTC_POLL_TIMEOUT_MS, MACH_PORT_NULL);

		if (kr == KERN_SUCCESS) {
			kern_return_t rearm = host_request_notification(host_port,
			    HOST_NOTIFY_CALENDAR_SET, notify_port);
			if (rearm != KERN_SUCCESS) {
				rtc_say("fuxi: timed: rtc-b re-arm failed kr=0x%x\n", (unsigned)rearm);
			}
			bool is_cal_set = (msg.hdr.msgh_id == HOST_CALENDAR_SET_REPLYID);
			rtc_handle_sample(is_cal_set);
			continue;
		}
		if (kr == MACH_RCV_TIMED_OUT) {
			poll_ticks++;
			rtc_handle_sample(false);
			if (!logged_verify10s && poll_ticks >= 2u) {
				uint32_t raw_now = 0u;
				int64_t now_ms = rtc_now_utc_ms();
				if (rtc_sysctl_read_raw(&raw_now, NULL)) {
					struct fuxi_rtc_store_rec snap;
					bool has_snap;
					char path_copy[160];
					pthread_mutex_lock(&rtc_state_lock);
					has_snap = rtc_has_anchor;
					snap = rtc_anchor;
					(void)snprintf(path_copy, sizeof(path_copy), "%s", rtc_active_path);
					pthread_mutex_unlock(&rtc_state_lock);
					rtc_say("fuxi: timed: rtc-b verify10s utc=%lld.%03lld raw=%u anchor=%u gen=%llu offset_ms=%lld path=%s\n",
					    (long long)(now_ms / 1000LL),
					    (long long)(now_ms % 1000LL),
					    (unsigned)raw_now,
					    has_snap ? 1u : 0u,
					    has_snap ? (unsigned long long)snap.generation : 0ull,
					    has_snap ? (long long)snap.saved_offset_ms : 0LL,
					    path_copy[0] ? path_copy : "(none)");
				}
				logged_verify10s = true;
			}
			continue;
		}
		rtc_say("fuxi: timed: rtc-b mach_msg err=0x%x\n", (unsigned)kr);
		rtc_sleep_ms(1000u);
		(void)host_request_notification(host_port, HOST_NOTIFY_CALENDAR_SET, notify_port);
	}
	return NULL;
}

static bool
rtc_wait_notify_and_gen(uint32_t prev_notify, uint64_t prev_gen, unsigned timeout_ms,
    uint32_t *notify_out, uint64_t *gen_out, int64_t *offset_out)
{
	unsigned waited = 0u;
	while (waited <= timeout_ms) {
		pthread_mutex_lock(&rtc_state_lock);
		uint32_t cur_notify = rtc_cal_notify_count;
		bool has_snap = rtc_has_anchor;
		uint64_t cur_gen = has_snap ? rtc_anchor.generation : 0ull;
		int64_t cur_off = has_snap ? rtc_anchor.saved_offset_ms : 0LL;
		pthread_mutex_unlock(&rtc_state_lock);

		if (cur_notify > prev_notify && cur_gen > prev_gen) {
			if (notify_out != NULL) *notify_out = cur_notify;
			if (gen_out != NULL) *gen_out = cur_gen;
			if (offset_out != NULL) *offset_out = cur_off;
			return true;
		}
		rtc_sleep_ms(50u);
		waited += 50u;
	}
	return false;
}

static void *
rtc_selftest_thread(void *context)
{
	(void)context;
	for (unsigned wait_idx = 0; wait_idx < 100u; wait_idx++) {
		if (__atomic_load_n(&rtc_watcher_armed, __ATOMIC_ACQUIRE) != 0) {
			break;
		}
		rtc_sleep_ms(50u);
	}
	rtc_sleep_ms(6000u);

	rtc_say("fuxi: timed: rtc-b selftest start\n");

	uint32_t dummy_raw = 12345u;
	errno = 0;
	int wr_raw = sysctlbyname("kern.fuxirtc_raw", NULL, NULL, &dummy_raw, sizeof(dummy_raw));
	int wr_raw_err = errno;
	struct fuxi_rtc_info dummy_info;
	memset(&dummy_info, 0, sizeof(dummy_info));
	errno = 0;
	int wr_info = sysctlbyname("kern.fuxirtc_info", NULL, NULL, &dummy_info, sizeof(dummy_info));
	int wr_info_err = errno;
	if (wr_raw == 0 || (wr_raw_err != EPERM && wr_raw_err != EACCES) ||
	    wr_info == 0 || (wr_info_err != EPERM && wr_info_err != EACCES)) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=ro-sysctl wr_raw=%d/%d wr_info=%d/%d\n",
		    wr_raw, wr_raw_err, wr_info, wr_info_err);
		return NULL;
	}

	uint32_t raw1 = 0u, raw2 = 0u;
	if (!rtc_sysctl_read_raw(&raw1, NULL)) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=raw1-read\n");
		return NULL;
	}
	rtc_sleep_ms(2100u);
	if (!rtc_sysctl_read_raw(&raw2, NULL) || raw2 <= raw1 || raw2 > raw1 + 5u) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=raw-tick raw1=%u raw2=%u\n",
		    (unsigned)raw1, (unsigned)raw2);
		return NULL;
	}

	pthread_mutex_lock(&rtc_state_lock);
	uint32_t notify0 = rtc_cal_notify_count;
	bool has0 = rtc_has_anchor;
	uint64_t gen0 = has0 ? rtc_anchor.generation : 0ull;
	int64_t off0 = has0 ? rtc_anchor.saved_offset_ms : 0LL;
	pthread_mutex_unlock(&rtc_state_lock);

	struct timespec mono0;
	struct timeval tv0;
	if (!has0 || clock_gettime(CLOCK_MONOTONIC, &mono0) != 0 || gettimeofday(&tv0, NULL) != 0) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=no-anchor-or-clock has0=%u\n",
		    has0 ? 1u : 0u);
		return NULL;
	}

	struct timeval shifted_tv = tv0;
	shifted_tv.tv_sec += 5;
	if (settimeofday(&shifted_tv, NULL) != 0) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=settimeofday-plus5 errno=%d\n", errno);
		return NULL;
	}

	uint32_t notify1 = 0u;
	uint64_t gen1 = 0ull;
	int64_t off1 = 0LL;
	bool step1_ok = rtc_wait_notify_and_gen(notify0, gen0, 4000u, &notify1, &gen1, &off1);

	struct timespec mono1;
	(void)clock_gettime(CLOCK_MONOTONIC, &mono1);
	int64_t elapsed_us = ((int64_t)mono1.tv_sec - (int64_t)mono0.tv_sec) * 1000000LL +
	    ((int64_t)mono1.tv_nsec - (int64_t)mono0.tv_nsec) / 1000LL;
	if (elapsed_us < 0) {
		elapsed_us = 0;
	}
	int64_t restore_us = (int64_t)tv0.tv_sec * 1000000LL + (int64_t)tv0.tv_usec + elapsed_us;
	struct timeval restore_tv;
	restore_tv.tv_sec = (time_t)(restore_us / 1000000LL);
	restore_tv.tv_usec = (suseconds_t)(restore_us % 1000000LL);
	int restore_rc = settimeofday(&restore_tv, NULL);
	int restore_err = errno;

	if (!step1_ok) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=step-plus5-notify-timeout restore_rc=%d\n",
		    restore_rc);
		return NULL;
	}
	int64_t delta1 = off1 - off0;
	if (delta1 < 3500LL || delta1 > 6500LL) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=step-plus5-delta delta1=%lld restore_rc=%d\n",
		    (long long)delta1, restore_rc);
		return NULL;
	}
	if (restore_rc != 0) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=settimeofday-restore errno=%d\n",
		    restore_err);
		return NULL;
	}

	uint32_t notify2 = 0u;
	uint64_t gen2 = 0ull;
	int64_t off2 = 0LL;
	if (!rtc_wait_notify_and_gen(notify1, gen1, 4000u, &notify2, &gen2, &off2)) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=restore-notify-timeout\n");
		return NULL;
	}
	if (fuxi_rtc_abs_diff_i64(off2, off0) > 1500ull) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=restore-offset-drift off0=%lld off2=%lld\n",
		    (long long)off0, (long long)off2);
		return NULL;
	}

	char active_path[160];
	pthread_mutex_lock(&rtc_state_lock);
	(void)snprintf(active_path, sizeof(active_path), "%s", rtc_active_path);
	pthread_mutex_unlock(&rtc_state_lock);

	struct fuxi_rtc_store_rec disk_rec;
	enum fuxi_rtc_store_status disk_st = FUXI_RTC_STORE_OK;
	int disk_err = 0;
	if (active_path[0] == '\0' ||
	    !rtc_store_read_file(active_path, &disk_rec, &disk_st, &disk_err) ||
	    disk_rec.generation != gen2 || disk_rec.saved_offset_ms != off2) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=disk-readback path=%s st=%d err=%d gen=%llu/%llu\n",
		    active_path, (int)disk_st, disk_err,
		    (unsigned long long)disk_rec.generation, (unsigned long long)gen2);
		return NULL;
	}

	struct fuxi_rtc_info live_info;
	if (!rtc_sysctl_read_info(&live_info, NULL)) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=info-reread\n");
		return NULL;
	}
	live_info.flags &= ~FUXI_RTC_FLAG_CLOCK_SET;

	struct fuxi_rtc_store_rec reset_rec = disk_rec;
	reset_rec.saved_raw = live_info.raw_now + 600u;
	reset_rec.saved_offset_ms = reset_rec.saved_utc_ms -
	    (int64_t)((uint64_t)reset_rec.saved_raw * 1000ull);
	struct fuxi_rtc_boot_plan reset_plan;
	fuxi_rtc_boot_decide(&live_info, 5000LL, true, &reset_rec, &reset_plan);

	struct fuxi_rtc_boot_plan restore_plan;
	fuxi_rtc_boot_decide(&live_info, 5000LL, true, &disk_rec, &restore_plan);
	int64_t now_ms = rtc_now_utc_ms();

	if (reset_plan.action != FUXI_RTC_BOOT_REFUSE_RAW_RESET ||
	    restore_plan.action != FUXI_RTC_BOOT_RESTORE_CLOCK ||
	    fuxi_rtc_abs_diff_i64(restore_plan.target_utc_ms, now_ms) > 2500ull) {
		rtc_say("fuxi: timed: rtc-b selftest FAIL reason=decision-check reset=%d restore=%d\n",
		    (int)reset_plan.action, (int)restore_plan.action);
		return NULL;
	}

	rtc_say("fuxi: timed: rtc-b selftest PASS ro_eperm=1 tick=%u->%u notify=%u->%u->%u gen=%llu->%llu->%llu raw_reset_refused=1 path=%s\n",
	    (unsigned)raw1, (unsigned)raw2,
	    (unsigned)notify0, (unsigned)notify1, (unsigned)notify2,
	    (unsigned long long)gen0, (unsigned long long)gen1, (unsigned long long)gen2,
	    active_path);
	return NULL;
}

static void
rtc_spawn_detached(void *(*fn)(void *), const char *label)
{
	pthread_attr_t attr;
	pthread_t tid;
	if (pthread_attr_init(&attr) != 0) {
		rtc_say("fuxi: timed: rtc-b %s attr init failed\n", label);
		return;
	}
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&tid, &attr, fn, NULL);
	(void)pthread_attr_destroy(&attr);
	if (rc != 0) {
		rtc_say("fuxi: timed: rtc-b %s spawn failed rc=%d\n", label, rc);
	}
}

__attribute__((constructor)) static void
fuxi_rtc_persist_init(void)
{
	const char *prog = getprogname();
	const char *inserted = getenv("DYLD_INSERT_LIBRARIES");
	uint32_t adt_mode = 0u;
	int adt_rc;
	struct fuxi_rtc_info info;
	int info_err = 0;
	int64_t sys_now_ms;
	struct fuxi_rtc_store_rec stored;
	char stored_path[160];
	bool has_stored;
	struct fuxi_rtc_boot_plan plan;

	if (prog == NULL || strcmp(prog, "timed") != 0) {
		return;
	}
	if (inserted != NULL && strcmp(inserted, RTC_SELF_LIB) == 0) {
		(void)unsetenv("DYLD_INSERT_LIBRARIES");
	}

	adt_rc = rtc_read_adt_mode(&adt_mode);
	if (adt_rc == 0 || (adt_rc > 0 && adt_mode == 0u)) {
		return;
	}
	if (adt_rc < 0) {
		rtc_say("fuxi: timed: rtc-b IORegistry:/chosen unreadable; defaulting mode=1\n");
		adt_mode = 1u;
	} else if (adt_mode > 3u) {
		rtc_say("fuxi: timed: rtc-b fuxi,rtc-user=%u out of range; disabled\n",
		    (unsigned)adt_mode);
		return;
	}
	rtc_mode = adt_mode;

	if (!rtc_sysctl_read_info(&info, &info_err)) {
		rtc_say("fuxi: timed: rtc-b kern.fuxirtc_info failed errno=%d uid=%u euid=%u\n",
		    info_err, (unsigned)getuid(), (unsigned)geteuid());
		return;
	}
	rtc_apid = info.apid;
	sys_now_ms = rtc_now_utc_ms();

	stored_path[0] = '\0';
	has_stored = rtc_store_read_best(&stored, stored_path, sizeof(stored_path));
	if (has_stored) {
		(void)snprintf(rtc_active_path, sizeof(rtc_active_path), "%s", stored_path);
	}

	rtc_say("fuxi: timed: rtc-b init mode=%u uid=%u euid=%u apid=%u raw=%u kflags=0x%x ksrc=%u sys_utc=%lld.%03lld stored=%u\n",
	    (unsigned)rtc_mode, (unsigned)getuid(), (unsigned)geteuid(),
	    (unsigned)info.apid, (unsigned)info.raw_now, (unsigned)info.flags,
	    (unsigned)info.boot_source,
	    (long long)(sys_now_ms / 1000LL), (long long)(sys_now_ms % 1000LL),
	    has_stored ? 1u : 0u);

	fuxi_rtc_boot_decide(&info, sys_now_ms, has_stored, has_stored ? &stored : NULL, &plan);

	switch (plan.action) {
	case FUXI_RTC_BOOT_SEED_STORE: {
		char saved_path[160];
		saved_path[0] = '\0';
		if (rtc_store_persist(&plan.record, saved_path, sizeof(saved_path))) {
			rtc_anchor = plan.record;
			rtc_has_anchor = true;
			rtc_raw_reset_pending = false;
			rtc_say("fuxi: timed: rtc-b boot action=seed-store persisted gen=%llu src=%u raw=%u offset_ms=%lld path=%s\n",
			    (unsigned long long)plan.record.generation,
			    (unsigned)plan.record.source,
			    (unsigned)plan.record.saved_raw,
			    (long long)plan.record.saved_offset_ms,
			    saved_path);
		} else {
			rtc_say("fuxi: timed: rtc-b boot action=seed-store persist FAILED\n");
		}
		break;
	}
	case FUXI_RTC_BOOT_KEEP_STORE:
		rtc_anchor = plan.record;
		rtc_has_anchor = true;
		rtc_raw_reset_pending = false;
		rtc_say("fuxi: timed: rtc-b boot action=keep-store gen=%llu raw=%u saved_raw=%u offset_ms=%lld path=%s\n",
		    (unsigned long long)plan.record.generation,
		    (unsigned)info.raw_now, (unsigned)plan.record.saved_raw,
		    (long long)plan.record.saved_offset_ms, stored_path);
		break;
	case FUXI_RTC_BOOT_RESTORE_CLOCK: {
		struct timeval target_tv;
		target_tv.tv_sec = (time_t)(plan.target_utc_ms / 1000LL);
		target_tv.tv_usec = (suseconds_t)((plan.target_utc_ms % 1000LL) * 1000LL);
		errno = 0;
		int set_rc = settimeofday(&target_tv, NULL);
		int set_err = errno;
		int64_t after_ms = rtc_now_utc_ms();
		rtc_anchor = plan.record;
		rtc_has_anchor = true;
		rtc_raw_reset_pending = false;
		rtc_say("fuxi: timed: rtc-b boot action=restore-clock rc=%d err=%d target=%lld.%03lld now=%lld.%03lld raw=%u saved_raw=%u gen=%llu path=%s\n",
		    set_rc, set_err,
		    (long long)(plan.target_utc_ms / 1000LL),
		    (long long)(plan.target_utc_ms % 1000LL),
		    (long long)(after_ms / 1000LL),
		    (long long)(after_ms % 1000LL),
		    (unsigned)info.raw_now, (unsigned)plan.record.saved_raw,
		    (unsigned long long)plan.record.generation, stored_path);
		break;
	}
	case FUXI_RTC_BOOT_REFUSE_RAW_RESET:
		rtc_anchor = plan.record;
		rtc_has_anchor = plan.has_record;
		rtc_raw_reset_pending = true;
		rtc_say("fuxi: timed: rtc-b boot action=refuse-raw-reset raw=%u saved_raw=%u apid=%u saved_apid=%u\n",
		    (unsigned)info.raw_now, (unsigned)plan.record.saved_raw,
		    (unsigned)info.apid, (unsigned)plan.record.apid);
		break;
	case FUXI_RTC_BOOT_REFUSE_OUT_OF_WINDOW:
		rtc_anchor = plan.record;
		rtc_has_anchor = plan.has_record;
		rtc_raw_reset_pending = true;
		rtc_say("fuxi: timed: rtc-b boot action=refuse-out-of-window raw=%u saved_raw=%u offset_ms=%lld\n",
		    (unsigned)info.raw_now, (unsigned)plan.record.saved_raw,
		    (long long)plan.record.saved_offset_ms);
		break;
	case FUXI_RTC_BOOT_WAIT_AUTHORITATIVE:
		rtc_say("fuxi: timed: rtc-b boot action=wait-authoritative raw=%u\n",
		    (unsigned)info.raw_now);
		break;
	case FUXI_RTC_BOOT_NO_HW:
	default:
		rtc_say("fuxi: timed: rtc-b boot action=%s; watcher skipped\n",
		    fuxi_rtc_boot_action_name(plan.action));
		return;
	}

	rtc_spawn_detached(rtc_watcher_thread, "watcher");
	if ((rtc_mode & 2u) != 0u) {
		rtc_spawn_detached(rtc_selftest_thread, "selftest");
	}
}
