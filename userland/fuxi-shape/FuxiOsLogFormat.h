/*
 * os_log 緩衝區轉成一行文字（FuxiBluetoothLog.c 用；主機測試 tools/test-oslog-format.c）。
 *
 * 緩衝格式依 clang 的 __builtin_os_log_format（clang/lib/AST/OSLog.cpp 與 CodeGen 的
 * emitBuiltinOSLogFormat）：byte 0 摘要旗標、byte 1 參數數；之後每個參數一個 descriptor byte
 * （高 4 位元種類、低 4 位元旗標：private 0x1、public 0x2、sensitive 0x4）、一個 size byte、size bytes 資料。
 * 種類：0 純量、1 count（%.*s 的長度）、2 字串指標、3 指標＋count 資料（%P）、4 物件（%@）、5 寬字串、6 errno（%m）、
 * 7 mask。20H392 bluetoothd 的錯誤重啟訊息就是這種佈局（0x10009069c：summary 0x02、2 個參數、
 * 0x00/4 的 reason、0x22/8 的 public 字串）。
 *
 * 只讀 [buf, buf+size) 之內；格式與參數對不上時印 <?>，不往後讀。字串指標指向呼叫端自己的記憶體，
 * 只讀到 precision 或 FUXI_OSLOG_STRING_MAX 個字元。
 */
#ifndef FUXI_OS_LOG_FORMAT_H
#define FUXI_OS_LOG_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUXI_OSLOG_KIND_SCALAR 0u
#define FUXI_OSLOG_KIND_COUNT 1u
#define FUXI_OSLOG_KIND_STRING 2u
#define FUXI_OSLOG_KIND_POINTER 3u
#define FUXI_OSLOG_KIND_OBJECT 4u
#define FUXI_OSLOG_KIND_WIDE 5u
#define FUXI_OSLOG_KIND_ERRNO 6u
#define FUXI_OSLOG_KIND_MASK 7u
#define FUXI_OSLOG_FLAG_PRIVATE 0x1u
#define FUXI_OSLOG_STRING_MAX 400u

/* %@ 物件轉文字：寫進 out（不含結尾 0），回寫入長度；0 表示不認得 */
typedef size_t (*fuxi_oslog_describe_fn)(const void *object, char *out, size_t capacity);

struct fuxi_oslog_out {
    char *buf;
    size_t cap;
    size_t len;
};

struct fuxi_oslog_item {
    unsigned kind;
    unsigned flags;
    unsigned size;
    const uint8_t *data;
};

struct fuxi_oslog_reader {
    const uint8_t *buf;
    uint32_t size;
    uint32_t pos;
    unsigned left;
};

static inline void fuxi_oslog_put(struct fuxi_oslog_out *out, const char *text, size_t length)
{
    while (length-- && out->len + 1 < out->cap) {
        out->buf[out->len++] = *text++;
    }
    out->buf[out->len] = 0;
}

static inline void fuxi_oslog_puts(struct fuxi_oslog_out *out, const char *text)
{
    fuxi_oslog_put(out, text, strlen(text));
}

