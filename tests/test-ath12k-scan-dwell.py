import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


HOST = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nw_control.h"
typedef uint8_t u8;
typedef uint32_t u32;
#define DIV_ROUND_UP(value,divisor) (((value)+(divisor)-1)/(divisor))
#define IEEE80211_MAX_SSID_LEN 32
#define IEEE80211_CHAN_DISABLED 1
#define IEEE80211_CHAN_NO_20MHZ 2
#define NUM_NL80211_BANDS 2
#define GFP_KERNEL 0
#define SCAN_DWELL_MODE_STATIC 4
struct ieee80211_channel { unsigned flags, center_freq; };
struct cfg80211_ssid { u8 ssid[32]; unsigned ssid_len; };
struct ieee80211_supported_band {
    unsigned n_bitrates, n_channels;
    struct ieee80211_channel *channels;
};
struct wiphy {
    unsigned max_scan_ssids, max_scan_ie_len;
    struct ieee80211_supported_band *bands[2];
};
struct cfg80211_scan_request {
    struct cfg80211_ssid *ssids;
    unsigned n_ssids, n_channels, rates[2];
    const u8 *ie;
    size_t ie_len;
    u8 bssid[6];
    void *wdev;
    struct wiphy *wiphy;
    unsigned scan_start;
    int tsf_report_link_id;
    uint16_t duration;
    bool duration_mandatory;
    struct ieee80211_channel *channels[];
};
struct cfg80211_scan_request_int { struct cfg80211_scan_request req; };
struct cfg80211_ops { int scan; };
struct cfg80211_registered_device {
    struct wiphy wiphy;
    struct cfg80211_ops *ops;
    struct cfg80211_scan_request_int *scan_req;
    void *scan_msg;
};
struct net_device { void *ieee80211_ptr; };
static struct net_device device;
static struct net_device *primary = &device;
static unsigned jiffies, allocations, frees, started;
static void *kzalloc(size_t size,unsigned flags) {
    (void)flags; void *memory=calloc(1,size); if(memory) allocations++; return memory;
}
static void kfree(void *memory) { if(memory) frees++; free(memory); }
static unsigned ieee80211_get_num_supported_channels(struct wiphy *wiphy) {
    unsigned count=0;
    for(unsigned band=0;band<2;band++)
        if(wiphy->bands[band]) count+=wiphy->bands[band]->n_channels;
    return count;
}
static struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy,u32 frequency) {
    for(unsigned band=0;band<2;band++) if(wiphy->bands[band])
        for(unsigned index=0;index<wiphy->bands[band]->n_channels;index++)
            if(wiphy->bands[band]->channels[index].center_freq==frequency)
                return &wiphy->bands[band]->channels[index];
    return NULL;
}
static u32 get_unaligned_le32(const void *memory) {
    const u8 *bytes=memory;
    return bytes[0]|(u32)bytes[1]<<8|(u32)bytes[2]<<16|(u32)bytes[3]<<24;
}
static void eth_broadcast_addr(u8 *bytes) { memset(bytes,255,6); }
static int cfg80211_scan(struct cfg80211_registered_device *rdev) {
    assert(rdev->scan_req); started++; return 0;
}
static void dev_hold(struct net_device *dev) { assert(dev==primary); }
static void nl80211_send_scan_start(struct cfg80211_registered_device *rdev,void *wdev) {
    assert(rdev->scan_req && wdev==primary->ieee80211_ptr);
}
struct ath12k_wmi_scan_req_arg {
    u32 dwell_time_active, dwell_time_active_2g, dwell_time_passive;
    u32 dwell_time_active_6g, dwell_time_passive_6g, burst_duration;
    u32 adaptive_dwell_time_mode;
};
'''

TEST = r'''
static struct cfg80211_registered_device rdev;
static u8 packet[2048];
static struct nw_control_builder request;
static struct nw_control_view view;
static void begin(void) {
    assert(!nw_control_begin(&request,packet,sizeof(packet),NW_CONTROL_REQUEST,NW_CONTROL_SCAN,1,9,2,0));
}
static int run(void) {
    assert(!nw_control_decode(packet,request.length,&view)); return scan(&rdev,&view);
}
static void release_scan(void) { kfree(rdev.scan_req); rdev.scan_req=NULL; }
int main(void) {
    _Static_assert(NW_CONTROL_ATTR_SCAN_DWELL_MS==46,"scan ABI");
    _Static_assert(NW_CONTROL_ATTR_COUNT==47,"scan attribute bound");
    struct ieee80211_channel channels[64]={0};
    for(unsigned index=0;index<64;index++) channels[index].center_freq=2412+index*5;
    struct ieee80211_supported_band band={.n_bitrates=8,.n_channels=1,.channels=channels};
    struct cfg80211_ops operations={.scan=1};
    rdev.ops=&operations; rdev.wiphy.bands[0]=&band;
    rdev.wiphy.max_scan_ssids=16; rdev.wiphy.max_scan_ie_len=512;
    begin(); assert(!run() && !rdev.scan_req->req.duration && !rdev.scan_req->req.duration_mandatory);
    struct ath12k_wmi_scan_req_arg arg; memset(&arg,0x5a,sizeof(arg));
    struct ath12k_wmi_scan_req_arg untouched=arg;
    ath12k_mac_scan_dwell(&arg,&rdev.scan_req->req);
    assert(!memcmp(&arg,&untouched,sizeof(arg))); release_scan();
    unsigned cases=0;
    for(unsigned milliseconds=1;milliseconds<=1000;milliseconds++) {
        begin(); assert(!nw_control_put_u32(&request,NW_CONTROL_ATTR_SCAN_DWELL_MS,milliseconds));
        assert(!run() && rdev.scan_req->req.duration_mandatory);
        const struct cfg80211_scan_request *actual=&rdev.scan_req->req;
        assert((unsigned)actual->duration*1024>=milliseconds*1000);
        assert((unsigned)actual->duration*1024<milliseconds*1000+1024);
        memset(&arg,0,sizeof(arg)); ath12k_mac_scan_dwell(&arg,actual);
        assert(arg.dwell_time_active>=milliseconds && arg.dwell_time_active<=milliseconds+2);
        assert(arg.dwell_time_active_2g==arg.dwell_time_active &&
               arg.dwell_time_passive==arg.dwell_time_active &&
               arg.dwell_time_active_6g==arg.dwell_time_active &&
               arg.dwell_time_passive_6g==arg.dwell_time_active &&
               arg.burst_duration==arg.dwell_time_active &&
               arg.adaptive_dwell_time_mode==SCAN_DWELL_MODE_STATIC);
        if(milliseconds==110) assert(actual->duration==108 && arg.dwell_time_active==111);
        release_scan(); cases++;
    }
    for(unsigned size=0;size<=8;size++) if(size!=4) {
        begin(); u8 invalid[8]={110};
        assert(!nw_control_put(&request,NW_CONTROL_ATTR_SCAN_DWELL_MS,invalid,size));
        unsigned previous=started; assert(run()==-EINVAL && started==previous && !rdev.scan_req); cases++;
    }
    const u32 invalid_values[]={0,1001,65535,(u32)-1};
    for(unsigned index=0;index<4;index++) {
        begin(); assert(!nw_control_put_u32(&request,NW_CONTROL_ATTR_SCAN_DWELL_MS,invalid_values[index]));
        unsigned previous=started; assert(run()==-EINVAL && started==previous && !rdev.scan_req); cases++;
    }
    band.n_channels=64; begin(); nw_control_put_u32(&request,NW_CONTROL_ATTR_SCAN_DWELL_MS,110);
    unsigned previous=started; assert(run()==-EINVAL && started==previous && !rdev.scan_req);
    begin(); nw_control_put_u32(&request,NW_CONTROL_ATTR_SCAN_DWELL_MS,110);
    u8 frequency[4]={0x6c,9,0,0};
    nw_control_put(&request,NW_CONTROL_ATTR_SCAN_FREQUENCIES,frequency,4);
    assert(!run() && rdev.scan_req->req.n_channels==1);
    assert(run()==-EBUSY); release_scan();
    assert(allocations==frees);
    printf("PASS production scan dwell: %u input cases, TU conversion, five WMI dwell fields, static mode, budget, busy, unchanged defaults; radio simulated\n",cases);
}
'''


def function(source, name):
    match = re.search(r'static (?:int|void) ' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError(f'Missing production function {name}')
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('module', type=Path)
    arguments = parser.parse_args()
    module = arguments.module.resolve()
    control = (module / 'src/nw_cfg80211_control.c').read_text()
    mac = (module / 'out/ath12k-src/mac.c').read_text()
    wmi = (module / 'out/ath12k-src/wmi.c').read_text()
    assert 'nw_control_put_u32(reply, NW_CONTROL_ATTR_FEATURES, NW_CONTROL_FEATURE_SCAN_DWELL_MS)' in control
    assert mac.count('ath12k_mac_scan_dwell(arg, req);') == 1
    for field in ('dwell_time_active', 'dwell_time_active_2g', 'dwell_time_passive',
                  'dwell_time_active_6g', 'dwell_time_passive_6g', 'burst_duration'):
        assert f'cmd->{field} = cpu_to_le32(arg->{field});' in wmi
    code = HOST + function(control, 'scan') + function(mac, 'ath12k_mac_scan_dwell') + TEST
    with tempfile.TemporaryDirectory(prefix='ath12k-scan-dwell-') as temporary:
        directory = Path(temporary)
        source = directory / 'scan.c'
        binary = directory / 'scan'
        source.write_text(code)
        command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                   '-I', str(module / 'include'), str(source),
                   str(module / 'src/nw_control_wire.c'), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
