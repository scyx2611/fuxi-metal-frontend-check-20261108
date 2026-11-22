/*
 * Host ASan/UBSan test for FuxiRtcPersistLogic.h (Plan B iOS RTC persistence).
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FuxiRtcPersistLogic.h"

static struct fuxi_rtc_info
make_info(uint32_t raw_now, bool clock_set, uint32_t boot_source)
{
	struct fuxi_rtc_info info;
	memset(&info, 0, sizeof(info));
	info.version = FUXI_RTC_INFO_VERSION;
	info.flags = FUXI_RTC_FLAG_VALID_APID | FUXI_RTC_FLAG_LIVE_READ |
	    (clock_set ? FUXI_RTC_FLAG_CLOCK_SET : 0u);
	info.raw_now = raw_now;
	info.raw_boot = raw_now > 5u ? raw_now - 5u : 0u;
	info.boot_source = boot_source;
	info.apid = 53u;
	info.boot_offset_ms = 1795003732213ull;
	info.boot_epoch_ms = info.boot_offset_ms + (uint64_t)info.raw_boot * 1000ull;
	return info;
}

static void
test_crc32_known_vector(void)
{
	static const uint8_t sample[] = "123456789";
	assert(fuxi_rtc_crc32(sample, 9u) == 0xcbf43926u);
	assert(fuxi_rtc_crc32(sample, 0u) == 0x00000000u);
}

static void
test_encode_decode_and_corruption(void)
{
	struct fuxi_rtc_store_rec orig;
	struct fuxi_rtc_store_rec decoded;
	uint8_t buf[FUXI_RTC_STORE_SIZE];

	fuxi_rtc_make_record(&orig, 53u, 280000u, 1795283732213LL,
	    FUXI_RTC_STORE_SRC_KERNEL_PERSIST, 1ull);
	assert(fuxi_rtc_store_encode(&orig, buf));
	assert(fuxi_rtc_store_decode(buf, sizeof(buf), &decoded) == FUXI_RTC_STORE_OK);
	assert(decoded.apid == 53u);
	assert(decoded.saved_raw == 280000u);
	assert(decoded.saved_utc_ms == 1795283732213LL);
	assert(decoded.saved_offset_ms == 1795003732213LL);
	assert(decoded.generation == 1ull);

	assert(fuxi_rtc_store_decode(buf, 63u, &decoded) == FUXI_RTC_STORE_BAD_LEN);
	assert(fuxi_rtc_store_decode(buf, 65u, &decoded) == FUXI_RTC_STORE_BAD_LEN);

	for (size_t pos = 0; pos < FUXI_RTC_STORE_SIZE; pos++) {
		uint8_t broken[FUXI_RTC_STORE_SIZE];
		memcpy(broken, buf, sizeof(broken));
		broken[pos] ^= 0x01u;
		assert(fuxi_rtc_store_decode(broken, sizeof(broken), &decoded) != FUXI_RTC_STORE_OK);
	}

	uint8_t tampered[FUXI_RTC_STORE_SIZE];
	memcpy(tampered, buf, sizeof(tampered));
	fuxi_rtc_put_le64(tampered + 24, (uint64_t)(orig.saved_offset_ms + 1000LL));
	fuxi_rtc_put_le32(tampered + 60, fuxi_rtc_crc32(tampered, 60u));
	assert(fuxi_rtc_store_decode(tampered, sizeof(tampered), &decoded) == FUXI_RTC_STORE_BAD_FIELDS);

	struct fuxi_rtc_store_rec bad_epoch;
	fuxi_rtc_make_record(&bad_epoch, 53u, 100u, 1000000000LL, FUXI_RTC_STORE_SRC_CALENDAR_SET, 1ull);
	assert(!fuxi_rtc_store_encode(&bad_epoch, buf));
}

static void
test_boot_decisions(void)
{
	struct fuxi_rtc_info info_plan_a = make_info(280100u, true, FUXI_RTC_SRC_PERSIST_ATS2);
	int64_t now_a_ms = 1795003732213LL + 280100LL * 1000LL;
	struct fuxi_rtc_boot_plan plan;

	fuxi_rtc_boot_decide(&info_plan_a, now_a_ms, false, NULL, &plan);
	assert(plan.action == FUXI_RTC_BOOT_SEED_STORE);
	assert(plan.has_record);
	assert(plan.record.generation == 1ull);
	assert(plan.record.source == FUXI_RTC_STORE_SRC_KERNEL_PERSIST);
	struct fuxi_rtc_store_rec seeded = plan.record;

	struct fuxi_rtc_info info_later = make_info(280500u, true, FUXI_RTC_SRC_PERSIST_ATS2);
	int64_t now_later_ms = seeded.saved_offset_ms + 280500LL * 1000LL + 800LL;
	fuxi_rtc_boot_decide(&info_later, now_later_ms, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_KEEP_STORE);
	assert(plan.record.generation == 1ull);

	fuxi_rtc_boot_decide(&info_later, now_later_ms + 4000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_SEED_STORE);
	assert(plan.record.generation == 2ull);

	struct fuxi_rtc_info info_b = make_info(280220u, false, FUXI_RTC_SRC_NONE);
	fuxi_rtc_boot_decide(&info_b, 8000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_RESTORE_CLOCK);
	assert(plan.target_utc_ms == seeded.saved_offset_ms + 280220LL * 1000LL);
	assert(plan.target_utc_ms == seeded.saved_utc_ms + 120000LL);

	struct fuxi_rtc_info info_reset = make_info(12u, false, FUXI_RTC_SRC_NONE);
	fuxi_rtc_boot_decide(&info_reset, 8000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_REFUSE_RAW_RESET);
	assert(plan.raw_reset_detected);

	struct fuxi_rtc_info info_apid = make_info(280500u, false, FUXI_RTC_SRC_NONE);
	info_apid.apid = 99u;
	fuxi_rtc_boot_decide(&info_apid, 8000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_REFUSE_RAW_RESET);

	fuxi_rtc_boot_decide(&info_b, 8000LL, false, NULL, &plan);
	assert(plan.action == FUXI_RTC_BOOT_WAIT_AUTHORITATIVE);

	struct fuxi_rtc_info info_a_post_reset = make_info(15u, true, FUXI_RTC_SRC_PERSIST_ATS2);
	fuxi_rtc_boot_decide(&info_a_post_reset, 1795285000000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_SEED_STORE);
	assert(plan.raw_reset_detected);
	assert(plan.record.source == FUXI_RTC_STORE_SRC_RECOVERY);
	assert(plan.record.saved_raw == 15u);
	assert(plan.record.generation == 2ull);

	struct fuxi_rtc_info info_nohw = info_b;
	info_nohw.flags &= ~FUXI_RTC_FLAG_LIVE_READ;
	fuxi_rtc_boot_decide(&info_nohw, 8000LL, true, &seeded, &plan);
	assert(plan.action == FUXI_RTC_BOOT_NO_HW);
}

static void
test_runtime_change_and_recovery(void)
{
	struct fuxi_rtc_store_rec anchor;
	struct fuxi_rtc_store_rec next;
	int64_t delta_ms = 0;

	fuxi_rtc_make_record(&anchor, 53u, 280100u, 1795283832213LL,
	    FUXI_RTC_STORE_SRC_KERNEL_PERSIST, 1ull);

	assert(fuxi_rtc_change_decide(true, 53u, 280110u,
	    anchor.saved_offset_ms + 280110LL * 1000LL + 900LL,
	    true, false, &anchor, &next, &delta_ms) == FUXI_RTC_CHANGE_SKIP_DEADBAND);
	assert(delta_ms == 900LL);

	assert(fuxi_rtc_change_decide(true, 53u, 280110u,
	    anchor.saved_offset_ms + 280110LL * 1000LL + 5000LL,
	    true, false, &anchor, &next, &delta_ms) == FUXI_RTC_CHANGE_PERSIST);
	assert(delta_ms == 5000LL);
	assert(next.generation == 2ull);
	assert(next.source == FUXI_RTC_STORE_SRC_CALENDAR_SET);
	assert(next.saved_offset_ms == anchor.saved_offset_ms + 5000LL);

	assert(fuxi_rtc_change_decide(false, 53u, 280120u,
	    next.saved_offset_ms + 280120LL * 1000LL + 1800LL,
	    true, false, &next, &anchor, &delta_ms) == FUXI_RTC_CHANGE_SKIP_DEADBAND);
	assert(fuxi_rtc_change_decide(false, 53u, 280125u,
	    next.saved_offset_ms + 280125LL * 1000LL + 2100LL,
	    true, false, &next, &anchor, &delta_ms) == FUXI_RTC_CHANGE_PERSIST);
	assert(anchor.generation == 3ull);
	assert(anchor.source == FUXI_RTC_STORE_SRC_POLL_DRIFT);

	assert(fuxi_rtc_change_decide(true, 53u, 280130u, 10000LL,
	    true, false, &anchor, &next, &delta_ms) == FUXI_RTC_CHANGE_IGNORE_UNSAFE);

	assert(fuxi_rtc_change_decide(true, 53u, 20u, 1795290000000LL,
	    true, true, &anchor, &next, &delta_ms) == FUXI_RTC_CHANGE_PERSIST);
	assert(next.generation == 4ull);
	assert(next.source == FUXI_RTC_STORE_SRC_RECOVERY);
	assert(next.saved_raw == 20u);
	assert(next.saved_offset_ms == 1795290000000LL - 20000LL);
}

static void
test_atomic_disk_roundtrip(void)
{
	char dir_tmpl[] = "/tmp/fuxi-rtc-test-XXXXXX";
	char *dir = mkdtemp(dir_tmpl);
	assert(dir != NULL);

	char final_path[256];
	char tmp_path[256];
	snprintf(final_path, sizeof(final_path), "%s/fuxi-rtc-offset.bin", dir);
	snprintf(tmp_path, sizeof(tmp_path), "%s/fuxi-rtc-offset.bin.tmp", dir);

	struct fuxi_rtc_store_rec rec;
	struct fuxi_rtc_store_rec readback;
	uint8_t out_buf[FUXI_RTC_STORE_SIZE];
	uint8_t in_buf[FUXI_RTC_STORE_SIZE];

	fuxi_rtc_make_record(&rec, 53u, 281234u, 1795284966213LL,
	    FUXI_RTC_STORE_SRC_CALENDAR_SET, 7ull);
	assert(fuxi_rtc_store_encode(&rec, out_buf));

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, out_buf, sizeof(out_buf)) == (ssize_t)sizeof(out_buf));
	assert(fsync(fd) == 0);
	assert(close(fd) == 0);
	assert(rename(tmp_path, final_path) == 0);

	fd = open(final_path, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, in_buf, sizeof(in_buf)) == (ssize_t)sizeof(in_buf));
	assert(close(fd) == 0);
	assert(fuxi_rtc_store_decode(in_buf, sizeof(in_buf), &readback) == FUXI_RTC_STORE_OK);
	assert(readback.generation == 7ull);
	assert(readback.saved_offset_ms == rec.saved_offset_ms);
	assert(readback.saved_raw == rec.saved_raw);

	assert(unlink(final_path) == 0);
	assert(rmdir(dir) == 0);
}

int
main(void)
{
	test_crc32_known_vector();
	test_encode_decode_and_corruption();
	test_boot_decisions();
	test_runtime_change_and_recovery();
	test_atomic_disk_roundtrip();
	printf("test-rtc-persist: ALL PASSED\n");
	return 0;
}