static inline void fuxi_oslog_putf(struct fuxi_oslog_out *out, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static inline void fuxi_oslog_putf(struct fuxi_oslog_out *out, const char *format, ...)
{
    char text[64];
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    __builtin_va_end(args);
    if (length > 0) {
        fuxi_oslog_put(out, text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
    }
}

static inline bool fuxi_oslog_next(struct fuxi_oslog_reader *reader, struct fuxi_oslog_item *item)
{
    if (!reader->left || reader->size < 2 || reader->pos > reader->size - 2) {
        reader->left = 0;
        return false;
    }
    const uint8_t descriptor = reader->buf[reader->pos];
    const unsigned size = reader->buf[reader->pos + 1];
    if (size > reader->size - reader->pos - 2) {
        reader->left = 0;
        return false;
    }
    item->kind = descriptor >> 4;
    item->flags = descriptor & 15u;
    item->size = size;
    item->data = reader->buf + reader->pos + 2;
    reader->pos += 2 + size;
    reader->left--;
    return true;
}

/* 下一個參數項；略過 %{mask.xxx} 在參數前放的 mask 項（8 bytes 遮罩名稱，例如 "hash"，不是指標）。
 * 2026-11-19 os_log 趟：沒略過時 %{private,mask.hash}@ 把 "hash" 當物件交給 CFGetTypeID，bluetoothd SIGSEGV
 * far=0x68736170（"hash"+8）。 */
static inline bool fuxi_oslog_take(struct fuxi_oslog_reader *reader, struct fuxi_oslog_item *item)
{
    while (fuxi_oslog_next(reader, item)) {
        if (item->kind != FUXI_OSLOG_KIND_MASK) {
            return true;
        }
    }
    return false;
}

static inline bool fuxi_oslog_peek_kind(const struct fuxi_oslog_reader *reader, unsigned *kind)
{
    struct fuxi_oslog_reader copy = *reader;
    struct fuxi_oslog_item item;
    if (!fuxi_oslog_take(&copy, &item)) {
        return false;
    }
    *kind = item.kind;
    return true;
}

/* 小端純量（1..8 bytes）；signed 時依 size 做符號延伸 */
static inline uint64_t fuxi_oslog_unsigned(const struct fuxi_oslog_item *item)
{
    uint64_t value = 0;
    const unsigned size = item->size < 8 ? item->size : 8;
    for (unsigned index = 0; index < size; index++) {
        value |= (uint64_t)item->data[index] << (8 * index);
    }
    return value;
}

static inline int64_t fuxi_oslog_signed(const struct fuxi_oslog_item *item)
{
    const uint64_t value = fuxi_oslog_unsigned(item);
    if (item->size && item->size < 8 && (value >> (8 * item->size - 1)) & 1) {
        return (int64_t)(value | (~UINT64_C(0) << (8 * item->size)));
    }
    return (int64_t)value;
}

static inline const void *fuxi_oslog_pointer(const struct fuxi_oslog_item *item)
{
    const void *pointer = NULL;
    if (item->size == sizeof(pointer)) {
        memcpy(&pointer, item->data, sizeof(pointer));
    }
    return pointer;
}

static inline void fuxi_oslog_string(struct fuxi_oslog_out *out, const struct fuxi_oslog_item *item, long precision)
{
    if (item->kind != FUXI_OSLOG_KIND_STRING || item->size != sizeof(void *)) {
        fuxi_oslog_puts(out, item->kind == FUXI_OSLOG_KIND_WIDE ? "<wide>" : "<?>");
        return;
    }
    if (item->flags & FUXI_OSLOG_FLAG_PRIVATE) {
        fuxi_oslog_puts(out, "<private>");
        return;
    }
    const char *text = fuxi_oslog_pointer(item);
    if (!text) {
        fuxi_oslog_puts(out, "(null)");
        return;
    }
    size_t limit = FUXI_OSLOG_STRING_MAX;
    if (precision >= 0 && (size_t)precision < limit) {
        limit = (size_t)precision;
    }
    size_t length = 0;
    while (length < limit && text[length]) {
        length++;
    }
    fuxi_oslog_put(out, text, length);
}

/* format＋buf → out（cap ≥ 1，結尾補 0），回輸出長度 */
static inline size_t fuxi_oslog_format(char *buffer, size_t cap, const char *format, const uint8_t *buf,
    uint32_t size, fuxi_oslog_describe_fn describe)
{
    struct fuxi_oslog_out out = { buffer, cap, 0 };
    struct fuxi_oslog_reader reader = { buf, buf ? size : 0, 2, buf && size >= 2 ? buf[1] : 0 };
    struct fuxi_oslog_item item;
    if (!cap) {
        return 0;
    }
    buffer[0] = 0;
    if (!format) {
        fuxi_oslog_puts(&out, "(no format)");
        return out.len;
    }
    for (const char *p = format; *p && out.len + 1 < out.cap;) {
        if (*p != '%') {
            const char *next = strchr(p, '%');
            const size_t length = next ? (size_t)(next - p) : strlen(p);
            fuxi_oslog_put(&out, p, length);
            p += length;
            continue;
        }
        p++;
        if (*p == '%') {
            fuxi_oslog_put(&out, "%", 1);
            p++;
            continue;
        }
        while (*p == '{') {
            const char *end = strchr(p, '}');
            p = end ? end + 1 : p + strlen(p);
        }
        while (*p && strchr("-+ #0'", *p)) {
            p++;
        }
        bool width_argument = false;
        if (*p == '*') {
            width_argument = true;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }
        long precision = -1;
        bool precision_argument = false;
        bool precision_constant = false;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                precision_argument = true;
                p++;
            } else {
                precision_constant = true;
                precision = 0;
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p++ - '0');
                    if (precision > 100000) {
                        precision = 100000;
                    }
                }
            }
        }
        while (*p && strchr("hlqjztL", *p)) {
            p++;
        }
        const char conversion = *p;
        if (!conversion) {
            break;
        }
        p++;
        /* clang OSLog.cpp 的項目順序：mask、寬度 *、精度 *（%s／%P 是 count 項）、%.N s／%.N P 的常數 count、參數 */
        if (width_argument) {
            fuxi_oslog_take(&reader, &item);
        }
        if (precision_argument) {
            precision = fuxi_oslog_take(&reader, &item) ? (long)fuxi_oslog_signed(&item) : 0;
        } else if (precision_constant && (conversion == 's' || conversion == 'S' || conversion == 'P')) {
            unsigned kind;
            if (fuxi_oslog_peek_kind(&reader, &kind) && kind == FUXI_OSLOG_KIND_COUNT) {
                fuxi_oslog_take(&reader, &item);
            }
        }
        if (!fuxi_oslog_take(&reader, &item)) {
            fuxi_oslog_puts(&out, "<?>");
            continue;
        }
        switch (conversion) {
        case 's':
            fuxi_oslog_string(&out, &item, precision);
            break;
        case '@':
            if (item.kind != FUXI_OSLOG_KIND_OBJECT || item.size != sizeof(void *)) {
                fuxi_oslog_puts(&out, "<?>");
            } else if (item.flags & FUXI_OSLOG_FLAG_PRIVATE) {
                fuxi_oslog_puts(&out, "<private>");
            } else if (!fuxi_oslog_pointer(&item)) {
                fuxi_oslog_puts(&out, "(null)");
            } else {
                char text[FUXI_OSLOG_STRING_MAX];
                const size_t length = describe ? describe(fuxi_oslog_pointer(&item), text, sizeof(text)) : 0;
                fuxi_oslog_put(&out, length ? text : "<obj>", length ? (length < sizeof(text) ? length : sizeof(text)) : 5);
            }
            break;
        case 'd':
        case 'i':
            fuxi_oslog_putf(&out, "%lld", (long long)fuxi_oslog_signed(&item));
            break;
        case 'u':
            fuxi_oslog_putf(&out, "%llu", (unsigned long long)fuxi_oslog_unsigned(&item));
            break;
        case 'x':
        case 'X':
            fuxi_oslog_putf(&out, conversion == 'x' ? "%llx" : "%llX", (unsigned long long)fuxi_oslog_unsigned(&item));
            break;
        case 'o':
            fuxi_oslog_putf(&out, "%llo", (unsigned long long)fuxi_oslog_unsigned(&item));
            break;
        case 'p':
            fuxi_oslog_putf(&out, "0x%llx", (unsigned long long)fuxi_oslog_unsigned(&item));
            break;
        case 'c': {
            const char character = (char)(fuxi_oslog_unsigned(&item) & 0xff);
            fuxi_oslog_put(&out, character ? &character : "?", 1);
            break;
        }
        case 'm':
            fuxi_oslog_putf(&out, "errno=%lld", (long long)fuxi_oslog_signed(&item));
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            double value = 0;
            if (item.size == sizeof(value)) {
                memcpy(&value, item.data, sizeof(value));
            }
            fuxi_oslog_putf(&out, "%g", value);
            break;
        }
        case 'P':
            fuxi_oslog_puts(&out, "<data>");
            break;
        default:
            fuxi_oslog_puts(&out, "<?>");
            break;
        }
    }
    return out.len;
}

