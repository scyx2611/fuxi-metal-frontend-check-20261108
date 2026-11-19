/*
 * fuxi,bt-probe：從 SpringBoard 驗證已初始化的 WCN7850。
 * kernel control 及 Skywalk slot 介面取自 XNU 8796 公開標頭，socket options 對照
 * docs-lanes/bluetooth-ios16-abi.md。只送 HCI Read Local Version，不做掃描或配對。
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

enum {
    BT_AF_SYSTEM = 32,
    BT_AF_SYS_KERNCONTROL = 2,
    BT_SYSPROTO_CONTROL = 2,
    BT_FIRST_TX_RING = 0,
    BT_FIRST_RX_RING = 2,
    BT_SYNC_TX = 0,
    BT_SYNC_RX = 1
};

struct bt_control_info {
    uint32_t id;
    char name[96];
};

struct bt_control_address {
    uint8_t length, family;
    uint16_t sysaddr;
    uint32_t id, unit, reserved[5];
};

struct bt_slot {
    uint16_t flags, length;
    uint32_t index;
    uint64_t external, buffer, metadata;
    uint32_t reserved[8];
};
_Static_assert(sizeof(struct bt_control_info) == 100, "CTLIOCGINFO ABI");
_Static_assert(sizeof(struct bt_control_address) == 32, "sockaddr_ctl ABI");
_Static_assert(sizeof(struct bt_slot) == 64, "Skywalk slot properties ABI");

struct bt_skywalk {
    void *(*create)(const uint8_t *, uint32_t);
    void (*destroy)(void *);
    uint32_t (*ring_id)(const void *, int);
    void *(*tx_ring)(const void *, uint32_t);
    void *(*rx_ring)(const void *, uint32_t);
    void *(*next_slot)(const void *, const void *, struct bt_slot *);
    void (*set_slot)(const void *, const void *, const struct bt_slot *);
    int (*advance)(void *, const void *);
    int (*sync)(const void *, int);
};

static uint64_t bt_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u + (unsigned)now.tv_nsec / 1000000u;
}

static int bt_version(const struct bt_skywalk *skywalk, void *channel, void *tx, void *rx)
{
    const uint8_t command[] = {0x01, 0x01, 0x10, 0x00};
    struct bt_slot props = {0};
    void *slot = skywalk->next_slot(tx, NULL, &props);
    if (!slot || !props.buffer || props.length < sizeof(command)) {
        return ENOBUFS;
    }
    memcpy((void *)(uintptr_t)props.buffer, command, sizeof(command));
    props.length = sizeof(command);
    skywalk->set_slot(tx, slot, &props);
    if (skywalk->advance(tx, slot) || skywalk->sync(channel, BT_SYNC_TX)) {
        return errno ? errno : EIO;
    }

    /* 核心依 H4 封包交付；其他事件或 ACL 整包略過，只看 Read Local Version 的 Command Complete。 */
    uint8_t packet[16];
    size_t received = 0, total = 0;
    unsigned skipped = 0;
    uint64_t start = bt_monotonic_ms();
    for (unsigned tries = 0; tries < 300 && bt_monotonic_ms() - start < 3000; ++tries) {
        if (skywalk->sync(channel, BT_SYNC_RX)) {
            return errno ? errno : EIO;
        }
        while ((slot = skywalk->next_slot(rx, NULL, &props))) {
            if (!props.buffer || props.length > 4096) {
                return EPROTO;
            }
            const uint8_t *bytes = (const uint8_t *)(uintptr_t)props.buffer;
            for (size_t index = 0; index < props.length; ++index) {
                if (received < sizeof(packet)) {
                    packet[received] = bytes[index];
                }
                ++received;
                const unsigned header = packet[0] == 0x04 ? 3 : packet[0] == 0x02 || packet[0] == 0x05 ? 5 :
                    packet[0] == 0x01 || packet[0] == 0x03 ? 4 : 0;
                if (!header) {
                    dprintf(2, "fuxi: Bluetooth HCI probe unexpected byte %#x at packet start (skipped %u)\n",
                        packet[0], skipped);
                    return EPROTO;
                }
                if (received == header) {
                    total = header + (packet[0] == 0x04 ? packet[2] :
                        packet[0] == 0x02 ? (size_t)(packet[3] | packet[4] << 8) :
                        packet[0] == 0x05 ? (size_t)((packet[3] | packet[4] << 8) & 0x3fff) : packet[3]);
                }
                if (received < header || received != total) {
                    continue;
                }
                if (received == 15 && packet[1] == 0x0e && packet[4] == 0x01 && packet[5] == 0x10) {
                    unsigned manufacturer = packet[11] | (unsigned)packet[12] << 8;
                    dprintf(2, "fuxi: Bluetooth HCI Read Local Version status=%u hci=%u lmp=%u manufacturer=%#x skipped=%u\n",
                        packet[6], packet[7], packet[10], manufacturer, skipped);
                    return packet[6] == 0 && manufacturer == 0x1d ? 0 : EPROTO;
                }
                if (skipped++ < 4) {
                    dprintf(2, "fuxi: Bluetooth HCI probe skipped packet type=%#x code=%#x bytes=%zu\n",
                        packet[0], packet[1], received);
                }
                received = total = 0;
            }
            if (skywalk->advance(rx, slot)) {
                return EIO;
            }
        }
        usleep(10000);
    }
    return ETIMEDOUT;
}

