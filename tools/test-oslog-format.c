/*
 * FuxiOsLogFormat.h 主機測試（ASan／UBSan）：依 clang __builtin_os_log_format 的緩衝佈局手工組參數，
 * 核對 20H392 bluetoothd 重啟訊息、private、%.*s、純量、截斷緩衝、小輸出、%@、位址遮蔽，
 * 再以只含純量轉換的亂數格式加亂數緩衝（精確大小 malloc）確認不越界。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FuxiOsLogFormat.h"

static int failures;

#define EXPECT_TEXT(actual, expected)                                                                                \
    do {                                                                                                             \
        if (strcmp((actual), (expected)) != 0) {                                                                     \
            fprintf(stderr, "FAIL %s:%d\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__, (actual), (expected));   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

struct builder {
    uint8_t bytes[256];
    uint32_t size;
};

static void begin(struct builder *b, uint8_t summary, uint8_t count)
{
    memset(b, 0, sizeof(*b));
    b->bytes[0] = summary;
    b->bytes[1] = count;
    b->size = 2;
}

static void item(struct builder *b, uint8_t descriptor, const void *data, uint8_t size)
{
    b->bytes[b->size++] = descriptor;
    b->bytes[b->size++] = size;
    memcpy(b->bytes + b->size, data, size);
    b->size += size;
}

static void item_u32(struct builder *b, uint8_t descriptor, uint32_t value)
{
    item(b, descriptor, &value, sizeof(value));
}

static void item_u64(struct builder *b, uint8_t descriptor, uint64_t value)
{
    item(b, descriptor, &value, sizeof(value));
}

static void item_ptr(struct builder *b, uint8_t descriptor, const void *pointer)
{
    item(b, descriptor, &pointer, sizeof(pointer));
}

/* 精確大小的堆積副本，讓 ASan 抓越界 */
static const char *run(const char *format, const struct builder *b, size_t cap, fuxi_oslog_describe_fn describe)
{
    static char out[1024];
    uint8_t *copy = malloc(b->size ? b->size : 1);
    memcpy(copy, b->bytes, b->size);
    char *target = malloc(cap);
    size_t length = fuxi_oslog_format(target, cap, format, copy, b->size, describe);
    if (cap && (length >= cap || target[length] != 0 || strlen(target) != length)) {
        fprintf(stderr, "FAIL bad termination for [%s]\n", format);
        failures++;
    }
    snprintf(out, sizeof(out), "%s", cap ? target : "");
    free(target);
    free(copy);
    return out;
}

static size_t describe_hello(const void *object, char *out, size_t capacity)
{
    (void)object;
    const char text[] = "hello";
    if (capacity < sizeof(text)) {
        return 0;
    }
    memcpy(out, text, sizeof(text) - 1);
    return sizeof(text) - 1;
}

static void sanitize_case(const char *input, const char *expected, unsigned masked)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", input);
    unsigned count = fuxi_oslog_sanitize(text, strlen(text));
    EXPECT_TEXT(text, expected);
    if (count != masked) {
        fprintf(stderr, "FAIL masked %u want %u for [%s]\n", count, masked, input);
        failures++;
    }
}