static inline bool fuxi_oslog_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 一筆紀錄維持一行：控制字元改空白；藍牙位址形式 HH:HH:HH:HH:HH:HH（或 -）遮成 xx，console 不留 BD 位址。
 * 回遮掉的位址數。 */
static inline unsigned fuxi_oslog_sanitize(char *text, size_t length)
{
    unsigned masked = 0;
    for (size_t index = 0; index < length; index++) {
        const unsigned char c = (unsigned char)text[index];
        if (c < 0x20 || c == 0x7f) {
            text[index] = ' ';
        }
    }
    for (size_t index = 0; index + 17 <= length; index++) {
        bool address = (index == 0 || !fuxi_oslog_hex(text[index - 1])) &&
            (index + 17 == length || !fuxi_oslog_hex(text[index + 17]));
        const char separator = text[index + 2];
        for (unsigned part = 0; address && part < 6; part++) {
            const size_t at = index + 3 * part;
            address = fuxi_oslog_hex(text[at]) && fuxi_oslog_hex(text[at + 1]) &&
                (part == 5 || ((separator == ':' || separator == '-') && text[at + 2] == separator));
        }
        if (address) {
            for (unsigned part = 0; part < 6; part++) {
                text[index + 3 * part] = 'x';
                text[index + 3 * part + 1] = 'x';
            }
            masked++;
            index += 16;
        }
    }
    return masked;
}

#endif
