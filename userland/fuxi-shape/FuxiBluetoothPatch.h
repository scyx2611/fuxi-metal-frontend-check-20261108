/*
 * 20H392 bluetoothd H4 傳輸表補丁檢核（FuxiBluetoothLog.c 用；主機測試 tools/test-bluetooth-patch.c）。
 *
 * 20H392 bluetoothd 在關閉藍牙電源時會呼叫 H4 傳輸驅動表 +0x60（PrepareTerminate），但純 H4 表
 * （靜態位址 0x100844228，共 18 個 64-bit 欄位）該欄為 NULL，導致關藍牙必現 pc=0 SIGSEGV。
 * 同表相鄰的 H4BC 驅動在 +0x60 的實作（0x10006a444）不收參數且無回傳值（void (void)）。
 *
 * 寫入前逐一檢核：
 *   - 表格完整落在 __DATA 區段且 8-byte 對齊
 *   - words[0] == 1（TRANSPORT_TYPE_H4）
 *   - words[1] 指向 __TEXT 區段內的字串 "H4\0"
 *   - words[2..9] 全為落在 __TEXT 區段內且 4-byte 對齊的函式指標
 *   - words[10..17] 全為 0（若 words[12] 非 0 回報 already_set）
 * 任一檢核不符就原樣不動。
 */
#ifndef FUXI_BLUETOOTH_PATCH_H
#define FUXI_BLUETOOTH_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUXI_BT_H4_STATIC_ADDR 0x100844228ULL
#define FUXI_BT_H4_WORDS 18u
#define FUXI_BT_H4_TRANSPORT_TYPE 1ULL
#define FUXI_BT_H4_FN_FIRST 2u
#define FUXI_BT_H4_FN_LAST 9u
#define FUXI_BT_H4_TAIL_FIRST 10u
#define FUXI_BT_H4_PREPARE_TERMINATE_INDEX 12u

enum fuxi_bt_h4_patch_result {
    FUXI_BT_H4_PATCHED = 0,
    FUXI_BT_H4_BAD_ARGS,
    FUXI_BT_H4_OUT_OF_DATA,
    FUXI_BT_H4_BAD_TYPE,
    FUXI_BT_H4_BAD_NAME,
    FUXI_BT_H4_BAD_FUNCS,
    FUXI_BT_H4_ALREADY_SET,
    FUXI_BT_H4_BAD_TAIL,
};

static inline const char *fuxi_bt_h4_patch_result_name(enum fuxi_bt_h4_patch_result result)
{
    switch (result) {
    case FUXI_BT_H4_PATCHED:
        return "patched";
    case FUXI_BT_H4_BAD_ARGS:
        return "bad_args";
    case FUXI_BT_H4_OUT_OF_DATA:
        return "out_of_data";
    case FUXI_BT_H4_BAD_TYPE:
        return "bad_type";
    case FUXI_BT_H4_BAD_NAME:
        return "bad_name";
    case FUXI_BT_H4_BAD_FUNCS:
        return "bad_funcs";
    case FUXI_BT_H4_ALREADY_SET:
        return "already_set";
    case FUXI_BT_H4_BAD_TAIL:
        return "bad_tail";
    default:
        return "unknown";
    }
}

static inline enum fuxi_bt_h4_patch_result fuxi_bt_patch_h4_table(uint64_t table_addr, uint64_t text_start,
    uint64_t text_end, uint64_t data_start, uint64_t data_end, uint64_t replacement)
{
    const uint64_t table_bytes = (uint64_t)FUXI_BT_H4_WORDS * sizeof(uint64_t);

    if (replacement == 0 || text_start >= text_end || data_start >= data_end ||
        (table_addr & (sizeof(uint64_t) - 1u)) != 0u) {
        return FUXI_BT_H4_BAD_ARGS;
    }
    if (table_addr < data_start || table_addr > data_end || (data_end - table_addr) < table_bytes) {
        return FUXI_BT_H4_OUT_OF_DATA;
    }

    uint64_t *words = (uint64_t *)(uintptr_t)table_addr;
    if (words[0] != FUXI_BT_H4_TRANSPORT_TYPE) {
        return FUXI_BT_H4_BAD_TYPE;
    }

    const uint64_t name_addr = words[1];
    if (name_addr < text_start || name_addr > text_end || (text_end - name_addr) < 3u) {
        return FUXI_BT_H4_BAD_NAME;
    }
    if (memcmp((const void *)(uintptr_t)name_addr, "H4", 3) != 0) {
        return FUXI_BT_H4_BAD_NAME;
    }

    for (unsigned index = FUXI_BT_H4_FN_FIRST; index <= FUXI_BT_H4_FN_LAST; index++) {
        const uint64_t fn_addr = words[index];
        if (fn_addr < text_start || fn_addr >= text_end || (fn_addr & 3u) != 0u) {
            return FUXI_BT_H4_BAD_FUNCS;
        }
    }

    if (words[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] != 0) {
        return FUXI_BT_H4_ALREADY_SET;
    }
    for (unsigned index = FUXI_BT_H4_TAIL_FIRST; index < FUXI_BT_H4_WORDS; index++) {
        if (words[index] != 0) {
            return FUXI_BT_H4_BAD_TAIL;
        }
    }

    words[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] = replacement;
    return FUXI_BT_H4_PATCHED;
}

#endif
