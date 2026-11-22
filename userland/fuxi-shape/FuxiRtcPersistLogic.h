/*
 * Plan B iOS-side RTC offset persistence logic (pure C11, host-testable).
 *
 * Reference survey (per AGENTS.md section 0):
 *  - Kernel read-only sysctl ABI & sanity window: xnu/fuxi-h1/fuxi_rtc_persist.h
 *    (struct fuxi_rtc_info = 40 bytes LE, window [1700000000, 2200000000] s).
 *  - Offset model (utc_ms = offset_ms + raw_s * 1000) without touching Android
 *    /persist or PMIC RTC hardware registers: mirrors Qualcomm time_daemon
 *    (ats_2) in iOS-private storage with CRC-32 and raw-reset protection.
 *  - Integrity checksum: standard IEEE 802.3 CRC-32 (reflected polynomial 0xedb88320).
 */
#ifndef FUXI_RTC_PERSIST_LOGIC_H
#define FUXI_RTC_PERSIST_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUXI_RTC_INFO_VERSION          1u

#define FUXI_RTC_SRC_NONE              0u
#define FUXI_RTC_SRC_BOOT_ARG          1u
#define FUXI_RTC_SRC_PERSIST_ATS2      2u
#define FUXI_RTC_SRC_PERSIST_ATS1      3u
#define FUXI_RTC_SRC_ADT               4u

#define FUXI_RTC_FLAG_VALID_APID       (1u << 0)
#define FUXI_RTC_FLAG_CLOCK_SET        (1u << 1)
#define FUXI_RTC_FLAG_PERSIST_ENABLED  (1u << 2)
#define FUXI_RTC_FLAG_PERSIST_REJECTED (1u << 3)
#define FUXI_RTC_FLAG_LIVE_READ        (1u << 4)

/* Must match kernel xnu/fuxi-h1/fuxi_rtc_persist.h byte-for-byte. */
struct fuxi_rtc_info {
	uint32_t version;
	uint32_t flags;
	uint32_t raw_now;
	uint32_t raw_boot;
	uint32_t boot_source;
	uint32_t apid;
	uint64_t boot_offset_ms;
	uint64_t boot_epoch_ms;
};
_Static_assert(sizeof(struct fuxi_rtc_info) == 40, "fuxi_rtc_info ABI must be 40 bytes");

#define FUXI_RTC_MAX_APID              1024u
#define FUXI_RTC_EPOCH_MIN_MS          1700000000000LL
#define FUXI_RTC_EPOCH_MAX_MS          2200000000000LL

#define FUXI_RTC_STORE_MAGIC           0x31435452u /* "RTC1" LE */
#define FUXI_RTC_STORE_VERSION         1u
#define FUXI_RTC_STORE_SIZE            64u

#define FUXI_RTC_STORE_SRC_KERNEL_PERSIST 1u
#define FUXI_RTC_STORE_SRC_KERNEL_ADT     2u
#define FUXI_RTC_STORE_SRC_CALENDAR_SET   3u
#define FUXI_RTC_STORE_SRC_POLL_DRIFT     4u
#define FUXI_RTC_STORE_SRC_RECOVERY       5u

/*
 * PMIC rtc@6100 raw counter has 1-second integer granularity, so
 * (gettimeofday_ms - raw * 1000) naturally wobbles up to +-1000 ms across
 * sub-second phase differences. Deadbands filter quantization noise while
 * persisting real NTP/manual shifts immediately.
 */
#define FUXI_RTC_EVENT_DEADBAND_MS     1500LL
#define FUXI_RTC_POLL_DEADBAND_MS      2000LL
#define FUXI_RTC_SEED_DEADBAND_MS      2000LL

struct fuxi_rtc_store_rec {
	uint32_t magic;
	uint32_t version;
	uint32_t flags;
	uint32_t apid;
	uint32_t saved_raw;
	uint32_t source;
	int64_t saved_offset_ms;
	int64_t saved_utc_ms;
	uint64_t generation;
	uint32_t crc32;
};