int main(void)
{
    struct builder b;

    /* 20H392 bluetoothd 自願重啟訊息的形狀：public 字串＋int＋public 字串 */
    begin(&b, 0x02, 3);
    item_ptr(&b, 0x22, "release");
    item_u32(&b, 0x00, 4074);
    item_ptr(&b, 0x22, "Controller timeout");
    EXPECT_TEXT(run("Bluetooth error - restarting { build=%{public}s, reason=%d, context=\"%{public}s\" }", &b, 512,
                    NULL),
        "Bluetooth error - restarting { build=release, reason=4074, context=\"Controller timeout\" }");

    /* OI 日誌：單一 %{public}s */
    begin(&b, 0x02, 1);
    item_ptr(&b, 0x22, "OI_HCI: command 0xfce2 complete");
    EXPECT_TEXT(run("%{public}s", &b, 512, NULL), "OI_HCI: command 0xfce2 complete");

    /* private 與未標註字串 */
    begin(&b, 0x03, 2);
    item_ptr(&b, 0x21, "secret");
    item_ptr(&b, 0x20, "plain");
    EXPECT_TEXT(run("a=%{private}s b=%s", &b, 512, NULL), "a=<private> b=plain");

    /* %.*s：count 項目決定長度 */
    begin(&b, 0x02, 2);
    item_u32(&b, 0x10, 3);
    item_ptr(&b, 0x22, "abcdef");
    EXPECT_TEXT(run("%{public}.*s|", &b, 512, NULL), "abc|");
    begin(&b, 0x02, 1);
    item_ptr(&b, 0x22, "abcdef");
    EXPECT_TEXT(run("%.2s|", &b, 512, NULL), "ab|");

    /* 純量 */
    begin(&b, 0x00, 7);
    item_u32(&b, 0x00, (uint32_t)-5);
    item_u32(&b, 0x00, 4294967295u);
    item_u32(&b, 0x00, 0xbeef);
    item_u64(&b, 0x00, 0xABCDEF0123ull);
    item_u32(&b, 0x00, 'Z');
    item_u64(&b, 0x00, 0x1234);
    item_u32(&b, 0x60, 2);
    EXPECT_TEXT(run("%d %u %x %llX %c %% %p %m", &b, 512, NULL), "-5 4294967295 beef ABCDEF0123 Z % 0x1234 errno=2");
    begin(&b, 0x00, 1);
    {
        uint8_t small = 0xff;
        item(&b, 0x00, &small, 1);
    }
    EXPECT_TEXT(run("%hhd/%{bool}d", &b, 512, NULL), "-1/<?>");

    /* 寬度 * 吃掉一項；浮點 */
    begin(&b, 0x00, 3);
    item_u32(&b, 0x00, 5);
    item_u32(&b, 0x00, 42);
    {
        double half = 1.5;
        item(&b, 0x00, &half, sizeof(half));
    }
    EXPECT_TEXT(run("[%*d] %f", &b, 512, NULL), "[42] 1.5");

    /* 緩衝宣稱 3 項但只放 1 項：缺的印 <?>，不越界 */
    begin(&b, 0x00, 3);
    item_u32(&b, 0x00, 7);
    EXPECT_TEXT(run("%d %d %d", &b, 512, NULL), "7 <?> <?>");
    /* size 欄位超出緩衝 */
    begin(&b, 0x00, 1);
    b.bytes[b.size++] = 0x00;
    b.bytes[b.size++] = 8;
    b.bytes[b.size++] = 1;
    EXPECT_TEXT(run("%d", &b, 512, NULL), "<?>");
    /* 空緩衝與 NULL 格式 */
    begin(&b, 0x00, 0);
    b.size = 0;
    EXPECT_TEXT(run("x=%d", &b, 512, NULL), "x=<?>");
    {
        char text[32];
        fuxi_oslog_format(text, sizeof(text), NULL, NULL, 0, NULL);
        EXPECT_TEXT(text, "(no format)");
        fuxi_oslog_format(text, sizeof(text), "v=%d", NULL, 99, NULL);
        EXPECT_TEXT(text, "v=<?>");
        if (fuxi_oslog_format(text, 0, "abc", NULL, 0, NULL) != 0) {
            fprintf(stderr, "FAIL cap 0\n");
            failures++;
        }
    }

    /* 輸出容量小：截斷並補 0 */
    begin(&b, 0x02, 1);
    item_ptr(&b, 0x22, "0123456789");
    EXPECT_TEXT(run("%{public}s", &b, 8, NULL), "0123456");
    EXPECT_TEXT(run("%{public}s", &b, 1, NULL), "");

    /* %@ */
    begin(&b, 0x02, 4);
    item_ptr(&b, 0x42, (const void *)&b);
    item_ptr(&b, 0x41, (const void *)&b);
    item_ptr(&b, 0x42, NULL);
    item_ptr(&b, 0x42, (const void *)&b);
    EXPECT_TEXT(run("%@ %@ %@ %{public}@", &b, 512, describe_hello), "hello <private> (null) hello");
    EXPECT_TEXT(run("%@", &b, 512, NULL), "<obj>");

    /* mask 項在參數前（2026-11-19 bluetoothd SIGSEGV 的形狀）：要略過，不可把 "hash" 當物件 */
    begin(&b, 0x03, 3);
    item_ptr(&b, 0x42, (const void *)&b);
    item(&b, 0x70, "hash\0\0\0\0", 8);
    item_ptr(&b, 0x41, (const void *)&b);
    EXPECT_TEXT(run("Application %{public}@ for user %{private,mask.hash}@ is interested", &b, 512, describe_hello),
        "Application hello for user <private> is interested");
    /* BD_ADDR：mask、常數 count 6、pointer 資料，之後的參數不錯位 */
    begin(&b, 0x03, 4);
    item(&b, 0x70, "hash\0\0\0\0", 8);
    item_u32(&b, 0x11, 6);
    item_ptr(&b, 0x31, (const void *)&b);
    item_u32(&b, 0x00, 5);
    EXPECT_TEXT(run("dev %{private,bluetooth:BD_ADDR,mask.hash}.6P x=%d", &b, 512, describe_hello), "dev <data> x=5");
    /* %.4s 的常數 count 項；%.*d 的精度純量項 */
    begin(&b, 0x02, 5);
    item_u32(&b, 0x10, 4);
    item_ptr(&b, 0x22, "abcdefg");
    item_u32(&b, 0x00, 9);
    item_u32(&b, 0x00, 3);
    item_u32(&b, 0x00, 42);
    EXPECT_TEXT(run("%.4s|%d|%.*d", &b, 512, NULL), "abcd|9|42");
    /* %@ 對到 mask 以外的非物件種類：不交給 describe */
    begin(&b, 0x00, 1);
    item_u64(&b, 0x00, 0x68736168);
    EXPECT_TEXT(run("%@", &b, 512, describe_hello), "<?>");

    /* 種類不符：%s 對到純量不解參考 */
    begin(&b, 0x00, 1);
    item_u64(&b, 0x00, 0x41414141);
    EXPECT_TEXT(run("%s", &b, 512, NULL), "<?>");

    /* 位址遮蔽與控制字元 */
    sanitize_case("a\nb\tc\r", "a b c ", 0);
    sanitize_case("addr 12:34:56:78:9a:bc end", "addr xx:xx:xx:xx:xx:xx end", 1);
    sanitize_case("12-34-56-78-9A-BC", "xx-xx-xx-xx-xx-xx", 1);
    sanitize_case("A1:B2:C3:D4:E5:F6,00:11:22:33:44:55", "xx:xx:xx:xx:xx:xx,xx:xx:xx:xx:xx:xx", 2);
    sanitize_case("112:34:56:78:9a:bc", "112:34:56:78:9a:bc", 0);
    sanitize_case("12:34:56:78:9a:bcd", "12:34:56:78:9a:bcd", 0);
    sanitize_case("12:34-56:78:9a:bc", "12:34-56:78:9a:bc", 0);
    sanitize_case("12:34:56:78:9a", "12:34:56:78:9a", 0);
    sanitize_case("", "", 0);

    /* 亂數：只用不解參考的轉換，緩衝與 size 都亂 */
    static const char *const tokens[] = { "%d", "%u", "%x", "%llx", "%c", "%%", "%p", "%m", "%f", "%P", "%*d", "%.*d",
        "%{public}d", "%{private}u", "%{bool}d", "%hhd", "%zu", "text ", "%", "%{", "%{public", "%.", "%-08.3x", "%q" };
    unsigned seed = 12345;
    for (unsigned round = 0; round < 50000; round++) {
        char format[128] = { 0 };
        size_t used = 0;
        seed = seed * 1103515245u + 12345u;
        const unsigned pieces = (seed >> 16) % 8;
        for (unsigned index = 0; index < pieces; index++) {
            seed = seed * 1103515245u + 12345u;
            const char *token = tokens[(seed >> 16) % (sizeof(tokens) / sizeof(tokens[0]))];
            const size_t length = strlen(token);
            if (used + length + 1 >= sizeof(format)) {
                break;
            }
            memcpy(format + used, token, length);
            used += length;
        }
        seed = seed * 1103515245u + 12345u;
        const uint32_t size = (seed >> 16) % 48;
        uint8_t *buffer = malloc(size ? size : 1);
        for (uint32_t index = 0; index < size; index++) {
            seed = seed * 1103515245u + 12345u;
            buffer[index] = (uint8_t)(seed >> 16);
        }
        seed = seed * 1103515245u + 12345u;
        const size_t cap = 1 + (seed >> 16) % 64;
        char *out = malloc(cap);
        const size_t length = fuxi_oslog_format(out, cap, format, size ? buffer : NULL, size, NULL);
        if (length >= cap || out[length] != 0) {
            fprintf(stderr, "FAIL fuzz round %u\n", round);
            failures++;
        }
        fuxi_oslog_sanitize(out, length);
        free(out);
        free(buffer);
    }

    if (failures) {
        fprintf(stderr, "test-oslog-format: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-oslog-format: ok (fixed cases + 50000 fuzz rounds)\n");
    return 0;
}