static int bt_exchange(const struct bt_skywalk *skywalk)
{
    struct bt_control_info info = {.name = "com.apple.uart.sk.bluetooth"};
    struct bt_control_address address = {
        .length = sizeof(address), .family = BT_AF_SYSTEM, .sysaddr = BT_AF_SYS_KERNCONTROL,
    };
    const uint32_t spec[] = {1024, 63, 63};
    uint8_t termios[72], uuid[16];
    void *channel = NULL;
    const char *stage = "socket";
    int error = 0;
    int fd = socket(BT_AF_SYSTEM, SOCK_STREAM, BT_SYSPROTO_CONTROL);
    if (fd < 0) {
        error = errno;
        goto finish;
    }
    stage = "lookup";
    if (ioctl(fd, _IOWR('N', 3, struct bt_control_info), &info)) {
        error = errno;
        goto finish;
    }
    address.id = info.id;
    stage = "connect";
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address))) {
        error = errno;
        goto finish;
    }
    socklen_t length = sizeof(termios);
    stage = "termios";
    if (getsockopt(fd, BT_SYSPROTO_CONTROL, 0x40487413, termios, &length) ||
        length != sizeof(termios) ||
        setsockopt(fd, BT_SYSPROTO_CONTROL, (int)0x80487414u, termios, sizeof(termios))) {
        error = errno ? errno : EPROTO;
        goto finish;
    }
    stage = "spec";
    if (setsockopt(fd, BT_SYSPROTO_CONTROL, (int)0x800c5414u, spec, sizeof(spec))) {
        error = errno;
        goto finish;
    }
    stage = "uuid";
    length = sizeof(uuid);
    if (getsockopt(fd, BT_SYSPROTO_CONTROL, 0x40105412, uuid, &length) || length != sizeof(uuid)) {
        error = errno ? errno : EPROTO;
        goto finish;
    }
    stage = "channel";
    channel = skywalk->create(uuid, 0);
    if (!channel) {
        error = errno ? errno : ENXIO;
        goto finish;
    }
    uint32_t txid = skywalk->ring_id(channel, BT_FIRST_TX_RING);
    uint32_t rxid = skywalk->ring_id(channel, BT_FIRST_RX_RING);
    dprintf(2, "fuxi: Bluetooth HCI probe rings tx=%u rx=%u\n", txid, rxid);
    stage = "rings";
    void *tx = skywalk->tx_ring(channel, txid);
    void *rx = skywalk->rx_ring(channel, rxid);
    if (!tx || !rx) {
        error = ENXIO;
        goto finish;
    }
    stage = "read-version";
    error = bt_version(skywalk, channel, tx, rx);

finish:
    if (channel) {
        skywalk->destroy(channel);
    }
    if (fd >= 0) {
        close(fd);
    }
    dprintf(2, "fuxi: Bluetooth HCI probe stage=%s error=%d\n", stage, error);
    return error;
}

void *fuxi_bluetooth_probe(void *argument)
{
    (void)argument;
    int status = EINPROGRESS;
    int error = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        size_t length = sizeof(status);
        int result = sysctlbyname("kern.qbt_boot", &status, &length, NULL, 0);
        error = result ? errno : length == sizeof(status) ? 0 : EPROTO;
        if ((!error && status != EINPROGRESS) || (error && error != EAGAIN)) {
            break;
        }
    }
    if (error || status) {
        dprintf(2, "fuxi: Bluetooth HCI probe boot status=%d error=%d\n", status, error);
        return NULL;
    }
    /* XNU 的 libsyscall 將 os_channel.c 編入 libsystem_kernel。 */
    void *library = dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!library) {
        dprintf(2, "fuxi: Bluetooth HCI probe libsystem_kernel unavailable\n");
        return NULL;
    }
    const struct bt_skywalk skywalk = {
        .create = dlsym(library, "os_channel_create"),
        .destroy = dlsym(library, "os_channel_destroy"),
        .ring_id = dlsym(library, "os_channel_ring_id"),
        .tx_ring = dlsym(library, "os_channel_tx_ring"),
        .rx_ring = dlsym(library, "os_channel_rx_ring"),
        .next_slot = dlsym(library, "os_channel_get_next_slot"),
        .set_slot = dlsym(library, "os_channel_set_slot_properties"),
        .advance = dlsym(library, "os_channel_advance_slot"),
        .sync = dlsym(library, "os_channel_sync"),
    };
    if (skywalk.create && skywalk.destroy && skywalk.ring_id && skywalk.tx_ring && skywalk.rx_ring &&
        skywalk.next_slot && skywalk.set_slot && skywalk.advance && skywalk.sync) {
        if (bt_exchange(&skywalk) == 0) {
            error = bt_exchange(&skywalk);
            dprintf(2, "fuxi: Bluetooth HCI probe reconnect error=%d\n", error);
        }
    } else {
        dprintf(2, "fuxi: Bluetooth HCI probe missing Skywalk exports\n");
    }
    dlclose(library);
    return NULL;
}