enum fuxi_rtc_store_status {
	FUXI_RTC_STORE_OK = 0,
	FUXI_RTC_STORE_BAD_LEN = -1,
	FUXI_RTC_STORE_BAD_MAGIC = -2,
	FUXI_RTC_STORE_BAD_VERSION = -3,
	FUXI_RTC_STORE_BAD_CRC = -4,
	FUXI_RTC_STORE_BAD_FIELDS = -5,
};

enum fuxi_rtc_boot_action {
	FUXI_RTC_BOOT_NO_HW = 0,
	FUXI_RTC_BOOT_SEED_STORE = 1,
	FUXI_RTC_BOOT_KEEP_STORE = 2,
	FUXI_RTC_BOOT_RESTORE_CLOCK = 3,
	FUXI_RTC_BOOT_REFUSE_RAW_RESET = 4,
	FUXI_RTC_BOOT_REFUSE_OUT_OF_WINDOW = 5,
	FUXI_RTC_BOOT_WAIT_AUTHORITATIVE = 6,
};

struct fuxi_rtc_boot_plan {
	enum fuxi_rtc_boot_action action;
	int64_t target_utc_ms;
	bool has_record;
	bool raw_reset_detected;
	struct fuxi_rtc_store_rec record;
};

enum fuxi_rtc_change_action {
	FUXI_RTC_CHANGE_IGNORE_UNSAFE = 0,
	FUXI_RTC_CHANGE_SKIP_DEADBAND = 1,
	FUXI_RTC_CHANGE_PERSIST = 2,
};

static inline uint32_t
fuxi_rtc_get_le32(const uint8_t *ptr)
{
	return (uint32_t)ptr[0] |
	    ((uint32_t)ptr[1] << 8) |
	    ((uint32_t)ptr[2] << 16) |
	    ((uint32_t)ptr[3] << 24);
}

static inline uint64_t
fuxi_rtc_get_le64(const uint8_t *ptr)
{
	return (uint64_t)fuxi_rtc_get_le32(ptr) |
	    ((uint64_t)fuxi_rtc_get_le32(ptr + 4) << 32);
}

static inline void
fuxi_rtc_put_le32(uint8_t *ptr, uint32_t val)
{
	ptr[0] = (uint8_t)(val & 0xffu);
	ptr[1] = (uint8_t)((val >> 8) & 0xffu);
	ptr[2] = (uint8_t)((val >> 16) & 0xffu);
	ptr[3] = (uint8_t)((val >> 24) & 0xffu);
}

static inline void
fuxi_rtc_put_le64(uint8_t *ptr, uint64_t val)
{
	fuxi_rtc_put_le32(ptr, (uint32_t)(val & 0xffffffffu));
	fuxi_rtc_put_le32(ptr + 4, (uint32_t)(val >> 32));
}

static inline uint32_t
fuxi_rtc_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;
	for (size_t idx = 0; idx < len; idx++) {
		crc ^= (uint32_t)data[idx];
		for (unsigned bit = 0; bit < 8u; bit++) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}

static inline bool
fuxi_rtc_ms_sane(int64_t utc_ms)
{
	return utc_ms >= FUXI_RTC_EPOCH_MIN_MS && utc_ms <= FUXI_RTC_EPOCH_MAX_MS;
}

static inline uint64_t
fuxi_rtc_abs_diff_i64(int64_t lhs, int64_t rhs)
{
	if (lhs >= rhs) {
		return (uint64_t)lhs - (uint64_t)rhs;
	}
	return (uint64_t)rhs - (uint64_t)lhs;
}

static inline bool
fuxi_rtc_i64_add_checked(int64_t base, int64_t delta, int64_t *out_sum)
{
	if (delta > 0 && base > INT64_MAX - delta) {
		return false;
	}
	if (delta < 0 && base < INT64_MIN - delta) {
		return false;
	}
	*out_sum = base + delta;
	return true;
}

