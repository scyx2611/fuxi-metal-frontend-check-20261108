/*
 * FuxiBluetoothPatch.h 主機測試（ASan／UBSan）：檢核 20H392 bluetoothd H4 傳輸表補丁的正常寫入
 * 與各項邊界／指紋拒絕條件。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FuxiBluetoothPatch.h"

static int failures;

#define EXPECT(cond)                                                                                                 \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                          \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

struct fixture {
    char text[128];
    uint64_t table[FUXI_BT_H4_WORDS];
    uint64_t text_start;
    uint64_t text_end;
    uint64_t data_start;
    uint64_t data_end;
    uint64_t replacement;
};

static void fixture_init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    memcpy(f->text + 13, "H4", 3);
    f->text_start = (uint64_t)(uintptr_t)f->text;
    f->text_end = f->text_start + sizeof(f->text);
    f->data_start = (uint64_t)(uintptr_t)f->table;
    f->data_end = f->data_start + sizeof(f->table);
    f->replacement = 0xdeadbeef12340000ULL;

    f->table[0] = FUXI_BT_H4_TRANSPORT_TYPE;
    f->table[1] = f->text_start + 13;
    for (unsigned index = FUXI_BT_H4_FN_FIRST; index <= FUXI_BT_H4_FN_LAST; index++) {
        const uint64_t aligned = (f->text_start + 32u + index * 4u) & ~(uint64_t)3u;
        f->table[index] = aligned;
    }
}

int main(void)
{
    struct fixture f;

    /* 正常情況：寫入 words[12]，其餘欄位不變，再次執行回報 already_set */
    fixture_init(&f);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_PATCHED);
    EXPECT(f.table[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] == f.replacement);
    EXPECT(f.table[10] == 0 && f.table[11] == 0 && f.table[13] == 0 && f.table[17] == 0);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end,
               f.replacement + 4u) == FUXI_BT_H4_ALREADY_SET);
    EXPECT(f.table[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] == f.replacement);

    /* 參數錯誤：replacement=0、反轉區間、未對齊 */
    fixture_init(&f);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, 0) ==
           FUXI_BT_H4_BAD_ARGS);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_end, f.text_start, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_ARGS);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start + 4u, f.text_start, f.text_end, f.data_start, f.data_end + 8u,
               f.replacement) == FUXI_BT_H4_BAD_ARGS);
    EXPECT(f.table[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] == 0);

    /* 表格超出 __DATA 邊界：不可解參考，直接拒絕 */
    fixture_init(&f);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end - 8u,
               f.replacement) == FUXI_BT_H4_OUT_OF_DATA);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start - 8u, f.text_start, f.text_end, f.data_start, f.data_end,
               f.replacement) == FUXI_BT_H4_OUT_OF_DATA);

    /* 傳輸型別不符 */
    fixture_init(&f);
    f.table[0] = 3;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_TYPE);
    EXPECT(f.table[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] == 0);

    /* 名稱不在 __TEXT、結尾空間不足、或內容不是 "H4\0" */
    fixture_init(&f);
    f.table[1] = f.text_end - 2u;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_NAME);
    fixture_init(&f);
    memcpy(f.text + 13, "H4BC", 5);
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_NAME);
    fixture_init(&f);
    f.table[1] = f.data_start;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_NAME);

    /* 8 個 __TEXT 函式指標任一越界或未 4-byte 對齊 */
    fixture_init(&f);
    f.table[5] = f.text_end;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_FUNCS);
    fixture_init(&f);
    f.table[9] |= 2u;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_FUNCS);

    /* 尾段（words[10..17] 除 [12] 外）非零 */
    fixture_init(&f);
    f.table[17] = 1;
    EXPECT(fuxi_bt_patch_h4_table(f.data_start, f.text_start, f.text_end, f.data_start, f.data_end, f.replacement) ==
           FUXI_BT_H4_BAD_TAIL);
    EXPECT(f.table[FUXI_BT_H4_PREPARE_TERMINATE_INDEX] == 0);

    EXPECT(strcmp(fuxi_bt_h4_patch_result_name(FUXI_BT_H4_PATCHED), "patched") == 0);
    EXPECT(strcmp(fuxi_bt_h4_patch_result_name(FUXI_BT_H4_BAD_TAIL), "bad_tail") == 0);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("test-bluetooth-patch: ok\n");
    return 0;
}