static inline bool
fuxi_rtc_store_fields_valid(const struct fuxi_rtc_store_rec *rec)
{
	int64_t raw_ms;
	int64_t recomputed_utc;

	if (rec == NULL) {
		return false;
	}
	if (rec->apid >= FUXI_RTC_MAX_APID || rec->generation == 0u) {
		return false;
	}
	if (!fuxi_rtc_ms_sane(rec->saved_utc_ms)) {
		return false;
	}
	raw_ms = (int64_t)((uint64_t)rec->saved_raw * 1000ull);
	if (!fuxi_rtc_i64_add_checked(rec->saved_offset_ms, raw_ms, &recomputed_utc)) {
		return false;
	}
	return recomputed_utc == rec->saved_utc_ms;
}

static inline bool
fuxi_rtc_store_encode(const struct fuxi_rtc_store_rec *rec, uint8_t out_buf[FUXI_RTC_STORE_SIZE])
{
	uint32_t crc;

	if (rec == NULL || out_buf == NULL || !fuxi_rtc_store_fields_valid(rec)) {
		return false;
	}
	memset(out_buf, 0, FUXI_RTC_STORE_SIZE);
	fuxi_rtc_put_le32(out_buf + 0, FUXI_RTC_STORE_MAGIC);
	fuxi_rtc_put_le32(out_buf + 4, FUXI_RTC_STORE_VERSION);
	fuxi_rtc_put_le32(out_buf + 8, rec->flags);
	fuxi_rtc_put_le32(out_buf + 12, rec->apid);
	fuxi_rtc_put_le32(out_buf + 16, rec->saved_raw);
	fuxi_rtc_put_le32(out_buf + 20, rec->source);
	fuxi_rtc_put_le64(out_buf + 24, (uint64_t)rec->saved_offset_ms);
	fuxi_rtc_put_le64(out_buf + 32, (uint64_t)rec->saved_utc_ms);
	fuxi_rtc_put_le64(out_buf + 40, rec->generation);
	crc = fuxi_rtc_crc32(out_buf, 60u);
	fuxi_rtc_put_le32(out_buf + 60, crc);
	return true;
}

static inline enum fuxi_rtc_store_status
fuxi_rtc_store_decode(const uint8_t *buf, size_t len, struct fuxi_rtc_store_rec *out_rec)
{
	struct fuxi_rtc_store_rec parsed;
	uint32_t computed_crc;

	if (buf == NULL || out_rec == NULL || len != FUXI_RTC_STORE_SIZE) {
		return FUXI_RTC_STORE_BAD_LEN;
	}
	memset(&parsed, 0, sizeof(parsed));
	parsed.magic = fuxi_rtc_get_le32(buf + 0);
	if (parsed.magic != FUXI_RTC_STORE_MAGIC) {
		return FUXI_RTC_STORE_BAD_MAGIC;
	}
	parsed.version = fuxi_rtc_get_le32(buf + 4);
	if (parsed.version != FUXI_RTC_STORE_VERSION) {
		return FUXI_RTC_STORE_BAD_VERSION;
	}
	parsed.crc32 = fuxi_rtc_get_le32(buf + 60);
	computed_crc = fuxi_rtc_crc32(buf, 60u);
	if (parsed.crc32 != computed_crc) {
		return FUXI_RTC_STORE_BAD_CRC;
	}
	parsed.flags = fuxi_rtc_get_le32(buf + 8);
	parsed.apid = fuxi_rtc_get_le32(buf + 12);
	parsed.saved_raw = fuxi_rtc_get_le32(buf + 16);
	parsed.source = fuxi_rtc_get_le32(buf + 20);
	parsed.saved_offset_ms = (int64_t)fuxi_rtc_get_le64(buf + 24);
	parsed.saved_utc_ms = (int64_t)fuxi_rtc_get_le64(buf + 32);
	parsed.generation = fuxi_rtc_get_le64(buf + 40);
	if (!fuxi_rtc_store_fields_valid(&parsed)) {
		return FUXI_RTC_STORE_BAD_FIELDS;
	}
	*out_rec = parsed;
	return FUXI_RTC_STORE_OK;
}

static inline bool
fuxi_rtc_info_usable(const struct fuxi_rtc_info *info)
{
	if (info == NULL || info->version != FUXI_RTC_INFO_VERSION) {
		return false;
	}
	if ((info->flags & FUXI_RTC_FLAG_VALID_APID) == 0u ||
	    (info->flags & FUXI_RTC_FLAG_LIVE_READ) == 0u) {
		return false;
	}
	return info->apid < FUXI_RTC_MAX_APID;
}

static inline void
fuxi_rtc_make_record(struct fuxi_rtc_store_rec *out_rec,
    uint32_t apid, uint32_t raw_now, int64_t utc_ms,
    uint32_t source, uint64_t generation)
{
	int64_t raw_ms = (int64_t)((uint64_t)raw_now * 1000ull);

	memset(out_rec, 0, sizeof(*out_rec));
	out_rec->magic = FUXI_RTC_STORE_MAGIC;
	out_rec->version = FUXI_RTC_STORE_VERSION;
	out_rec->flags = 0u;
	out_rec->apid = apid;
	out_rec->saved_raw = raw_now;
	out_rec->source = source;
	out_rec->saved_offset_ms = utc_ms - raw_ms;
	out_rec->saved_utc_ms = utc_ms;
	out_rec->generation = generation;
}

static inline void
fuxi_rtc_boot_decide(const struct fuxi_rtc_info *info, int64_t sys_now_ms,
    bool has_stored, const struct fuxi_rtc_store_rec *stored,
    struct fuxi_rtc_boot_plan *plan)
{
	bool sys_sane;
	uint64_t next_gen;

	memset(plan, 0, sizeof(*plan));
	if (!fuxi_rtc_info_usable(info)) {
		plan->action = FUXI_RTC_BOOT_NO_HW;
		return;
	}

	if (has_stored && stored != NULL && !fuxi_rtc_store_fields_valid(stored)) {
		has_stored = false;
		stored = NULL;
	}
	next_gen = (has_stored && stored != NULL) ? (stored->generation + 1ull) : 1ull;
	sys_sane = fuxi_rtc_ms_sane(sys_now_ms);

	if (sys_sane) {
		int64_t raw_ms = (int64_t)((uint64_t)info->raw_now * 1000ull);
		int64_t live_offset_ms = sys_now_ms - raw_ms;
		uint32_t seed_source = (info->boot_source == FUXI_RTC_SRC_PERSIST_ATS2 ||
		    info->boot_source == FUXI_RTC_SRC_PERSIST_ATS1)
		    ? FUXI_RTC_STORE_SRC_KERNEL_PERSIST
		    : FUXI_RTC_STORE_SRC_KERNEL_ADT;
		bool need_seed = !has_stored || stored == NULL;

		if (!need_seed) {
			if (stored->apid != info->apid || info->raw_now < stored->saved_raw) {
				need_seed = true;
				seed_source = FUXI_RTC_STORE_SRC_RECOVERY;
				plan->raw_reset_detected = true;
			} else if (fuxi_rtc_abs_diff_i64(live_offset_ms, stored->saved_offset_ms) >=
			    (uint64_t)FUXI_RTC_SEED_DEADBAND_MS) {
				need_seed = true;
			}
		}
		if (need_seed) {
			fuxi_rtc_make_record(&plan->record, info->apid, info->raw_now, sys_now_ms,
			    seed_source, next_gen);
			plan->has_record = true;
			plan->action = FUXI_RTC_BOOT_SEED_STORE;
		} else {
			plan->record = *stored;
			plan->has_record = true;
			plan->action = FUXI_RTC_BOOT_KEEP_STORE;
		}
		return;
	}

	if (!has_stored || stored == NULL) {
		plan->action = FUXI_RTC_BOOT_WAIT_AUTHORITATIVE;
		return;
	}

	plan->record = *stored;
	plan->has_record = true;
	if (stored->apid != info->apid || info->raw_now < stored->saved_raw) {
		plan->raw_reset_detected = true;
		plan->action = FUXI_RTC_BOOT_REFUSE_RAW_RESET;
		return;
	}

	int64_t raw_now_ms = (int64_t)((uint64_t)info->raw_now * 1000ull);
	int64_t target_utc_ms = 0;
	if (!fuxi_rtc_i64_add_checked(stored->saved_offset_ms, raw_now_ms, &target_utc_ms) ||
	    !fuxi_rtc_ms_sane(target_utc_ms) || target_utc_ms < stored->saved_utc_ms) {
		plan->action = FUXI_RTC_BOOT_REFUSE_OUT_OF_WINDOW;
		return;
	}

	plan->target_utc_ms = target_utc_ms;
	plan->action = FUXI_RTC_BOOT_RESTORE_CLOCK;
}

static inline enum fuxi_rtc_change_action
fuxi_rtc_change_decide(bool is_calendar_event, uint32_t apid, uint32_t raw_now, int64_t now_utc_ms,
    bool has_anchor, bool raw_reset_pending, const struct fuxi_rtc_store_rec *anchor,
    struct fuxi_rtc_store_rec *out_next, int64_t *out_delta_ms)
{
	int64_t raw_ms;
	int64_t new_offset_ms;
	uint64_t abs_delta_ms = 0u;
	uint64_t deadband_ms = (uint64_t)(is_calendar_event ?
	    FUXI_RTC_EVENT_DEADBAND_MS : FUXI_RTC_POLL_DEADBAND_MS);
	uint64_t next_gen;
	uint32_t source;

	if (out_delta_ms != NULL) {
		*out_delta_ms = 0;
	}
	if (apid >= FUXI_RTC_MAX_APID || !fuxi_rtc_ms_sane(now_utc_ms) || out_next == NULL) {
		return FUXI_RTC_CHANGE_IGNORE_UNSAFE;
	}
	if (has_anchor && anchor != NULL && !fuxi_rtc_store_fields_valid(anchor)) {
		has_anchor = false;
		anchor = NULL;
	}

	raw_ms = (int64_t)((uint64_t)raw_now * 1000ull);
	new_offset_ms = now_utc_ms - raw_ms;
	next_gen = (has_anchor && anchor != NULL) ? (anchor->generation + 1ull) : 1ull;

	if (has_anchor && anchor != NULL) {
		abs_delta_ms = fuxi_rtc_abs_diff_i64(new_offset_ms, anchor->saved_offset_ms);
		if (out_delta_ms != NULL) {
			*out_delta_ms = (new_offset_ms >= anchor->saved_offset_ms)
			    ? (int64_t)abs_delta_ms
			    : -(int64_t)abs_delta_ms;
		}
		bool hw_reset = raw_reset_pending || anchor->apid != apid || raw_now < anchor->saved_raw;
		if (!hw_reset && abs_delta_ms < deadband_ms) {
			return FUXI_RTC_CHANGE_SKIP_DEADBAND;
		}
		source = hw_reset ? FUXI_RTC_STORE_SRC_RECOVERY :
		    (is_calendar_event ? FUXI_RTC_STORE_SRC_CALENDAR_SET : FUXI_RTC_STORE_SRC_POLL_DRIFT);
	} else {
		source = raw_reset_pending ? FUXI_RTC_STORE_SRC_RECOVERY :
		    (is_calendar_event ? FUXI_RTC_STORE_SRC_CALENDAR_SET : FUXI_RTC_STORE_SRC_POLL_DRIFT);
	}

	fuxi_rtc_make_record(out_next, apid, raw_now, now_utc_ms, source, next_gen);
	return FUXI_RTC_CHANGE_PERSIST;
}

static inline const char *
fuxi_rtc_boot_action_name(enum fuxi_rtc_boot_action action)
{
	switch (action) {
	case FUXI_RTC_BOOT_NO_HW: return "no-hw";
	case FUXI_RTC_BOOT_SEED_STORE: return "seed-store";
	case FUXI_RTC_BOOT_KEEP_STORE: return "keep-store";
	case FUXI_RTC_BOOT_RESTORE_CLOCK: return "restore-clock";
	case FUXI_RTC_BOOT_REFUSE_RAW_RESET: return "refuse-raw-reset";
	case FUXI_RTC_BOOT_REFUSE_OUT_OF_WINDOW: return "refuse-out-of-window";
	case FUXI_RTC_BOOT_WAIT_AUTHORITATIVE: return "wait-authoritative";
	default: return "unknown";
	}
}

#endif /* FUXI_RTC_PERSIST_LOGIC_H */
