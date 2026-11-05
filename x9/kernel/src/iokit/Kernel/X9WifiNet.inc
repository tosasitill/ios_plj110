/*
 * X9 Wi-Fi, iOS side (track A of docs/network/wifi-plan.md), included by
 * SweetX9NetworkStack.cpp.
 *
 * The network half is ported from UniXNU-sheng package/WifiNet.cpp
 * (https://github.com/UniXNU/UniXNU-sheng, 516a780473a2ac59c8b438e132bb60204dda9e22,
 * SPDX-License-Identifier: BSD-2-Clause-Patent): the Ethernet interface en0 the
 * system routes through, the registry entries by which wifid and configd find a
 * wireless interface, and the Apple80211 requests (SIOCGA80211/SIOCSA80211 on
 * en0) wifid uses to scan, join and read state. Request numbers and layouts are
 * theirs; on iOS 16.7.16 every request is logged ("X9 WIFI REQ") so differences
 * show in the first run.
 *
 * Below it sits a station interface shaped like Ath12kWifi include/nw_abi.h
 * (same repository family, same licence). Until the MT6653 radio exists
 * (track B) the only backend is a FAKE radio: two synthetic networks, a join
 * that succeeds without moving a frame. It is off unless x9_wifi_fake=1.
 *
 * Nothing here touches a device register.
 */
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/mbuf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_private.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <netinet/in.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <stdarg.h>

/* the header declares it without C linkage; the symbol is the C one */
extern "C" int x9_kev_post(struct kev_msg *) __asm__("_kev_post_msg");
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define X9_SIOCSA80211 0x802869c8UL   /* _IOW('i', 200, struct apple80211req) */
#define X9_SIOCGA80211 0xc02869c9UL   /* _IOWR('i', 201, struct apple80211req) */

namespace x9wifi { namespace {

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char line[200]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  IOLog("X9 WIFI: %s\n", line);
}

/* ======================= station interface (nw_abi.h shape) ======================= */
struct bss_info {
  uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32];
  uint32_t freq_mhz; int8_t signal_dbm; uint16_t caps, beacon_int; uint32_t age_ms;
  uint16_t ie_len; uint8_t ie[384];
};
enum { STA_OFF, STA_IDLE, STA_SCANNING, STA_AUTH, STA_ASSOC, STA_KEYS, STA_UP };
enum { RES_OK = 0, RES_NOT_FOUND = 1, RES_TIMEOUT = 2, RES_REJECTED = 3, RES_BAD_KEY = 4, RES_UNSUPPORTED = 5, RES_FAILED = 6 };
struct sta_status { uint32_t state; int32_t result; uint8_t bssid[6]; uint8_t ssid_len; uint8_t ssid[32]; uint32_t freq_mhz; int8_t signal_dbm; uint8_t mac[6]; };
struct sta_events {
  void (*scan_done)(void);
  void (*connect_done)(int result);
  void (*link_lost)(int reason);
  void (*rx)(const uint8_t *eth, size_t len);
};
struct sta_stats { uint32_t stage, scans, joins, tx_frames, tx_bytes; int last_err; };

/* ---- fake radio: two synthetic networks, events from thread calls ---- */
namespace fake {
IOLock *lock;
const sta_events *events;
sta_status status;
sta_stats stats;
bss_info bss[2];
unsigned bss_count;
thread_call_t scanCall, joinCall;
int joinResult;

unsigned ie_put(uint8_t *p, unsigned at, uint8_t id, const uint8_t *v, uint8_t n) {
  p[at] = id; p[at + 1] = n; memcpy(p + at + 2, v, n); return at + 2u + n;
}
void make(bss_info &b, const char *ssid, uint8_t last, uint32_t mhz, int8_t dbm, bool rsn) {
  memset(&b, 0, sizeof b);
  static const uint8_t base[6] = { 0x02, 0x58, 0x39, 0xfa, 0x4e, 0x00 };
  memcpy(b.bssid, base, 6); b.bssid[5] = last;
  b.ssid_len = (uint8_t)strlen(ssid); memcpy(b.ssid, ssid, b.ssid_len);
  b.freq_mhz = mhz; b.signal_dbm = dbm; b.beacon_int = 100; b.age_ms = 0;
  b.caps = 0x0001 | 0x0400 | (rsn ? 0x0010 : 0);           /* ESS, short slot, privacy */
  static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
  static const uint8_t country[6] = { 'C', 'N', ' ', 0x01, 0x0d, 0x14 };
  static const uint8_t rsnie[20] = { 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                     0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };   /* CCMP / CCMP / PSK */
  uint8_t ch = mhz >= 4900 ? (uint8_t)((mhz - 5000) / 5) : (uint8_t)((mhz - 2407) / 5);
  unsigned at = 0;
  at = ie_put(b.ie, at, 0, b.ssid, b.ssid_len);
  at = ie_put(b.ie, at, 1, rates, sizeof rates);
  at = ie_put(b.ie, at, 3, &ch, 1);
  at = ie_put(b.ie, at, 7, country, sizeof country);
  if (rsn) at = ie_put(b.ie, at, 48, rsnie, sizeof rsnie);
  b.ie_len = (uint16_t)at;
}
void scan_fired(thread_call_param_t, thread_call_param_t) {
  IOLockLock(lock);
  if (status.state == STA_SCANNING) status.state = STA_IDLE;
  bss_count = 2; stats.scans++;
  const sta_events *ev = events;
  IOLockUnlock(lock);
  log("fake radio: scan done, %u networks", 2u);
  if (ev && ev->scan_done) ev->scan_done();
}
void join_fired(thread_call_param_t, thread_call_param_t) {
  IOLockLock(lock);
  int r = joinResult;
  if (r == RES_OK) status.state = STA_UP; else status.state = STA_IDLE;
  status.result = r; stats.joins++;
  const sta_events *ev = events;
  IOLockUnlock(lock);
  log("fake radio: join finished %d", r);
  if (ev && ev->connect_done) ev->connect_done(r);
}
void after(thread_call_t call, unsigned ms) {
  uint64_t at; clock_interval_to_deadline(ms, kMillisecondScale, &at); thread_call_enter_delayed(call, at);
}
}  // namespace fake

int sta_start(const uint8_t mac[6]) {
  using namespace fake;
  lock = IOLockAlloc();
  scanCall = thread_call_allocate(scan_fired, nullptr);
  joinCall = thread_call_allocate(join_fired, nullptr);
  if (!lock || !scanCall || !joinCall) return -1;
  memset(&status, 0, sizeof status); memcpy(status.mac, mac, 6); status.state = STA_IDLE;
  make(bss[0], "X9-FAKE-OPEN", 1, 2437, -48, false);
  make(bss[1], "X9-FAKE-WPA2", 2, 5180, -61, true);
  bss_count = 0; stats.stage = 1;
  return 0;
}
void sta_set_events(const sta_events *ev) { IOLockLock(fake::lock); fake::events = ev; IOLockUnlock(fake::lock); }
int sta_scan(const uint8_t *, size_t) {
  using namespace fake;
  IOLockLock(lock); if (status.state == STA_IDLE) status.state = STA_SCANNING; IOLockUnlock(lock);
  after(scanCall, 800);
  return 0;
}
int sta_bss_count(void) { IOLockLock(fake::lock); int n = (int)fake::bss_count; IOLockUnlock(fake::lock); return n; }
int sta_bss_get(int i, bss_info *out) {
  using namespace fake;
  int r = -1;
  IOLockLock(lock);
  if (i >= 0 && (unsigned)i < bss_count) { memcpy(out, &bss[i], sizeof *out); r = 0; }
  IOLockUnlock(lock);
  return r;
}
int sta_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen) {
  using namespace fake;
  int found = -1;
  IOLockLock(lock);
  for (unsigned i = 0; i < 2; i++)
    if ((bssid && !memcmp(bssid, bss[i].bssid, 6)) || (len == bss[i].ssid_len && !memcmp(ssid, bss[i].ssid, len))) { found = (int)i; break; }
  if (found < 0) joinResult = RES_NOT_FOUND;
  else if ((bss[found].caps & 0x0010) && (!pass || plen < 8)) joinResult = RES_BAD_KEY;
  else {
    joinResult = RES_OK;
    memcpy(status.bssid, bss[found].bssid, 6); status.ssid_len = bss[found].ssid_len; memcpy(status.ssid, bss[found].ssid, 32);
    status.freq_mhz = bss[found].freq_mhz; status.signal_dbm = bss[found].signal_dbm;
  }
  status.state = STA_AUTH;
  IOLockUnlock(lock);
  log("fake radio: join requested: network %d, key length %u", found, (unsigned)plen);
  after(joinCall, 300);
  return 0;
}
void sta_disconnect(void) { IOLockLock(fake::lock); if (fake::status.state > STA_IDLE) fake::status.state = STA_IDLE; IOLockUnlock(fake::lock); }
int sta_tx(const uint8_t *, size_t len) {
  IOLockLock(fake::lock); fake::stats.tx_frames++; fake::stats.tx_bytes += (uint32_t)len; IOLockUnlock(fake::lock);
  return 0;   /* the fake radio swallows frames */
}
void sta_status_get(sta_status *st) { IOLockLock(fake::lock); *st = fake::status; IOLockUnlock(fake::lock); }

/* ============================ network side (WifiNet.cpp) ============================ */
struct apple80211req {
  char req_if_name[16];
  int32_t req_type;
  int32_t req_val;
  uint32_t req_len;
  user_addr_t req_data __attribute__((aligned(8)));
};
enum {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10, kScanResult = 11,
  kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20, kAssociateResult = 21,
  kDisassociate = 22, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kCountryCode = 51,
  kRadioInfo = 54, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103, kChannelsInfo = 207, kHwSupportedChannels = 254,
};
enum { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10 };
enum { kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21 };

struct channel { uint32_t version, channel, flags; };
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };
struct scan_result {
  uint32_t version; channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability; uint8_t bssid[6];
  uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms; uint8_t flags, unused2;
  uint16_t elements_length; uint8_t elements[1024];
};
struct assoc_request {
  uint32_t version; uint16_t mode, auth_lower, auth_upper, unused0; uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6], unused1[2];
  uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64]; uint8_t rest[0x250 - 0x88];
};
struct link_event { uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3]; uint32_t reason, subreason; uint8_t unused3[4]; };
static_assert(sizeof(scan_result) == 1164 && __builtin_offsetof(scan_result, ssid_length) == 0x60 && __builtin_offsetof(scan_result, age_ms) == 0x84 &&
              __builtin_offsetof(scan_result, elements) == 0x8c, "scan result layout");
static_assert(sizeof(assoc_request) == 592 && __builtin_offsetof(assoc_request, key) == 0x48, "association layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");

ifnet_t gIf;
uint8_t gMac[6];
bool gPower = true;
bool gKeyed;
uint32_t gRxFrames, gRxErrors; int gRxLastError;
int gAssocResult = kAssocOk;
unsigned gScanCursor, gScanning, gResultsShown;
uint32_t gLogged[256 / 32];
uint32_t gRequests, gRefused;
thread_call_t gCachedScan;

void post_event(uint32_t code, const void *payload, size_t length) {
  uint8_t data[128]; if (length > sizeof data - 16) length = sizeof data - 16;
  memset(data, 0, 16); snprintf(reinterpret_cast<char *>(data), 16, "en0"); if (length) memcpy(data + 16, payload, length);
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = 6; msg.kev_subclass = 1; msg.event_code = code;
  msg.dv[0].data_length = static_cast<uint32_t>(16 + length); msg.dv[0].data_ptr = data;
  x9_kev_post(&msg);
}
void post_link(bool up) {
  if (!gIf) return;
  ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);
  struct { struct net_event_data data; } payload; memset(&payload, 0, sizeof payload);
  payload.data.if_family = IFNET_FAMILY_ETHERNET; payload.data.if_unit = 0; snprintf(payload.data.if_name, sizeof payload.data.if_name, "en");
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = KEV_NETWORK_CLASS; msg.kev_subclass = KEV_DL_SUBCLASS; msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
  msg.dv[0].data_length = sizeof payload; msg.dv[0].data_ptr = &payload;
  x9_kev_post(&msg);
}

void on_scan_done() { __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); gScanCursor = 0; post_event(kEvScanDone, nullptr, 0); }
void cached_scan_done(thread_call_param_t, thread_call_param_t) { on_scan_done(); }
void on_connect_done(int result) {
  gAssocResult = result == RES_OK ? kAssocOk : result == RES_BAD_KEY ? kAssocBadKey : result == RES_TIMEOUT ? kAssocTimeout : kAssocFailed;
  log("join finished: station result %d", result);
  if (result == RES_OK) post_link(true);
  post_event(kEvAssocDone, nullptr, 0);
  if (result == RES_OK) {
    post_event(kEvSsidChanged, nullptr, 0); post_event(kEvBssidChanged, nullptr, 0);
    sta_status st; sta_status_get(&st);
    link_event ev; memset(&ev, 0, sizeof ev); ev.rssi = st.signal_dbm; ev.noise = static_cast<uint16_t>(-95); ev.snr = static_cast<uint16_t>(st.signal_dbm + 95);
    post_event(kEvLinkChanged, &ev, sizeof ev);
  }
}
void on_link_lost(int reason) {
  log("link lost: reason %d", reason);
  post_link(false);
  link_event ev; memset(&ev, 0, sizeof ev); ev.down = 1; ev.involuntary = 1; ev.reason = static_cast<uint32_t>(reason);
  post_event(kEvLinkChanged, &ev, sizeof ev);
}
void on_rx(const uint8_t *eth, size_t length) {
  if (!gIf || length < 14) return;
  enum : size_t { Align = 2 };
  mbuf_t m = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, length + Align, nullptr, &m) || !m) { gRxErrors++; return; }
  size_t done = 0; bool first = true;
  for (mbuf_t n = m; n; n = mbuf_next(n)) {
    size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
    if (first) { if (room < Align + 14) break; mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0); room -= Align; }
    const size_t take = length - done < room ? length - done : room;
    memcpy(mbuf_data(n), eth + done, take); mbuf_setlen(n, take); done += take; first = false;
  }
  if (done != length) { gRxErrors++; mbuf_freem(m); return; }
  mbuf_pkthdr_setlen(m, length); mbuf_pkthdr_setrcvif(m, gIf);
  mbuf_pkthdr_setheader(m, mbuf_data(m)); mbuf_adj(m, 14);
  struct ifnet_stat_increment_param inc; memset(&inc, 0, sizeof inc); inc.packets_in = 1; inc.bytes_in = static_cast<uint32_t>(length);
  const errno_t e = ifnet_input(gIf, m, &inc);
  if (e) { gRxErrors++; gRxLastError = e; } else gRxFrames++;
}
const sta_events gEvents = { on_scan_done, on_connect_done, on_link_lost, on_rx };

errno_t if_output(ifnet_t, mbuf_t m) {
  size_t length = mbuf_pkthdr_len(m);
  if (length >= 14 && length <= 2000) {
    uint8_t *frame = static_cast<uint8_t *>(IOMalloc(length));
    if (frame) {
      mbuf_copydata(m, 0, length, frame);
      if (!sta_tx(frame, length)) ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0);
      IOFree(frame, length);
    }
  }
  mbuf_freem(m);
  return 0;
}
void if_event_stub(ifnet_t, const struct kev_msg *) {}

void fill_channel(channel &c, uint32_t mhz) {
  c.version = 1;
  if (mhz >= 4900) { c.channel = (mhz - 5000) / 5; c.flags = 0x2 | 0x10; } else { c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5; c.flags = 0x2 | 0x8; }
}
void fill_result(scan_result &r, const bss_info &b) {
  memset(&r, 0, sizeof r); r.version = 1; fill_channel(r.ch, b.freq_mhz); r.noise_known = 1; r.noise = -95; r.rssi = b.signal_dbm; r.snr = b.signal_dbm + 95;
  r.beacon_interval = b.beacon_int; r.capability = b.caps; memcpy(r.bssid, b.bssid, 6); r.ssid_length = b.ssid_len; memcpy(r.ssid, b.ssid, 32);
  r.age_ms = b.age_ms; r.elements_length = b.ie_len > sizeof r.elements ? sizeof r.elements : b.ie_len; memcpy(r.elements, b.ie, r.elements_length);
}
int copy_out(const apple80211req &rq, const void *src, size_t size) {
  if (rq.req_len == 0 || !rq.req_data) return EINVAL;
  return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}
int answer_value(apple80211req &rq, uint32_t value) {
  if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
  const uint32_t w[2] = { 1, value }; return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}
int answer_bytes(apple80211req &rq, const void *src, uint32_t size) {
  if (!rq.req_data) return EINVAL;
  const uint32_t n = rq.req_len < size ? rq.req_len : size; rq.req_len = size; return copyout(src, rq.req_data, n);
}
int copy_in(const apple80211req &rq, void *dst, size_t size) {
  if (!rq.req_data) return EINVAL;
  memset(dst, 0, size);
  return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}
void country(char cc[3]) {
  cc[0] = 'X'; cc[1] = 'X'; cc[2] = 0; int best = -200; bss_info b;
  for (int i = 0, n = sta_bss_count(); i < n; ++i) {
    if (sta_bss_get(i, &b) || b.signal_dbm <= best) continue;
    for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1])
      if (b.ie[o] == 7 && b.ie[o + 1] >= 2 && b.ie[o + 2] >= 'A' && b.ie[o + 2] <= 'Z' && b.ie[o + 3] >= 'A' && b.ie[o + 3] <= 'Z') {
        cc[0] = static_cast<char>(b.ie[o + 2]); cc[1] = static_cast<char>(b.ie[o + 3]); best = b.signal_dbm; break; }
  }
}
int map_state(unsigned s) { switch (s) { case STA_SCANNING: return 1; case STA_AUTH: return 2; case STA_ASSOC: case STA_KEYS: return 3; case STA_UP: return 4; default: return 0; } }

union Out { uint8_t raw[5120]; channels_list chans; scan_result res; };

int apple80211(bool set, apple80211req &rq) {
  sta_status st; sta_status_get(&st);
  const int type = rq.req_type;
  Out *outp = static_cast<Out *>(IOMalloc(sizeof(Out)));
  if (!outp) return ENOMEM;
  memset(outp, 0, sizeof(Out));
  struct Free { Out *p; ~Free() { IOFree(p, sizeof(Out)); } } freeOut{outp};
  Out &out = *outp;
  uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
  if (set) {
    switch (type) {
    case kPower: { uint32_t in[6]; if (copy_in(rq, in, sizeof in)) return EFAULT; gPower = in[2] != 0; log("power set %u", in[2]); if (!gPower) sta_disconnect(); post_event(kEvPowerChanged, nullptr, 0); return 0; }
    case kScanReq: case kScanReqMultiple: {
      uint8_t buf[0x80]; if (copy_in(rq, buf, sizeof buf)) return EFAULT;
      uint32_t ssidLength; memcpy(&ssidLength, buf + 16, 4); const uint8_t *ssid = buf + 20;
      if (st.state >= STA_AUTH && sta_bss_count() > 0) {   /* joined: answer from the cache */
        if (!gCachedScan) gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
        gScanning = 1; gScanCursor = 0; gResultsShown = 24; uint64_t at; clock_interval_to_deadline(2000, kMillisecondScale, &at); thread_call_enter_delayed(gCachedScan, at); return 0; }
      gScanning = 1; gScanCursor = 0; gResultsShown = 0; sta_scan(ssidLength && ssidLength <= 32 ? ssid : nullptr, ssidLength <= 32 ? ssidLength : 0); return 0; }
    case kAssociate: {
      assoc_request a; if (copy_in(rq, &a, sizeof a)) return EFAULT;
      if (a.ssid_length > 32) return EINVAL;
      const bool hasBssid = (a.bssid[0] | a.bssid[1] | a.bssid[2] | a.bssid[3] | a.bssid[4] | a.bssid[5]) != 0;
      const bool hasKey = a.key_length != 0 && a.key_length <= 64; char pass[65]; memset(pass, 0, sizeof pass); if (hasKey) memcpy(pass, a.key, a.key_length);
      unsigned keyLength = a.key_length;
      if (hasKey && a.key_cipher == 6 && a.key_length == 32) { static const char hex[] = "0123456789abcdef"; for (int i = 0; i < 32; i++) { pass[2 * i] = hex[a.key[i] >> 4]; pass[2 * i + 1] = hex[a.key[i] & 15]; } keyLength = 64; }
      gAssocResult = kAssocFailed;
      log("join: ssid length %u, bssid given %d, key length %u cipher %u", a.ssid_length, hasBssid, a.key_length, a.key_cipher);
      gKeyed = hasKey; int r = sta_connect(a.ssid, a.ssid_length, hasBssid ? a.bssid : nullptr, hasKey ? pass : nullptr, hasKey ? keyLength : 0);
      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);
      return r ? EINVAL : 0; }
    case kDisassociate: case kDeauth: log("leave"); sta_disconnect(); post_link(false); return 0;
    case kScanCacheClear: case kLocale: case kCountryCode: case kPowerSave: case kAuthType: case kCipherKey: return 0;
    default: return EOPNOTSUPP;
    }
  }
  switch (type) {
  case kPower: w[0] = 1; w[1] = 1; w[2] = gPower ? 1 : 0; return copy_out(rq, w, 24);
  case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
  case kOpMode: return answer_value(rq, 1);
  case kAuthType: w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x8 : 0; return copy_out(rq, w, 12);
  case kPowerSave: return answer_value(rq, 0);
  case kCardCaps: {
    static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };
    for (uint8_t b : has) out.raw[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
    return answer_bytes(rq, out.raw, 12); }
  case kPhyMode: w[0] = 1; w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40; w[2] = st.state == STA_UP ? 0x8 : 0; return copy_out(rq, w, 12);
  case kSsid: if (st.state < STA_ASSOC) return ENOENT; return answer_bytes(rq, st.ssid, st.ssid_len);
  case kBssid: if (st.state < STA_ASSOC) return ENOENT; return answer_bytes(rq, st.bssid, 6);
  case kChannel: if (st.state < STA_AUTH) return ENOENT; w[0] = 1; fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz); return copy_out(rq, w, 16);
  case kRssi: case kNoise: {
    if (type == kRssi && st.state != STA_UP) return ENOENT;
    const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
    w[0] = 1; w[1] = 1; w[2] = 0; for (int i = 3; i < 13; i++) w[i] = v; return copy_out(rq, w, 13 * 4); }
  case kRate: return answer_value(rq, st.state == STA_UP ? 54 : 0);
  case kTxPower: return answer_value(rq, 15);
  case kDriverVersion: case kHardwareVersion: { const char *v = type == kDriverVersion ? "x9wifi fake 1" : "MT6653"; strlcpy(reinterpret_cast<char *>(out.raw), v, 256); return copy_out(rq, out.raw, 256); }
  case kCountryCode: { char cc[3]; country(cc); return answer_bytes(rq, cc, 3); }
  case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult));
  case kSupportedChannels: case kHwSupportedChannels: {
    out.chans.version = 1; unsigned n = 0;
    static const uint16_t known[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825 };
    for (uint16_t f : known) { fill_channel(out.chans.list[n], f); out.chans.list[n].flags |= 0x80; n++; }
    out.chans.count = n; return copy_out(rq, &out.chans, sizeof out.chans); }
  case kScanResult: {
    bss_info b; const int count = sta_bss_count();
    if (gScanCursor >= static_cast<unsigned>(count) || sta_bss_get(static_cast<int>(gScanCursor), &b)) { gScanCursor = 0; return EIO; }
    gScanCursor++; fill_result(out.res, b);
    if (gResultsShown < 24) { ++gResultsShown; log("scan result %u: %u MHz, %d dBm, %.*s", gScanCursor, b.freq_mhz, b.signal_dbm, b.ssid_len, reinterpret_cast<const char *>(b.ssid)); }
    return copy_out(rq, &out.res, sizeof out.res); }
  case kCurrentNetwork: {
    if (st.state < STA_ASSOC) return ENOENT;
    bss_info b; const int count = sta_bss_count();
    for (int i = 0; i < count; i++) if (!sta_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6)) { fill_result(out.res, b); return copy_out(rq, &out.res, sizeof out.res); }
    return ENOENT; }
  default: return EOPNOTSUPP;
  }
}

errno_t if_ioctl(ifnet_t, unsigned long cmd, void *data) {
  switch (cmd) {
  case X9_SIOCGA80211: case X9_SIOCSA80211: {
    apple80211req *rq = static_cast<apple80211req *>(data);
    const uint32_t len = rq->req_len; const int32_t val = rq->req_val;
    int e = apple80211(cmd == X9_SIOCSA80211, *rq);
    gRequests++; if (e == EOPNOTSUPP) gRefused++;
    static unsigned shown; const int type = rq->req_type; const bool first = type >= 0 && type < 256 && !(gLogged[type / 32] & (1u << (type % 32)));
    if (type >= 0 && type < 256) gLogged[type / 32] |= 1u << (type % 32);
    if (shown < 300 || first) {
      ++shown; char who[24] = "?"; proc_selfname(who, sizeof who);
      IOLog("X9 WIFI REQ: %u %s[%d] type %d %s len %u val %d -> %d (len %u val %d)\n", shown, who, proc_selfpid(), type,
            cmd == X9_SIOCSA80211 ? "set" : "get", len, val, e, rq->req_len, rq->req_val);
    }
    return e;
  }
  case SIOCGIFMEDIA: {
    struct ifmediareq *m = static_cast<struct ifmediareq *>(data);
    sta_status st; sta_status_get(&st);
    m->ifm_current = IFM_IEEE80211 | IFM_AUTO; m->ifm_mask = 0; m->ifm_status = IFM_AVALID | (st.state == STA_UP ? IFM_ACTIVE : 0);
    m->ifm_active = IFM_IEEE80211 | IFM_AUTO; m->ifm_count = 1;
    return 0;
  }
  case SIOCGIFDEVMTU: {   /* current, min, max */
    struct ifreq *r = static_cast<struct ifreq *>(data);
    r->ifr_devmtu.ifdm_current = 1500; r->ifr_devmtu.ifdm_min = 1280; r->ifr_devmtu.ifdm_max = 1500;
    return 0;
  }
  case SIOCSIFFLAGS: case SIOCSIFMTU: case SIOCADDMULTI: case SIOCDELMULTI: case SIOCSIFADDR: case SIOCSIFMEDIA: return 0;
  default: {
    static unsigned said;
    if (said++ < 16) { char who[24] = "?"; proc_selfname(who, sizeof who); log("interface request %lx from %s not handled", cmd, who); }
    return EOPNOTSUPP; }
  }
}

}  // namespace
}  // namespace x9wifi

/* ---- registry entries: wifid/configd tell a wireless interface by these class names ---- */
class IO80211Controller : public IOService { OSDeclareDefaultStructors(IO80211Controller); };
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
class IONetworkInterface : public IOService { OSDeclareDefaultStructors(IONetworkInterface); };
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
class IO80211Interface : public IONetworkInterface {
  OSDeclareDefaultStructors(IO80211Interface);
 public:
  IOReturn newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE {
    static unsigned said;
    if (said++ < 8) { char who[24] = "?"; proc_selfname(who, sizeof who);
      x9wifi::log("%s asked for a user client of type %u: none offered (IO80211 falls back to the ioctl bind)", who, static_cast<unsigned>(type)); }
    return kIOReturnUnsupported;
  }
};
OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);

namespace x9wifi { namespace {

IOService *publish(IOService *service, IOService *provider, const char *name) {
  if (!service || !service->attach(provider)) { OSSafeReleaseNULL(service); return nullptr; }
  service->setName(name);
  if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
  service->registerService();
  return service;
}
void publish_registry(const char *ifname) {
  IOService *platform = IOService::getPlatform();
  IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
  IO80211Interface *wlan = OSTypeAlloc(IO80211Interface);
  if (!platform || !controller || !wlan || !controller->init() || !wlan->init()) { OSSafeReleaseNULL(controller); OSSafeReleaseNULL(wlan); log("registry objects not made"); return; }
  if (OSData *mac = OSData::withBytes(gMac, 6)) { controller->setProperty("IOMACAddress", mac); mac->release(); }
  controller->setProperty("IOBuiltin", true);
  controller = static_cast<IO80211Controller *>(publish(controller, platform, "X9WLANController"));
  if (!controller) { wlan->release(); log("the controller did not start"); return; }
  wlan->setProperty("IOInterfaceName", ifname); wlan->setProperty("BSD Name", ifname); wlan->setProperty("IOInterfaceNamePrefix", "en");
  if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) { wlan->setProperty("IOInterfaceType", n); n->release(); }
  if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) { wlan->setProperty("IOInterfaceUnit", n); n->release(); }
  wlan->setProperty("IOBuiltin", true); wlan->setProperty("IOPrimaryInterface", true); wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
  if (!publish(wlan, controller, "X9WLAN")) log("the interface service did not start"); else wlan->release();
  controller->release();
}

/* every 10 s: requests seen, address, packet counts */
void stats_thread(void *, wait_result_t) {
  for (unsigned round = 1; round <= 360; ++round) {
    IOSleep(10000);
    char text[32] = "none"; ifaddr_t *list = nullptr;
    if (gIf && !ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
      if (list[0]) { struct sockaddr_in a; memset(&a, 0, sizeof a);
        if (!ifaddr_address(list[0], reinterpret_cast<struct sockaddr *>(&a), sizeof a)) { const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr); snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]); } }
      ifnet_free_address_list(list); }
    sta_status st; sta_status_get(&st);
    IOLog("X9 WIFI STATS: requests=%u refused=%u state=%u scans=%u joins=%u tx=%u/%u rx=%u rx_err=%u en0=%s\n",
          gRequests, gRefused, st.state, fake::stats.scans, fake::stats.joins, fake::stats.tx_frames, fake::stats.tx_bytes, gRxFrames, gRxErrors, text);
  }
  thread_terminate(current_thread());
}

bool gStarted;
void start(void) {
  if (gStarted) return;
  gStarted = true;
  static const uint8_t mac[6] = { 0x02, 0x58, 0x39, 0x57, 0x46, 0x01 };   /* locally administered, fake radio */
  memcpy(gMac, mac, 6);
  if (sta_start(gMac)) { log("station backend did not start"); return; }
  struct ifnet_init_eparams p; memset(&p, 0, sizeof p);
  p.ver = IFNET_INIT_CURRENT_VERSION; p.len = sizeof p; p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO; p.name = "en"; p.unit = 0; p.family = IFNET_FAMILY_ETHERNET; p.subfamily = IFNET_SUBFAMILY_WIFI; p.type = IFT_ETHER;
  p.output = if_output; p.demux = ether_demux; p.add_proto = ether_add_proto; p.del_proto = ether_del_proto; p.check_multi = ether_check_multi; p.framer = ether_frameout;
  p.ioctl = if_ioctl; p.event = if_event_stub; static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; p.broadcast_addr = bcast; p.broadcast_len = 6;
  ifnet_t ifp = nullptr; errno_t e = ifnet_allocate_extended(&p, &ifp);
  if (e || !ifp) { log("ifnet allocation failed: %d", e); return; }
  ifnet_set_mtu(ifp, 1500); ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff); ifnet_set_baudrate(ifp, 54000000ULL);
  ifnet_set_hdrlen(ifp, 14); ifnet_set_addrlen(ifp, 6); ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
  e = ifnet_attach(ifp, nullptr);
  if (e) { log("ifnet attach failed: %d", e); ifnet_release(ifp); return; }
  ifnet_set_lladdr(ifp, gMac, 6);
  e = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr); if (e) log("packet filter attach failed: %d", e);
  gIf = ifp; ifnet_set_flags(ifp, IFF_UP, IFF_UP);
  sta_set_events(&gEvents);
  publish_registry("en0");
  thread_t t = nullptr; if (kernel_thread_start(stats_thread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  log("en0 is up (FAKE radio), hardware address %02x:%02x:%02x:%02x:%02x:%02x", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
}
thread_call_t gStartCall;
void start_fired(thread_call_param_t, thread_call_param_t) { start(); }

}  // namespace
}  // namespace x9wifi

/* Called when configd first opens IONetworkStack: BSD networking is up by then. */
static void
x9_wifi_maybe_start(void)
{
    if (sweet_x9_arg("x9_wifi_fake", 0) != 1) {
        return;
    }
    if (!x9wifi::gStartCall) {
        x9wifi::gStartCall = thread_call_allocate(x9wifi::start_fired, nullptr);
    }
    if (x9wifi::gStartCall) {
        thread_call_enter(x9wifi::gStartCall);   /* never inside configd's open */
    }
}

/*
 * B1/B2 round 1 (x9_wifi_probe=1): READ-ONLY state of the MT6653 power pins and
 * of PCIe0, from MT6993 sources (mt6993.dts, pinctrl-mt6993.c, clk-mt6993-peri.c,
 * clk-mt6993-pd.c, conninfra connv3/platform/mt6993):
 *   GPIO block 0x1002d000: DIR 0x0/0x10..., DO 0x100+, DI 0x200+ (1 bit/pin,
 *   32 pins per word), MODE 0x300 + (pin/4)*0x10, 8-bit stride, 5-bit field.
 *   Pins: 65 PMIC faultb, 243/244 combo UART, 248 POR reset, 250 WF_RST,
 *   251 PMIC_EN (MT6376 enable; connsys PMIC on when high).
 *   pextp0cfg_ao 0x169b0000: CG status +0x14 (1 = gated): MAC0_TL 0, MAC0_REF 1,
 *   PHY0_MCU_BUS 6, PHY0_REF 7, MAC0_AXI_250 12, MAC0_AHB_APB 13; reset +0x8.
 *   ap_pd 0x1c040000 +0x12900 (1 = powered): PEXTP_MAC0 bit 16, PEXTP_PHY0 bit 17.
 * The PCIe0 MAC itself (0x16910000: RST_CTRL 0x148, LTSSM 0x150, LINK 0x154,
 * per pcie-mediatek-gen3.c) is read only when its power domain and all MAC
 * clocks are on: an unclocked MediaTek block can hang the bus. Nothing written.
 */
static void
x9_wifi_probe_hw(void)
{
    if (sweet_x9_arg("x9_wifi_probe", 0) != 1) {
        return;
    }
    volatile uint32_t *gpio = (volatile uint32_t *)ml_io_map(0x1002d000, 0x1000);
    volatile uint32_t *cg = (volatile uint32_t *)ml_io_map(0x169b0000, 0x1000);
    volatile uint32_t *pd = (volatile uint32_t *)ml_io_map(0x1c052000, 0x1000);   /* 0x1c040000 + 0x12900 page */
    if (!gpio || !cg || !pd) {
        IOLog("X9 WIFI PROBE: map failed gpio=%p cg=%p pd=%p\n", gpio, cg, pd);
        return;
    }
    static const struct { unsigned pin; const char *what; } pins[] = {
        { 65, "PMIC faultb" }, { 243, "combo UTXD" }, { 244, "combo URXD" },
        { 248, "POR reset" }, { 250, "WF_RST" }, { 251, "PMIC_EN" },
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        unsigned pin = pins[i].pin, word = (pin / 32) * 0x10, bit = pin % 32;
        uint32_t dir = gpio[word / 4], dout = gpio[(0x100 + word) / 4], din = gpio[(0x200 + word) / 4];
        uint32_t mode = (gpio[(0x300 + (pin / 4) * 0x10) / 4] >> ((pin % 4) * 8)) & 0x1f;
        IOLog("X9 WIFI PROBE: GPIO%u %-12s mode=%u dir=%s out=%u in=%u\n", pin, pins[i].what, mode,
            (dir >> bit) & 1 ? "out" : "in", (dout >> bit) & 1, (din >> bit) & 1);
    }
    uint32_t cgsta = cg[0x14 / 4], rst = cg[0x8 / 4], pdsta = pd[0x900 / 4];
    bool macPower = (pdsta >> 16) & 1, phyPower = (pdsta >> 17) & 1;
    const uint32_t macClocks = (1u << 0) | (1u << 1) | (1u << 12) | (1u << 13);
    IOLog("X9 WIFI PROBE: pextp0 cg_sta=%08x (gated: tl=%u ref=%u phy_bus=%u phy_ref=%u axi=%u ahb=%u) rst=%08x pd=%08x mac_power=%d phy_power=%d\n",
        cgsta, cgsta & 1, (cgsta >> 1) & 1, (cgsta >> 6) & 1, (cgsta >> 7) & 1, (cgsta >> 12) & 1, (cgsta >> 13) & 1,
        rst, pdsta, macPower, phyPower);
    if (macPower && !(cgsta & macClocks)) {
        volatile uint32_t *mac = (volatile uint32_t *)ml_io_map(0x16910000, 0x1000);
        if (mac) {
            IOLog("X9 WIFI PROBE: pcie0 MAC rst_ctrl=%08x ltssm=%08x link=%08x (link up = bit 8)\n",
                mac[0x148 / 4], mac[0x150 / 4], mac[0x154 / 4]);
        }
    } else {
        IOLog("X9 WIFI PROBE: pcie0 MAC not read (power domain or clocks off)\n");
    }
}

/*
 * B1 round 1 (x9_wifi_power=1): the FIRST hardware write of the Wi-Fi bring-up,
 * user-approved 2026-11-04. Only GPIO250 (WF_RST) and GPIO251 (PMIC_EN) are
 * changed, by read-modify-write of their own mode/dir/out bits, in the
 * connv3/gen4m order (connv3_core.c opfunc_power_on_internal):
 *   PRE_PWR_ON: wf_rst_on (GPIO250 out high; gen4m plat_priv.c, >= 200 ms
 *   since a previous toggle) -> connv3_hw_pwr_on: mdelay(30), pmic-en-set
 *   (GPIO251 out high), mdelay(20) -> gen4m connsys_power_on: mdelay(10).
 * Faultb (GPIO65) pull-up and the combo-UART pull-ups are NOT programmed this
 * round. Then about 3 s of observation (GPIO65 level, PCIe0 LTSSM/link; PCIe0
 * is powered and clocked per the 31.txt probe) and power-off in reverse
 * (connv3: mdelay(20) before pmic_en low; wf_rst_off), back to LK's state.
 * Runs once from a thread 20 s after IONetworkStack starts.
 */
static volatile uint32_t *x9_gpio;
static void
x9_gpio_rmw(unsigned off, uint32_t mask, uint32_t value)
{
    uint32_t v = x9_gpio[off / 4];
    x9_gpio[off / 4] = (v & ~mask) | (value & mask);
    __asm__ volatile ("dsb sy" ::: "memory");
}
static void
x9_gpio_out(unsigned pin, unsigned level)
{
    unsigned word = (pin / 32) * 0x10, bit = 1u << (pin % 32);
    unsigned mode = 0x300 + (pin / 4) * 0x10, shift = (pin % 4) * 8;
    x9_gpio_rmw(mode, 0x1fu << shift, 0);              /* function 0 = GPIO */
    x9_gpio_rmw(0x100 + word, bit, level ? bit : 0);   /* DO first, then DIR: no glitch */
    x9_gpio_rmw(word, bit, bit);                       /* DIR = out */
}
static unsigned
x9_gpio_in(unsigned pin)
{
    return (x9_gpio[(0x200 + (pin / 32) * 0x10) / 4] >> (pin % 32)) & 1;
}
/*
 * B1/B2 round 2 (x9_wifi_power=2), 45.txt follow-up. Round 1 proved the two
 * GPIO writes land (GPIO250/251 read back 1) but faultb stayed 0 for 3 s and
 * LTSSM/link stayed 0. faultb was never pulled up, and connv3 does that itself
 * right after pmic-en-set (mt6993_pmic.c connv3_plt_pmic_common_power_ctrl_mt6993
 * -> "connsys-pin-pmic-faultb-enable" = GPIO65 GPIO mode, input-enable,
 * bias-pull-up R1R0_10; mt6993.dts), so round 1's 0 says nothing. Round 2 adds,
 * in the Linux order:
 *  1. faultb-enable: iocfg_lm0 0x13020000 (pio reg index 11) IES +0x50, PUPD
 *     +0x90 (0 = pull-up), R0 +0xb0 = 0, R1 +0xc0 = 1, all bit 1
 *     (pinctrl-mt6993.c, pinctrl-mtk-common-v2.c bias_set_pupd_r1_r0).
 *  2. 10 ms "pcie phy ready" (cmm_asic_connac3x.c), then the PCIe0 RC start of
 *     pcie-mediatek-gen3.c mtk_pcie_startup_port for mt6993 port 0, reduced to
 *     the link-relevant steps: RC mode (0x80 bit0), 26M request force-on and
 *     the BBCK1 PMRC bypass (pextp0cfg 0x169b0000 +0x7c bits 0/4, clkbuf
 *     force_26m + pre_init_6993), LTSSM-disable bit (0x84 bit2) cleared if
 *     set, assert MAC/PHY/BRG/PE resets (0x148), 50 ms, release MAC/PHY/BRG,
 *     50 ms, release PERST, then poll link (0x154 bit 8) for 500 ms logging
 *     every LTSSM (0x150 bits 28:24) change.
 *  3. Only when the link is up: type-0 config read of bus 1 dev 0 through
 *     CFGNUM 0x140 + window 0x1000 (vendor/device, class, BAR0). No config
 *     access while the link is down (completion timeouts could raise SError).
 *  4. Undo: PERST asserted again, 0x7c restored, faultb back to its default
 *     pull-down state, PMIC_EN low, 200 ms, WF_RST low.
 * No PMIC register, clock-buffer (MT6687) or PHY calibration write; the PHY
 * registers post_init_6993 touches (0x16900000 +0x3064/+0x5030/+0x4c) are only
 * read and logged.
 */
#define X9_IOCFG_LM0 0x13020000u
static void
x9_iocfg_bit(volatile uint32_t *io, unsigned off, unsigned bit, unsigned v)
{
    uint32_t r = io[off / 4];
    io[off / 4] = v ? (r | (1u << bit)) : (r & ~(1u << bit));
    __asm__ volatile ("dsb sy" ::: "memory");
}
static void
x9_faultb_bias(volatile uint32_t *io, bool pullup)
{
    /* GPIO65: iocfg_lm0 bit 1 in IES/PUPD/R0/R1; R1R0 = 10 in both DT states */
    x9_iocfg_bit(io, 0x50, 1, 1);
    x9_iocfg_bit(io, 0x90, 1, pullup ? 0 : 1);
    x9_iocfg_bit(io, 0xb0, 1, 0);
    x9_iocfg_bit(io, 0xc0, 1, 1);
}
static uint64_t
x9_ms_since(uint64_t t0)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    return ns / 1000000;
}
/* Config access through the gen3 window (pcie-mediatek-gen3.c mtk_pcie_config_tlp_header
 * + mtk_pcie_map_bus): CFGNUM 0x140 = force-BE | BE | bus<<8 | devfn, data at 0x1000+where. */
static uint32_t
x9_pcie_cfg_rd(volatile uint32_t *mac, unsigned bus, unsigned devfn, unsigned where)
{
    mac[0x140 / 4] = (1u << 20) | (0xfu << 16) | (bus << 8) | devfn;
    __asm__ volatile ("dsb sy" ::: "memory");
    return mac[(0x1000 + (where & ~3u)) / 4];
}
static void
x9_pcie_cfg_wr(volatile uint32_t *mac, unsigned bus, unsigned devfn, unsigned where, uint32_t v)
{
    mac[0x140 / 4] = (1u << 20) | (0xfu << 16) | (bus << 8) | devfn;
    __asm__ volatile ("dsb sy" ::: "memory");
    mac[(0x1000 + (where & ~3u)) / 4] = v;
    __asm__ volatile ("dsb sy" ::: "memory");
}
/*
 * Stage 6 (49.txt follow-up: ROM idle 0x1D1E, driver own taken in 5 ms): the
 * first DMA. Downloads the ROM patch through WFDMA host DMA0, polled, no MSI.
 * Firmware: x9/tools/wifi_firmware.py embeds the vendor files from
 * .local/x9/wifi-fw at build time (generated/x9_wifi_firmware.inc, not in git).
 * Sources (gen4m): chips/mt6653/mt6653.c (rings, mt6653WfdmaManualPrefetch,
 * mt6653WfdmaControl, ...ConfigExt1/2), chips/common/cmm_asic_connac3x.c
 * (asicConnac3xFillInitCmdTxdInfo, u2TxInitCmdPort = TX_RING_CMD = HW ring 15,
 * u2TxFwDlPort = TX_RING_FWDL = HW ring 16), hal_pdma.c (descriptors), wlan_oid.c
 * (wlanSendInitSetQueryCmdImpl/wlanWaitInitEvt: event = 32-byte RXD + INIT_WIFI_EVENT
 * on RX_RING_EVT = HW ring 7), chips/common/fw_dl.c (wlanGetPatchInfoAndDownloadV2).
 * Kbuild.main sets CFG_ROM_PATCH_NO_SEM_CTRL=1 for MT6653 with WMT, so the
 * Android driver sends no PATCH_SEMAPHORE_CONTROL; neither do we.
 *  - DMA memory: one 64 KiB physically contiguous, cacheable buffer (DRAM starts
 *    at 0x140000000, so 36-bit addressing: GLO_CFG ADDR_EXT_EN, ring CTRL1 bits
 *    16-19, TX desc word 3, RX desc word 2); flush_dcache64 by PA around every
 *    device access (the PCIe RC is not dma-coherent in mt6993.dts).
 *  - Rings: TX15 (commands), TX16 (firmware data), RX7 (events), 16 entries.
 *  - Command = 32-byte CONNAC3X TXD (DW0 length | PKT_FT_CMD << 23, DW1 HF=command
 *    << 14) + INIT_HIF_TX_HEADER (u16 length-32, u16 PQ 0, CID, 0xA0, 0, seq, 28
 *    reserved bytes) + payload. Firmware data on TX16 is raw, 2048 B per packet.
 *  - Patch (V2, 2 sections, sec_info 0x01000000 = AES key 0): per section
 *    PATCH_START (CID 5, {addr, len, mode}, mode = ACK|ENC|RESET = 0x80000009)
 *    -> CMD_RESULT (EID 1) status 0 -> data -> after all sections PATCH_FINISH
 *    (CID 7, {0}) -> CMD_RESULT status 0.
 *  - Afterwards WFDMA is stopped and the rings cleared; the caller hands the
 *    MCU back and powers the chip off as before. The buffer is never freed.
 */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "../../../generated/x9_wifi_firmware.inc"
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
/*
 * 60.txt follow-up: the TX15 descriptor read reaches the RC AXI master and never completes. Android does not
 * DMA from ordinary kernel memory on mt6993: gen4m (CFG_SUPPORT_WIFI_RSV_MEM, hif_mem.c halAllocHifMem)
 * places every TX/RX descriptor ring, command and FWDL buffer in the "wifi" node's memory-region, which LK
 * carves out as mblock-55 shared-dma-pool_wifi-reserve-memory_dma = 0xe2700000 + 0x1900000 ("MPU-in-lk":
 * mt6993.dts connsys-emi-config emi-mpu-region <6 11 47> covers layout idx 11 "WFDMA1"; plat_priv.c
 * kalSetDrvEmiMpuProtection opens such regions only to DOMAIN_AP + DOMAIN_CONN). Same address in 50..60.txt.
 * Upstream thread with the same idea for mediatek-gen3 (no IOMMU, firmware MPU limits PCIe DMA to a carve-out):
 * patchew 20260508063633.3894348-1-wenst@chromium.org. XNU RAM starts at 0x140000000, so this pool is not
 * XNU memory; it is mapped write-combine (Normal NC) and needs no cache maintenance.
 */
#define X9_WIFI_RSV_PA 0xe2700000ULL
#define X9_WIFI_RSV_SIZE 0x1900000ULL

#define X9_WFDMA 0xd4000u                    /* WFDMA host DMA0 (chip 0x20024000) in BAR0 */
#define X9_DMA_SIZE 0x10000u
#define X9_RING_N 16u
#define X9_D_TX15 0x0000u                    /* descriptor rings, 16 x 16 B */
#define X9_D_TX16 0x0100u
#define X9_D_RX7 0x0200u
#define X9_B_CMD 0x1000u                     /* command buffer */
#define X9_B_FW 0x2000u                      /* firmware data buffer */
#define X9_B_RX 0x8000u                      /* RX buffers, 16 x 2 KiB */
#define X9_BUF 0x800u
/* RC (pcie0 MAC) and pextp0cfg_ao maps, set by x9_wifi_pcie_try for the DMA diagnostics */
static volatile uint32_t *x9_rc_mac, *x9_rc_pextp, *x9_ep_bar;
static void
x9_wifi_rc_diag(const char *when)
{
    volatile uint32_t *m = x9_rc_mac, *p = x9_rc_pextp, *b = x9_ep_bar;
    if (!m || !p) {
        return;
    }
    if (b) {
        IOLog("X9 WIFI CHIP %s: cb slp_prot hw=%08x sw=%08x slp_status=%08x pcie_slp_cfg0=%08x cfg1=%08x access_detect=%08x ep_axi_if=%08x lpctl=%08x conn_on_misc=%08x\n",
            when, b[0x1f5210 / 4], b[0x1f5214 / 4], b[0x1f500c / 4], b[0x1f5014 / 4], b[0x1f5018 / 4],
            b[0xe0128 / 4], b[0x1d01a8 / 4], b[0xe0010 / 4], b[0xe00f0 / 4]);
        /* WFDMA wrapper (0x20027000 -> bar 0xd7000) and host DMA0 debug probe, as
         * dbg_mt6653.c mt6653_show_wfdma_wrapper_info / _dbg_probe_info read them */
        /*
         * 57.txt follow-up (read-only): the MT6653's own PCIe MAC (PCIE_MAC_IREG chip 0x74030000 = bar 0x1d0000).
         * AXI slave port config/status and the outbound ATR tables ATR_AXI_SLV0-3 (0x800 + 0x100*slave, 8 x 0x20):
         * +0 SRC_ADDR_LSB[31:12]|ATR_SIZE[6:1]|IMPL[0], +4 SRC_MSB, +8 TRSL_LSB, +c TRSL_MSB, +10 TRSL_PARAM
         * (TRSF_PARAM[27:16] TRSL_ID[3:0]), +18/+1c TRSL_MASK. Only non-empty entries are printed.
         */
        {
            volatile uint32_t *pm = b + 0x1d0000 / 4;
            IOLog("X9 WIFI EPMAC %s: slv_conf=%08x/%08x/%08x/%08x mst_conf=%08x/%08x pend_adt=%08x p_adt_win=%08x/%08x a_adt_slv=%08x/%08x/%08x/%08x istat_dma_pend=%08x if_ctrl=%08x basic_sta=%08x\n",
                when, pm[0x34 / 4], pm[0x3c / 4], pm[0x44 / 4], pm[0x4c / 4], pm[0x30 / 4], pm[0x38 / 4], pm[0x1d4 / 4],
                pm[0x1d8 / 4], pm[0x1dc / 4], pm[0x1e0 / 4], pm[0x1e4 / 4], pm[0x1e8 / 4], pm[0x1ec / 4], pm[0x1d0 / 4],
                pm[0x1a8 / 4], pm[0x18 / 4]);
            unsigned shown = 0;
            for (unsigned sl = 0; sl < 4; sl++) {
                for (unsigned t = 0; t < 8; t++) {
                    volatile uint32_t *e = pm + (0x800 + sl * 0x100 + t * 0x20) / 4;
                    uint32_t w0 = e[0], w1 = e[1], w2 = e[2], w3 = e[3], w4 = e[4], w6 = e[6], w7 = e[7];
                    if (!(w0 | w1 | w2 | w3 | w4 | w6 | w7)) {
                        continue;
                    }
                    shown++;
                    uint64_t src = ((uint64_t)w1 << 32) | (w0 & 0xfffff000u), trsl = ((uint64_t)w3 << 32) | (w2 & 0xfffff000u);
                    IOLog("X9 WIFI ATR %s: slv%u t%u src=%llx size=2^%u impl=%u trsl=%llx param=%08x (trsf=%03x id=%u) mask=%08x/%08x\n",
                        when, sl, t, src, ((w0 >> 1) & 0x3f) + 1, w0 & 1, trsl, w4, (w4 >> 16) & 0xfff, w4 & 0xf, w7, w6);
                }
            }
            if (!shown) {
                IOLog("X9 WIFI ATR %s: all ATR_AXI_SLV0-3 entries are zero\n", when);
            }
        }
        /* AXI0 R2A bridge (wrapper 0x500-0x51c): STS b11 RRESP_ERR, b10:3 RRESP_ERR_STS, b0 RD_AXI_ERR_DET_INT */
        IOLog("X9 WIFI R2A %s: ctrl0=%08x ctrl1=%08x ctrl2=%08x sts=%08x dmawr_probe=%08x dmard_probe=%08x wr_dbg0=%08x wr_dbg1=%08x txfifo_dbg0=%08x txfifo_dbg1=%08x\n",
            when, b[0xd7500 / 4], b[0xd7504 / 4], b[0xd7508 / 4], b[0xd750c / 4], b[0xd7510 / 4], b[0xd7514 / 4],
            b[0xd7518 / 4], b[0xd751c / 4], b[0xd706c / 4], b[0xd7070 / 4]);
        IOLog("X9 WIFI WFDMA %s: hif_misc=%08x axi_slpprot=%08x slpprot0=%08x slpprot1=%08x msi_cfg=%08x\n",
            when, b[0xd7044 / 4], b[0xd7050 / 4], b[0xd7078 / 4], b[0xd707c / 4], b[0xd702c / 4]);
        static const uint8_t dbg[] = { 0x0, 0x1, 0x2, 0x3, 0x30, 0x5, 0x7, 0xa, 0xb, 0xc, 0x62 };
        uint32_t pr[sizeof dbg];
        uint32_t idx0 = b[0xd4124 / 4];
        for (unsigned k = 0; k < sizeof dbg; k++) {
            b[0xd4124 / 4] = 0x100u + dbg[k];
            __asm__ volatile ("dsb sy" ::: "memory");
            pr[k] = b[0xd4128 / 4];
        }
        b[0xd4124 / 4] = idx0;
        IOLog("X9 WIFI WFDMA %s: probe 00=%08x 01=%08x 02=%08x 03=%08x 30=%08x 05=%08x 07=%08x 0a=%08x 0b=%08x 0c=%08x 62=%08x\n",
            when, pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], pr[6], pr[7], pr[8], pr[9], pr[10]);
    }
    IOLog("X9 WIFI RC %s: pextp clk_con=%08x res_req=%08x pwrctl0=%08x pwrctl6=%08x pwrctl8=%08x req_ctrl=%08x slpprot_rdy=%08x\n",
        when, p[0x20 / 4], p[0x34 / 4], p[0x40 / 4], p[0x58 / 4], p[0x74 / 4], p[0x7c / 4], p[0x264 / 4]);
    IOLog("X9 WIFI RC %s: int_sta=%08x axi_if=%08x misc=%08x res_sta=%08x res_ctrl=%08x axi0_err_addr=%08x axi0_err_info=%08x wcpl=%08x spc2=%08x ltssm=%08x link=%08x\n",
        when, m[0x184 / 4], m[0x1a8 / 4], m[0x348 / 4], m[0xd28 / 4], m[0xd2c / 4], m[0xe00 / 4], m[0xe08 / 4],
        m[0x340 / 4], m[0xd8 / 4], m[0x150 / 4], m[0x154 / 4]);
    {
        unsigned ec = 0x100, aer = 0;
        for (unsigned n = 0; ec >= 0x100 && ec < 0x1000 && n < 32; n++) {
            uint32_t h = x9_pcie_cfg_rd(m, 1, 0, ec);
            if (h == 0 || h == 0xffffffffu) break;
            if ((h & 0xffff) == 0x0001) { aer = ec; break; }
            ec = (h >> 20) & 0xffc;
        }
        IOLog("X9 WIFI AER %s: rc unc_sta=%08x cor_sta=%08x rp_sts=%08x pref=%08x/%08x/%08x | ep aer@%03x unc_sta=%08x unc_mask=%08x cor_sta=%08x hdr=%08x %08x %08x %08x\n",
            when, m[0x204 / 4], m[0x210 / 4], m[0x230 / 4], x9_pcie_cfg_rd(m, 0, 0, 0x24), x9_pcie_cfg_rd(m, 0, 0, 0x28),
            x9_pcie_cfg_rd(m, 0, 0, 0x2c), aer, aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x04) : 0,
            aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x08) : 0, aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x10) : 0,
            aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x1c) : 0, aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x20) : 0,
            aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x24) : 0, aer ? x9_pcie_cfg_rd(m, 1, 0, aer + 0x28) : 0);
    }
    /*
     * 59.txt follow-up (read-only): both bridges are PLDA. Did the EP's read reach the RC? RC inbound ATR
     * (ATR_PCIE_WIN0/1 at 0x600/0x700, 8 x 0x20; HW default, Linux never writes them), RC ISTATUS_LOCAL 0x184
     * (P_ATR b23:20, A_ATR b19:16: bit order POST/FETCH/DISCARD/DOORBELL), PENDING_ADT 0x1d4, P_ADT_WIN0/1
     * 0x1d8/0x1dc, A_ADT_SLV0 0x1e0, ERR_ADDR_L/H/INFO 0xe40-0xe48, TX credits 0xd30/0xd34. EP: ISTATUS_LOCAL
     * (chip MAC 0x184), DEVCAP2/DEVCTL2 (PCIe cap 0x80 + 0x24/0x28: completion timeout ranges / disable b4).
     */
    {
        IOLog("X9 WIFI RCIN %s: istat_local=%08x pend_adt=%08x p_adt_win=%08x/%08x a_adt_slv0=%08x err_addr=%08x%08x err_info=%08x tx_credit=%08x/%08x ep istat_local=%08x devcap2=%08x devctl2/sta2=%08x\n",
            when, m[0x184 / 4], m[0x1d4 / 4], m[0x1d8 / 4], m[0x1dc / 4], m[0x1e0 / 4], m[0xe44 / 4], m[0xe40 / 4],
            m[0xe48 / 4], m[0xd30 / 4], m[0xd34 / 4], x9_ep_bar ? x9_ep_bar[0x1d0184 / 4] : 0,
            x9_pcie_cfg_rd(m, 1, 0, 0x80 + 0x24), x9_pcie_cfg_rd(m, 1, 0, 0x80 + 0x28));
        unsigned shown = 0;
        for (unsigned w = 0; w < 2; w++) {
            for (unsigned t = 0; t < 8; t++) {
                volatile uint32_t *e = m + (0x600 + w * 0x100 + t * 0x20) / 4;
                uint32_t w0 = e[0], w1 = e[1], w2 = e[2], w3 = e[3], w4 = e[4];
                if (!(w0 & 1) && !(w2 | w3 | w4)) {
                    continue;
                }
                shown++;
                IOLog("X9 WIFI RCIN %s: win%u t%u src=%llx size=2^%u impl=%u trsl=%llx param=%08x (id=%u)\n", when, w, t,
                    ((uint64_t)w1 << 32) | (w0 & 0xfffff000u), ((w0 >> 1) & 0x3f) + 1, w0 & 1,
                    ((uint64_t)w3 << 32) | (w2 & 0xfffff000u), w4, w4 & 0xf);
            }
        }
        if (!shown) {
            IOLog("X9 WIFI RCIN %s: no enabled RC inbound ATR entry (win0/1 t0-t7 all IMPL=0)\n", when);
        }
    }
    IOLog("X9 WIFI RC %s: rc cmd/sta=%08x sec_sta=%08x ep cmd/sta=%08x ep devctl/sta=%08x\n", when,
        x9_pcie_cfg_rd(m, 0, 0, 0x04), x9_pcie_cfg_rd(m, 0, 0, 0x1c), x9_pcie_cfg_rd(m, 1, 0, 0x04),
        x9_pcie_cfg_rd(m, 1, 0, 0x80 + 0x08));
}
/* 58.txt follow-up (read-only): when does the chip PCIe MAC's pending outbound read (PENDING_ADT R b27:16) appear? */
static void
x9_wifi_pend(volatile uint32_t *b, volatile uint32_t *mac, const char *when)
{
    volatile uint32_t *pm = b + 0x1d0000 / 4;
    uint32_t pend = pm[0x1d4 / 4];
    IOLog("X9 WIFI PEND %s: pend_adt=%08x (R=%u W=%u) a_adt_slv0=%08x p_adt_win0=%08x dma_pend=%08x r2a_rd_probe=%08x r2a_sts=%08x ep cmd/sta=%08x devsta=%04x rc int_sta=%08x\n",
        when, pend, (pend >> 16) & 0xfff, pend & 0xfff, pm[0x1e0 / 4], pm[0x1d8 / 4], pm[0x1d0 / 4], b[0xd7514 / 4],
        b[0xd750c / 4], x9_pcie_cfg_rd(mac, 1, 0, 0x04), x9_pcie_cfg_rd(mac, 1, 0, 0x80 + 0x08) >> 16, mac[0x184 / 4]);
}
struct x9_wdma {
    volatile uint32_t *bar;
    uint8_t *va;
    uint64_t pa;
    bool nc;                                 /* LK Wi-Fi pool mapped Normal non-cacheable: no cache maintenance */
    unsigned c15, c16, r7;                   /* next TX CPU index; next RX index to read */
    uint8_t seq;
    unsigned events;
};
static void
x9_wd_wr(x9_wdma *d, unsigned off, uint32_t v)
{
    d->bar[(X9_WFDMA + off) / 4] = v;
    __asm__ volatile ("dsb sy" ::: "memory");
}
static uint32_t
x9_wd_rd(x9_wdma *d, unsigned off)
{
    return d->bar[(X9_WFDMA + off) / 4];
}
static void
x9_wd_sync(x9_wdma *d, unsigned off, unsigned len)
{
    __asm__ volatile ("dsb sy" ::: "memory");
    if (!d->nc) {
        flush_dcache64((addr64_t)(d->pa + off), len, 1);
        __asm__ volatile ("dsb sy" ::: "memory");
    }
}
/* 53.txt follow-up (read-only): CONN_HIF_RST and DMASHDL (chip 0x20026000 -> bar 0xd6000) state */
static void
x9_wd_state(x9_wdma *d, const char *when)
{
    volatile uint32_t *s = d->bar + 0xd6000 / 4;
    IOLog("X9 WIFI DMASHDL %s: hif_rst=%08x sw_ctrl=%08x opt=%08x page=%08x refill=%08x ctrl_sig=%08x pkt_max=%08x grp0=%08x grp1=%08x qmap0=%08x qmap1=%08x qmap2=%08x qmap3=%08x dbg0=%08x dbg1=%08x rd02=%08x\n",
        when, x9_wd_rd(d, 0x100), s[0x04 / 4], s[0x08 / 4], s[0x0c / 4], s[0x10 / 4], s[0x18 / 4], s[0x1c / 4],
        s[0x20 / 4], s[0x24 / 4], s[0x60 / 4], s[0x64 / 4], s[0x68 / 4], s[0x6c / 4], s[0xc0 / 4], s[0xc4 / 4], s[0xc8 / 4]);
}
static void
x9_wd_rx_desc(x9_wdma *d, unsigned i)
{
    volatile uint32_t *r = (volatile uint32_t *)(void *)(d->va + X9_D_RX7 + i * 16);
    uint64_t b = d->pa + X9_B_RX + i * X9_BUF;
    r[0] = (uint32_t)b;
    r[1] = X9_BUF << 16;                     /* SDLen0 = buffer, DMADONE = 0 */
    r[2] = (uint32_t)(b >> 32) & 0xf;        /* RX: SDPtr1 carries address bits 32..35 */
    r[3] = 0;
}
/* Queue one buffer on TX ring 15 or 16 and wait until DIDX passes it. */
static bool
x9_wd_tx(x9_wdma *d, unsigned ring, unsigned boff, unsigned len)
{
    unsigned *cidx = ring == 15 ? &d->c15 : &d->c16;
    unsigned doff = (ring == 15 ? X9_D_TX15 : X9_D_TX16) + *cidx * 16;
    volatile uint32_t *t = (volatile uint32_t *)(void *)(d->va + doff);
    uint64_t b = d->pa + boff;
    t[0] = (uint32_t)b;
    t[1] = ((len & 0x3fffu) << 16) | (1u << 30);  /* SDLen0, LastSec0, DMADONE = 0 */
    t[2] = 0;
    t[3] = (uint32_t)(b >> 32) & 0xf;             /* SDPtr0Ext */
    x9_wd_sync(d, boff, len);
    x9_wd_sync(d, doff, 16);
    *cidx = (*cidx + 1) % X9_RING_N;
    x9_wd_wr(d, 0x308 + ring * 0x10, *cidx);
    uint64_t t0 = mach_absolute_time();
    uint32_t didx;
    while ((didx = x9_wd_rd(d, 0x30c + ring * 0x10)) != *cidx) {
        if (x9_ms_since(t0) > 200) {
            IOLog("X9 WIFI DMA: TX%u stuck cidx=%u didx=%u ctrl0=%08x ctrl1=%08x glo=%08x int_sta=%08x\n",
                ring, *cidx, didx, x9_wd_rd(d, 0x300 + ring * 0x10), x9_wd_rd(d, 0x304 + ring * 0x10),
                x9_wd_rd(d, 0x208), x9_wd_rd(d, 0x200));
            x9_wifi_rc_diag("stuck");
            x9_wd_state(d, "stuck");
            unsigned prev = (*cidx + X9_RING_N - 1) % X9_RING_N;
            unsigned poff = (ring == 15 ? X9_D_TX15 : X9_D_TX16) + prev * 16;
            x9_wd_sync(d, poff & ~63u, 64);
            volatile uint32_t *pd = (volatile uint32_t *)(void *)(d->va + poff);
            x9_wd_sync(d, boff, 64);
            const uint32_t *pb = (const uint32_t *)(const void *)(d->va + boff);
            IOLog("X9 WIFI DMA: TX%u desc[%u] after stall = %08x %08x %08x %08x (DMADONE=%u len=%u last=%u) buf %08x %08x %08x %08x | %08x %08x %08x %08x %08x\n",
                ring, prev, pd[0], pd[1], pd[2], pd[3], pd[1] >> 31, (pd[1] >> 16) & 0x3fff, (pd[1] >> 30) & 1,
                pb[0], pb[1], pb[2], pb[3], pb[8], pb[9], pb[10], pb[11], pb[17]);
            return false;
        }
        IODelay(5);
    }
    return true;
}
/* Wait for the next RX7 event; copies up to cap bytes, returns the length or -1. */
static int
x9_wd_rx(x9_wdma *d, unsigned ms, uint8_t *out, unsigned cap)
{
    unsigned i = d->r7;
    volatile uint32_t *r = (volatile uint32_t *)(void *)(d->va + X9_D_RX7 + i * 16);
    uint64_t t0 = mach_absolute_time();
    for (;;) {
        x9_wd_sync(d, X9_D_RX7 + (i & ~3u) * 16, 64);
        if (r[1] & (1u << 31)) {
            break;
        }
        if (x9_ms_since(t0) > ms) {
            IOLog("X9 WIFI DMA: no event on RX7[%u] after %u ms: cidx=%u didx=%u glo=%08x int_sta=%08x\n",
                i, ms, x9_wd_rd(d, 0x578), x9_wd_rd(d, 0x57c), x9_wd_rd(d, 0x208), x9_wd_rd(d, 0x200));
            x9_wifi_rc_diag("no event");
            return -1;
        }
        IODelay(20);
    }
    unsigned len = (r[1] >> 16) & 0x3fff;
    x9_wd_sync(d, X9_B_RX + i * X9_BUF, X9_BUF);
    const uint8_t *p = d->va + X9_B_RX + i * X9_BUF;
    memcpy(out, p, len < cap ? len : cap);
    d->events++;
    d->r7 = (i + 1) % X9_RING_N;
    if ((i & 3) == 3) {                      /* hand back a whole cache line of descriptors */
        for (unsigned k = i - 3; k <= i; k++) {
            x9_wd_rx_desc(d, k);
        }
        x9_wd_sync(d, X9_D_RX7 + (i - 3) * 16, 64);
        x9_wd_wr(d, 0x578, i);
    }
    return (int)len;
}
/*
 * Send init command cid with payload; when evt != 0 wait for that event and copy
 * its payload (after INIT_WIFI_EVENT's 8 bytes) to res. Returns false on any failure.
 */
static bool
x9_wd_cmd(x9_wdma *d, uint8_t cid, const void *payload, unsigned plen, uint8_t evt, uint8_t *res, unsigned rcap)
{
    /* 32-byte connac3 TXD + 32-byte INIT_HIF_TX_HEADER (u2TxByteCount, u2PQ_ID,
     * INIT_WIFI_CMD {CID, PktTypeID 0xA0, rsv, SeqNum, u4Reserved,
     * au4D3toD7Rev[5]}) -> payload at 64 (gen4m wlanSendInitSetQueryCmdImpl:
     * cmd + sizeof(INIT_HIF_TX_HEADER) + sizeof(..._PENDING_FOR_HW_32BYTES);
     * mt76 mt76_connac2_mcu_txd is 64 bytes too). 63.txt: with 68 the ROM read
     * PATCH_START {addr,len,mode} 4 bytes late and answered status 14, CID 0. */
    uint8_t *c = d->va + X9_B_CMD;
    unsigned len = 64 + plen;
    memset(c, 0, 64);
    uint32_t dw0 = (len & 0xffffu) | (2u << 23), dw1 = 1u << 14;
    memcpy(c + 0, &dw0, 4);
    memcpy(c + 4, &dw1, 4);
    uint16_t hl = (uint16_t)(len - 32);
    memcpy(c + 32, &hl, 2);
    uint8_t seq = ++d->seq;
    c[36] = cid;
    c[37] = 0xa0;
    c[39] = seq;
    memcpy(c + 64, payload, plen);
    if (!x9_wd_tx(d, 15, X9_B_CMD, len)) {
        IOLog("X9 WIFI CMD: cid 0x%02x seq %u not consumed\n", cid, seq);
        return false;
    }
    if (!evt) {
        return true;
    }
    uint8_t ev[96];
    /* gen4m wlanSendInitSetQueryCmd waits CFG_DEFAULT_RX_RESPONSE_TIMEOUT (3-10 s).
     * 67.txt: PATCH_FINISH got no event within our former 1 s while the ROM
     * applies the patch; give it 10 s, every other init command 3 s. */
    unsigned wait_ms = cid == 0x07 ? 10000 : 3000;
    uint64_t tw = mach_absolute_time();
    int n = x9_wd_rx(d, wait_ms, ev, sizeof ev);
    if (n < 0) {
        IOLog("X9 WIFI CMD: cid 0x%02x seq %u: no event in %u ms (rom_sram=%08x romcode_index=%08x)\n", cid, seq, wait_ms,
            d->bar[0x1ab160 / 4], d->bar[0xc1604 / 4]);
        return false;
    }
    IOLog("X9 WIFI CMD: cid 0x%02x seq %u event after %llu ms\n", cid, seq, x9_ms_since(tw));
    uint16_t rbc, ptype;
    memcpy(&rbc, ev + 32, 2);
    memcpy(&ptype, ev + 34, 2);
    uint8_t eid = ev[36], eseq = ev[37];
    uint32_t rxd0, rxd1;
    memcpy(&rxd0, ev, 4);
    memcpy(&rxd1, ev + 4, 4);
    IOLog("X9 WIFI CMD: cid 0x%02x seq %u -> event len=%d rxd0=%08x rxd1=%08x bytes=%u type=%04x eid=%u seq=%u data=%02x %02x %02x %02x\n",
        cid, seq, n, rxd0, rxd1, rbc, ptype, eid, eseq,
        ev[40], ev[41], ev[42], ev[43]);
    if (eid != evt || eseq != seq) {
        IOLog("X9 WIFI CMD: expected eid %u seq %u\n", evt, seq);
        return false;
    }
    if (res && rcap) {
        memcpy(res, ev + 40, rcap < sizeof ev - 40 ? rcap : sizeof ev - 40);
    }
    return true;
}
static uint32_t
x9_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void
x9_wifi_stage6(volatile uint32_t *bar)
{
    if (!X9_WIFI_FW_PRESENT || x9_wifi_patch_len < 96) {
        IOLog("X9 WIFI FW: no firmware embedded (build without .local/x9/wifi-fw); skipping DMA\n");
        return;
    }
    IOLog("X9 WIFI FW: patch %s %u B sha256 %.16s..., ram %u B %.16s..., phy %u B %.16s...\n",
        x9_wifi_patch_name, x9_wifi_patch_len, x9_wifi_patch_sha256, x9_wifi_ram_len, x9_wifi_ram_sha256,
        x9_wifi_phy_len, x9_wifi_phy_sha256);
    IOByteCount seglen = 0;
    x9_wdma d = {};
    d.bar = bar;
    if (sweet_x9_arg("x9_wifi_rsvmem", 1) == 1) {
        static_assert(X9_DMA_SIZE <= X9_WIFI_RSV_SIZE, "DMA area must fit the LK Wi-Fi pool");
        d.va = (uint8_t *)ml_io_map_wcomb((vm_offset_t)X9_WIFI_RSV_PA, X9_DMA_SIZE);
        d.pa = X9_WIFI_RSV_PA;
        d.nc = true;
        seglen = X9_DMA_SIZE;
        IOLog("X9 WIFI DMA: using LK wifi-reserve-memory pool pa=%llx size=%llx (x9_wifi_rsvmem=1, Normal NC map va=%p)\n",
            X9_WIFI_RSV_PA, X9_WIFI_RSV_SIZE, d.va);
    } else {
        IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, X9_DMA_SIZE, 0xfffffffffULL & ~0xfffULL);
        if (!md) {
            IOLog("X9 WIFI DMA: buffer allocation failed\n");
            return;
        }
        d.va = (uint8_t *)md->getBytesNoCopy();
        d.pa = md->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
    }
    if (!d.va || !d.pa || seglen < X9_DMA_SIZE) {
        IOLog("X9 WIFI DMA: buffer va=%p pa=%llx seg=%llx unusable\n", d.va, d.pa, (uint64_t)seglen);
        return;                              /* leaked on purpose */
    }
    memset(d.va, 0, X9_DMA_SIZE);
    for (unsigned i = 0; i < X9_RING_N; i++) {
        x9_wd_rx_desc(&d, i);
    }
    x9_wd_sync(&d, 0, X9_DMA_SIZE);
    uint32_t glo0 = x9_wd_rd(&d, 0x208);
    uint32_t hi = (uint32_t)(d.pa >> 32) & 0xf;
    IOLog("X9 WIFI DMA: buffer pa=%llx va=%p glo_cfg=%08x int_sta=%08x tx15 ctrl=%08x/%08x/%08x/%08x rx7 ctrl=%08x/%08x/%08x/%08x\n",
        d.pa, d.va, glo0, x9_wd_rd(&d, 0x200), x9_wd_rd(&d, 0x3f0), x9_wd_rd(&d, 0x3f4), x9_wd_rd(&d, 0x3f8),
        x9_wd_rd(&d, 0x3fc), x9_wd_rd(&d, 0x570), x9_wd_rd(&d, 0x574), x9_wd_rd(&d, 0x578), x9_wd_rd(&d, 0x57c));
    x9_wifi_rc_diag("before DMA");
    x9_wd_state(&d, "before DMA");
    /*
     * 54.txt follow-up: DMADONE stayed 0 with no PCIe error, consistent with WFDMA reading the TXD and
     * then waiting for DMASHDL pages. Bypass test, as gen4m does when DMASHDL is disabled
     * (cmm_asic_connac3x.c asicConnac3xWfdmaInitForUSB): GLO_CFG_EXT0 bit 6 TX_DMASHDL_EN off and
     * DMASHDL SW_CONTROL (0x20026004) bit 28 DMASHDL_BYPASS on. Both restored in the teardown.
     */
    uint32_t ext0Orig = x9_wd_rd(&d, 0x2b0), shdlOrig = bar[0xd6004 / 4];
    x9_wd_wr(&d, 0x2b0, ext0Orig & ~(1u << 6));
    bar[0xd6004 / 4] = shdlOrig | (1u << 28);
    __asm__ volatile ("dsb sy" ::: "memory");
    IOLog("X9 WIFI DMASHDL: bypass test: ext0 %08x -> %08x, sw_ctrl %08x -> %08x\n",
        ext0Orig, x9_wd_rd(&d, 0x2b0), shdlOrig, bar[0xd6004 / 4]);
    /* 52.txt follow-up: if the WFDMA wrapper's AXI sleep protection is enabled, release it */
    uint32_t axislp0 = bar[0xd7050 / 4];
    if (axislp0 & 0x00010001u) {
        bar[0xd7050 / 4] = axislp0 & ~0x00010001u;
        __asm__ volatile ("dsb sy" ::: "memory");
        IODelay(10);
        IOLog("X9 WIFI WFDMA: AXI sleep protection was ENABLED (%08x); cleared -> %08x\n", axislp0, bar[0xd7050 / 4]);
    } else {
        IOLog("X9 WIFI WFDMA: AXI sleep protection off (%08x)\n", axislp0);
    }
    /* halWpdmaInitRing order: DMA off, rings, prefetch + index reset, DMA on */
    const uint32_t gloOff = 0x5030b850u | (1u << 26);
    x9_wd_wr(&d, 0x208, gloOff);
    static const unsigned txr[2] = { 15, 16 };
    for (unsigned k = 0; k < 2; k++) {
        unsigned n = txr[k];
        uint64_t base = d.pa + (n == 15 ? X9_D_TX15 : X9_D_TX16);
        x9_wd_wr(&d, 0x300 + n * 0x10, (uint32_t)base);
        x9_wd_wr(&d, 0x308 + n * 0x10, 0);
        x9_wd_wr(&d, 0x304 + n * 0x10, X9_RING_N | hi << 16);
    }
    x9_wd_wr(&d, 0x570, (uint32_t)(d.pa + X9_D_RX7));
    x9_wd_wr(&d, 0x574, X9_RING_N | hi << 16);
    x9_wd_wr(&d, 0x578, X9_RING_N - 1);
    static const struct { uint16_t off; uint32_t v; } pf[] = {      /* mt6653WfdmaManualPrefetch, no MDDP */
        { 0x690, 0x00000008 }, { 0x694, 0x00800008 }, { 0x698, 0x01000008 },     /* RX4..6 */
        { 0x69c, 0x01800004 }, { 0x6a0, 0x01c00004 },                            /* RX7, RX8 */
        { 0x640, 0x02000004 },                                                   /* TX16 */
        { 0x600, 0x02000010 }, { 0x604, 0x03000010 }, { 0x608, 0x04000010 }, { 0x60c, 0x05000010 },
        { 0x610, 0x06000004 }, { 0x614, 0x06400004 },                            /* TX4, TX5 */
        { 0x63c, 0x06800004 },                                                   /* TX15 */
    };
    for (unsigned k = 0; k < sizeof(pf) / sizeof(pf[0]); k++) {
        x9_wd_wr(&d, pf[k].off, pf[k].v);
    }
    x9_wd_wr(&d, 0x20c, 0xffffffffu);        /* RST_DTX_PTR */
    x9_wd_wr(&d, 0x280, 0xffffffffu);        /* RST_DRX_PTR */
    x9_wd_wr(&d, 0x3f8, 0);
    x9_wd_wr(&d, 0x408, 0);
    x9_wd_wr(&d, 0x578, X9_RING_N - 1);
    x9_wd_wr(&d, 0x208, gloOff | (1u << 0) | (1u << 2));          /* TX_DMA_EN | RX_DMA_EN */
    x9_wd_wr(&d, 0x2b4, 0x8c800404u | 0x10000000u);               /* EXT1 + TX_FCTRL_MODE */
    x9_wd_wr(&d, 0x2b8, 0x00001404u);                             /* EXT2 */
    IOLog("X9 WIFI DMA: rings up glo=%08x ext1=%08x ext2=%08x tx15=%08x/%08x/%u/%u tx16=%08x/%08x/%u/%u rx7=%08x/%08x/%u/%u int_sta=%08x\n",
        x9_wd_rd(&d, 0x208), x9_wd_rd(&d, 0x2b4), x9_wd_rd(&d, 0x2b8),
        x9_wd_rd(&d, 0x3f0), x9_wd_rd(&d, 0x3f4), x9_wd_rd(&d, 0x3f8), x9_wd_rd(&d, 0x3fc),
        x9_wd_rd(&d, 0x400), x9_wd_rd(&d, 0x404), x9_wd_rd(&d, 0x408), x9_wd_rd(&d, 0x40c),
        x9_wd_rd(&d, 0x570), x9_wd_rd(&d, 0x574), x9_wd_rd(&d, 0x578), x9_wd_rd(&d, 0x57c), x9_wd_rd(&d, 0x200));

    /* ROM patch, PATCH_FORMAT_V2 */
    const uint8_t *fw = x9_wifi_patch;
    unsigned nsec = x9_be32(fw + 32 + 12);
    char date[17];
    memcpy(date, fw, 16);
    date[16] = 0;
    for (unsigned k = 0; k < 16; k++) {
        if (date[k] < 0x20 || date[k] > 0x7e) date[k] = '.';
    }
    uint32_t swhw;
    memcpy(&swhw, fw + 20, 4);
    IOLog("X9 WIFI PATCH: date %s platform %.4s swhw %08x sections %u\n", date, (const char *)(fw + 16), swhw, nsec);
    bool ok = nsec >= 1 && nsec <= 8 && 96 + 64 * nsec <= x9_wifi_patch_len;
    unsigned sent = 0;
    uint64_t tp = mach_absolute_time();
    for (unsigned s = 0; ok && s < nsec; s++) {
        const uint8_t *m = fw + 96 + 64 * s;
        uint32_t type = x9_be32(m), off = x9_be32(m + 4), addr = x9_be32(m + 12), len = x9_be32(m + 16);
        uint32_t info = x9_be32(m + 20);
        if ((type & 0xffff) != 2) {
            IOLog("X9 WIFI PATCH: section %u type %08x not binary, skipped\n", s, type);
            continue;
        }
        if (off > x9_wifi_patch_len || len > x9_wifi_patch_len - off) {
            IOLog("X9 WIFI PATCH: section %u outside the file\n", s);
            ok = false;
            break;
        }
        uint32_t mode = 1u << 31;                                 /* DOWNLOAD_CONFIG_ACK_OPTION */
        if (info != 0xffffffffu) {
            unsigned enc = info >> 24;
            if (enc == 1) mode |= 1u | ((info & 0xffu) << 1 & 0x6u) | (1u << 3);
            else if (enc == 2) mode |= 1u | (1u << 6) | (1u << 3);
        }
        uint32_t cfg[3] = { addr, len, mode };
        uint8_t res[4] = {};
        bool c = x9_wd_cmd(&d, 0x05, cfg, sizeof cfg, 1, res, sizeof res);
        IOLog("X9 WIFI PATCH: section %u addr=%08x len=%u info=%08x mode=%08x PATCH_START %s status=%u cid=%02x\n",
            s, addr, len, info, mode, c ? "answered" : "FAILED", res[0], res[1]);
        if (!c || res[0] != 0) {
            ok = false;
            break;
        }
        for (uint32_t at = 0; at < len; at += 2048) {
            unsigned chunk = len - at < 2048 ? len - at : 2048;
            memcpy(d.va + X9_B_FW, fw + off + at, chunk);
            if (!x9_wd_tx(&d, 16, X9_B_FW, chunk)) {
                IOLog("X9 WIFI PATCH: section %u data stuck at %u/%u\n", s, at, len);
                ok = false;
                break;
            }
            sent += chunk;
        }
        if (ok) {
            IOLog("X9 WIFI PATCH: section %u data sent (%u B) t=%llu ms int_sta=%08x\n", s, len, x9_ms_since(tp),
                x9_wd_rd(&d, 0x200));
            /* 68.txt: slpprot0 b28 / CB slp_status b5 / R2A dmawr_probe were only seen after the PATCH_FINISH
             * timeout; dump right after the data to see whether they already flip during the data phase. */
            x9_wifi_rc_diag(s == 0 ? "after data s0" : "after data s1");
        }
    }
    if (ok) {
        uint8_t fin[4] = { 0, 0, 0, 0 }, res[4] = {};
        bool c = x9_wd_cmd(&d, 0x07, fin, sizeof fin, 1, res, sizeof res);
        IOLog("X9 WIFI PATCH: PATCH_FINISH %s status=%u cid=%02x; %u B in %llu ms -> %s\n",
            c ? "answered" : "FAILED", res[0], res[1], sent, x9_ms_since(tp),
            (c && res[0] == 0) ? "ROM PATCH DOWNLOADED" : "patch download failed");
    }
    IOLog("X9 WIFI MCU: after patch rom_sram=%08x romcode_index=%08x conn_on_misc=%08x events=%u\n",
        bar[0x1ab160 / 4], bar[0xc1604 / 4], bar[0xe00f0 / 4], d.events);
    /* teardown: DMA off, wait idle, indexes reset, ring registers cleared */
    x9_wd_wr(&d, 0x208, gloOff);
    uint64_t ti = mach_absolute_time();
    while ((x9_wd_rd(&d, 0x208) & 0xau) && x9_ms_since(ti) < 100) {
        IODelay(50);
    }
    x9_wd_wr(&d, 0x20c, 0xffffffffu);
    x9_wd_wr(&d, 0x280, 0xffffffffu);
    for (unsigned k = 0; k < 2; k++) {
        x9_wd_wr(&d, 0x300 + txr[k] * 0x10, 0);
        x9_wd_wr(&d, 0x304 + txr[k] * 0x10, 0);
    }
    x9_wd_wr(&d, 0x570, 0);
    x9_wd_wr(&d, 0x574, 0);
    x9_wd_wr(&d, 0x208, glo0 & ~0x5u);
    x9_wd_wr(&d, 0x2b0, ext0Orig);
    bar[0xd6004 / 4] = shdlOrig;
    __asm__ volatile ("dsb sy" ::: "memory");
    if (axislp0 & 0x00010001u) {
        bar[0xd7050 / 4] = axislp0;
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    IOLog("X9 WIFI DMA: stopped glo=%08x int_sta=%08x (buffer kept)\n", x9_wd_rd(&d, 0x208), x9_wd_rd(&d, 0x200));
}

/*
 * Stage 4 (47.txt follow-up): stage 3 released the MAC/PHY resets, base_conf
 * became 14110f01 and the link trained to L0 (LTSSM 0x10) 16 ms after PERST,
 * but the bus-1 config read returned ffffffff: the root port (bus 0 devfn 0,
 * a type-1 bridge) still had secondary/subordinate bus 0, so nothing was
 * forwarded. Stage 4, all inside the link-up window:
 *  - pre_init_6993 safety bits first: AXI_IF_CTRL 0x1a8 |= AXI0_SLV_RESP_MASK
 *    (bit 12) | SW_CPLTO_DATA_SEL (bit 28) so a completion timeout answers
 *    zeros instead of an AXI slave error (SError).
 *  - RC: id, bus numbers 0x18 = 00 01 01, memory window 0x20 = 0x5000..0x57f0,
 *    command |= MEM | MASTER.
 *  - EP bus 1 devfn 0: id/class/header, capability walk (PCIe cap link status),
 *    BAR0 (and BAR1 if 64-bit) sizing with restore.
 *  - Translation table 0 (0x800, mtk_pcie_set_trans_table): cpu 0x50000000 ->
 *    pci 0x50000000, 128 MiB (pcie0 ranges in mt6993.dts), MEM type; BAR0 =
 *    0x50000000; EP command = MEM only (no bus master, no DMA).
 *  - Read-only MMIO through BAR0 via mt6653_bus2chip_cr_mapping: CB TOP
 *    0x70000000 (bus 0x1e0000), CB infra1 0x70010200..208 (0x1c0200), conn_infra
 *    cfg 0x7c011000 (0x101000, IP version, expect 0x03040001), WF_TOP_CFG
 *    0x80020010 (0xb0010), TOP_HCR 0x88000000 (0x140000, expect 6653 in it).
 *  - Undo: EP command 0, BAR0 back, table 0 disabled, RC command/bus/window
 *    restored, AXI_IF_CTRL restored.
 */
static void
x9_wifi_pcie_stage4(volatile uint32_t *mac, unsigned stage)
{
    uint32_t axi0 = mac[0x1a8 / 4];
    mac[0x1a8 / 4] = axi0 | (1u << 12) | (1u << 28);
    __asm__ volatile ("dsb sy" ::: "memory");
    uint32_t rcid = x9_pcie_cfg_rd(mac, 0, 0, 0x00), rccls = x9_pcie_cfg_rd(mac, 0, 0, 0x08);
    uint32_t rccmd = x9_pcie_cfg_rd(mac, 0, 0, 0x04), rcbus = x9_pcie_cfg_rd(mac, 0, 0, 0x18);
    uint32_t rcmem = x9_pcie_cfg_rd(mac, 0, 0, 0x20);
    /* 55.txt follow-up: prefetchable window (never programmed by us). Linux pci_setup_bridge disables it
     * when nothing behind the bridge is prefetchable: base 0xfff0 / limit 0 in 0x24, upper 32 bits 0. */
    uint32_t rcio = x9_pcie_cfg_rd(mac, 0, 0, 0x1c), rcpf = x9_pcie_cfg_rd(mac, 0, 0, 0x24);
    uint32_t rcpfbu = x9_pcie_cfg_rd(mac, 0, 0, 0x28), rcpflu = x9_pcie_cfg_rd(mac, 0, 0, 0x2c);
    IOLog("X9 WIFI EP: RC id=%08x class=%08x cmd=%08x bus=%08x mem=%08x axi_if=%08x->%08x\n",
        rcid, rccls, rccmd, rcbus, rcmem, axi0, mac[0x1a8 / 4]);
    x9_pcie_cfg_wr(mac, 0, 0, 0x18, (rcbus & 0xff000000u) | 0x00010100u);
    x9_pcie_cfg_wr(mac, 0, 0, 0x20, 0x57f05000u);
    if (stage >= 6) {
        x9_pcie_cfg_wr(mac, 0, 0, 0x24, 0x0000fff0u);
        x9_pcie_cfg_wr(mac, 0, 0, 0x28, 0);
        x9_pcie_cfg_wr(mac, 0, 0, 0x2c, 0);
        IOLog("X9 WIFI EP: RC windows: io/sec %08x, pref %08x/%08x/%08x -> %08x/%08x/%08x (disabled as Linux)\n",
            rcio, rcpf, rcpfbu, rcpflu, x9_pcie_cfg_rd(mac, 0, 0, 0x24), x9_pcie_cfg_rd(mac, 0, 0, 0x28),
            x9_pcie_cfg_rd(mac, 0, 0, 0x2c));
    }
    x9_pcie_cfg_wr(mac, 0, 0, 0x04, (rccmd & 0xffffu) | 0x6u);
    IOLog("X9 WIFI EP: RC bus=%08x mem=%08x cmd=%08x int_sta=%08x\n",
        x9_pcie_cfg_rd(mac, 0, 0, 0x18), x9_pcie_cfg_rd(mac, 0, 0, 0x20), x9_pcie_cfg_rd(mac, 0, 0, 0x04), mac[0x184 / 4]);
    uint32_t id = x9_pcie_cfg_rd(mac, 1, 0, 0x00);
    IOLog("X9 WIFI EP: id=%08x cmd/sta=%08x class/rev=%08x hdr=%08x subsys=%08x cap=%02x int=%08x int_sta=%08x\n",
        id, x9_pcie_cfg_rd(mac, 1, 0, 0x04), x9_pcie_cfg_rd(mac, 1, 0, 0x08), x9_pcie_cfg_rd(mac, 1, 0, 0x0c),
        x9_pcie_cfg_rd(mac, 1, 0, 0x2c), x9_pcie_cfg_rd(mac, 1, 0, 0x34) & 0xff, x9_pcie_cfg_rd(mac, 1, 0, 0x3c), mac[0x184 / 4]);
    if (id == 0xffffffffu || id == 0) {
        IOLog("X9 WIFI EP: no endpoint answer; stopping before BAR/MMIO\n");
        goto undo_rc;
    }
    {
        unsigned capp = x9_pcie_cfg_rd(mac, 1, 0, 0x34) & 0xfc;
        for (unsigned n = 0; capp >= 0x40 && n < 16; n++) {
            uint32_t h = x9_pcie_cfg_rd(mac, 1, 0, capp);
            if ((h & 0xff) == 0x10) {
                uint32_t lcap = x9_pcie_cfg_rd(mac, 1, 0, capp + 0x0c), lsta = x9_pcie_cfg_rd(mac, 1, 0, capp + 0x10) >> 16;
                IOLog("X9 WIFI EP: cap 0x%02x PCIe ver=%u type=%u linkcap=%08x link speed=Gen%u width=x%u devcap=%08x\n",
                    capp, (h >> 16) & 0xf, (h >> 20) & 0xf, lcap, lsta & 0xf, (lsta >> 4) & 0x3f, x9_pcie_cfg_rd(mac, 1, 0, capp + 4));
            } else {
                IOLog("X9 WIFI EP: cap 0x%02x id=0x%02x hdr=%08x\n", capp, h & 0xff, h);
            }
            capp = (h >> 8) & 0xfc;
        }
    }
    {
        uint32_t bar0 = x9_pcie_cfg_rd(mac, 1, 0, 0x10), bar1 = x9_pcie_cfg_rd(mac, 1, 0, 0x14);
        bool is64 = ((bar0 >> 1) & 3) == 2;
        x9_pcie_cfg_wr(mac, 1, 0, 0x10, 0xffffffffu);
        uint32_t sz0 = x9_pcie_cfg_rd(mac, 1, 0, 0x10);
        uint32_t sz1 = 0xffffffffu;
        if (is64) {
            x9_pcie_cfg_wr(mac, 1, 0, 0x14, 0xffffffffu);
            sz1 = x9_pcie_cfg_rd(mac, 1, 0, 0x14);
        }
        uint64_t mask = ((uint64_t)(is64 ? sz1 : 0xffffffffu) << 32) | (sz0 & ~0xfu);
        uint64_t size = mask ? (~mask + 1) : 0;
        IOLog("X9 WIFI EP: BAR0 orig=%08x/%08x sizing=%08x/%08x %s size=0x%llx\n",
            bar0, bar1, sz0, sz1, is64 ? "64-bit" : "32-bit", size);
        if (size < 0x200000 || size > 0x8000000) {
            IOLog("X9 WIFI EP: BAR0 size outside 2..128 MiB; not mapping\n");
            x9_pcie_cfg_wr(mac, 1, 0, 0x10, bar0);
            if (is64) x9_pcie_cfg_wr(mac, 1, 0, 0x14, bar1);
            goto undo_rc;
        }
        /* translation table 0: cpu 0x50000000 -> pci 0x50000000, 128 MiB, MEM */
        volatile uint32_t *tt = mac + 0x800 / 4;
        uint32_t tt0[5] = { tt[0], tt[1], tt[2], tt[3], tt[4] };
        tt[0] = 0x50000000u | ((((27u - 1u) << 1) & 0x7eu) | 1u);
        tt[1] = 0; tt[2] = 0x50000000u; tt[3] = 0; tt[4] = 0;
        __asm__ volatile ("dsb sy" ::: "memory");
        x9_pcie_cfg_wr(mac, 1, 0, 0x10, 0x50000000u | (bar0 & 0xfu));
        if (is64) x9_pcie_cfg_wr(mac, 1, 0, 0x14, 0);
        uint32_t epcmd = x9_pcie_cfg_rd(mac, 1, 0, 0x04);
        x9_pcie_cfg_wr(mac, 1, 0, 0x04, (epcmd & 0xfff8u) | 0x2u);       /* MEM only */
        IOLog("X9 WIFI EP: tt0=%08x/%08x/%08x/%08x/%08x (was %08x) BAR0=%08x cmd=%08x\n",
            tt[0], tt[1], tt[2], tt[3], tt[4], tt0[0], x9_pcie_cfg_rd(mac, 1, 0, 0x10), x9_pcie_cfg_rd(mac, 1, 0, 0x04));
        volatile uint32_t *bar = (volatile uint32_t *)ml_io_map(0x50000000, 0x200000);
        if (bar) {
            if (stage >= 6) {
                x9_wifi_pend(bar, mac, "first BAR access (EP cmd MEM only)");
            }
            static const struct { uint32_t off; const char *what; } r[] = {
                { 0x1e0000, "CB_TOP 0x70000000" }, { 0x1e0004, "CB_TOP 0x70000004" },
                { 0x1c0200, "CB_INFRA1 0x70010200" }, { 0x1c0204, "CB_INFRA1 0x70010204" },
                { 0x1c0208, "CB_INFRA1 0x70010208" }, { 0x101000, "CONN_CFG IPVER 0x7c011000" },
                { 0xb0010, "WF_TOP_CFG IPVER 0x80020010" }, { 0x140000, "TOP_HCR 0x88000000" },
                { 0x140004, "TOP_FVR 0x88000004" }, { 0xe00f0, "CONN_ON_MISC 0x200600f0" },
            };
            for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
                uint32_t v = bar[r[i].off / 4];
                IOLog("X9 WIFI MMIO: %-26s bar+0x%06x = %08x int_sta=%08x\n", r[i].what, r[i].off, v, mac[0x184 / 4]);
            }
            if (stage >= 5) {
                /*
                 * Stage 5 (48.txt follow-up: EP 14c3:6653 Gen4 x1, BAR0 2 MiB, CB_INFRA1
                 * HCR 0x6653, conn_infra/WF IP version 0x03040001). gen4m mt6653.c
                 * mt6653_mcu_check_idle polls the ROM state at sram 0x7c05b160 (bar
                 * 0x1ab160) and WF_TOP_CFG_ON_ROMCODE_INDEX 0x81021604 (bar 0xc1604) for
                 * MCU_IDLE 0x1D1E; the driver-own handshake is CONN_HOST_CSR_TOP
                 * WF_BAND0_LPCTL 0x20060010 (bar 0xe0010): write PCIE_LPCR_HOST_CLR_OWN
                 * (bit1), owned when OWNER_STATE_SYNC (bit2) reads 0
                 * (mt6653LowPowerOwnClear); SET_OWN (bit0) hands it back (bit2 = 1).
                 * Also read WFDMA host DMA0 GLO_CFG 0x20024208 (bar 0xd4208).
                 */
                uint32_t rom = bar[0x1ab160 / 4], romidx = bar[0xc1604 / 4], lp0 = bar[0xe0010 / 4];
                IOLog("X9 WIFI MCU: rom_sram=%08x romcode_index=%08x (idle=00001d1e) lpctl=%08x irq_stat=%08x wakeup_top=%08x\n",
                    rom, romidx, lp0, bar[0xe0014 / 4], bar[0xe01a0 / 4]);
                bar[0xe0010 / 4] = 1u << 1;                         /* HOST_CLR_OWN: driver own */
                __asm__ volatile ("dsb sy" ::: "memory");
                uint64_t td = mach_absolute_time();
                uint32_t lp = bar[0xe0010 / 4];
                while ((lp & (1u << 2)) && x9_ms_since(td) < 200) {
                    IODelay(100);
                    lp = bar[0xe0010 / 4];
                }
                IOLog("X9 WIFI MCU: driver own %s after %llu ms lpctl=%08x irq_stat=%08x\n",
                    (lp & (1u << 2)) ? "NOT taken" : "TAKEN", x9_ms_since(td), lp, bar[0xe0014 / 4]);
                IOLog("X9 WIFI MCU: owned: rom_sram=%08x romcode_index=%08x wfdma0 glo_cfg=%08x conn_on_misc=%08x hcr=%08x\n",
                    bar[0x1ab160 / 4], bar[0xc1604 / 4], bar[0xd4208 / 4], bar[0xe00f0 / 4], bar[0x140000 / 4]);
                if (stage >= 6) {
                    x9_wifi_pend(bar, mac, "after driver own");
                }
                if (stage >= 6 && !(lp & (1u << 2))) {
                    /*
                     * 51.txt: host side clean (no AXI error, sleep protect released, resources
                     * acked) yet TX15 never fetched -> the chip never issued the read. Do the two
                     * writes mt6653_mcu_init makes before anything else (WMT build skips mcu_reset):
                     * CB_INFRA_SLP_PROT_SW_CTRL 0x70025214 = 0 (static window bar 0x1f5214, the same
                     * path gen4m's HAL_MCR_WR takes) and CONN_HOST_CSR_TOP AP2CONN_ACCESS_DETECT_EN
                     * 0x20060128 (bar 0xe0128) = 0. Both restored afterwards.
                     */
                    x9_ep_bar = bar;
                    uint32_t slp0 = bar[0x1f5214 / 4], det0 = bar[0xe0128 / 4];
                    x9_wifi_rc_diag("as ROM left it");
                    IOLog("X9 WIFI CHIP: cb remap_wf=%08x (def 74037001)\n", bar[0x1f6554 / 4]);
                    bar[0x1f5214 / 4] = 0;
                    __asm__ volatile ("dsb sy" ::: "memory");
                    bar[0xe0128 / 4] = 0;
                    __asm__ volatile ("dsb sy" ::: "memory");
                    IODelay(100);
                    IOLog("X9 WIFI CHIP: slp_prot_sw %08x -> %08x, access_detect %08x -> %08x\n",
                        slp0, bar[0x1f5214 / 4], det0, bar[0xe0128 / 4]);
                    /*
                     * 56.txt + mt76 comparison: mt76 cold-starts MT7927 (same BAR layout as MT6653) with
                     * mt7927_wfsys_reset before any DMA (mt7925/pci.c probe, mt792x_dma.c): CONN_CFG_EMI_CTL_0
                     * 0x7c011100 (bar 0x101100) bit1 SLPPROT_EN = 1; CB_INFRA_RGU WF reset (bar 0x1f8600) bit4
                     * set, 1-2 ms, clear, 5-10 ms; CB crypto-top MCU_OWN_SET (bar 0x1f5034) = 1; poll
                     * ROMCODE_INDEX (bar 0xc1604) & 0xffff == 0x1d1e for 200 ms. gen4m's equivalent
                     * mt6653_mcu_reset is compiled out of WMT builds.
                     */
                    uint32_t emi0 = bar[0x101100 / 4], rgu0 = bar[0x1f8600 / 4];
                    IOLog("X9 WIFI WFSYS: before reset emi_ctl0=%08x rgu_wf_rst=%08x mcu_own=%08x romcode=%08x lpctl=%08x\n",
                        emi0, rgu0, bar[0x1f5030 / 4], bar[0xc1604 / 4], bar[0xe0010 / 4]);
                    bar[0x101100 / 4] = emi0 | (1u << 1);
                    __asm__ volatile ("dsb sy" ::: "memory");
                    bar[0x1f8600 / 4] = bar[0x1f8600 / 4] | (1u << 4);
                    __asm__ volatile ("dsb sy" ::: "memory");
                    IODelay(1500);
                    bar[0x1f8600 / 4] = bar[0x1f8600 / 4] & ~(1u << 4);
                    __asm__ volatile ("dsb sy" ::: "memory");
                    IOSleep(8);
                    bar[0x1f5034 / 4] = 1u;
                    __asm__ volatile ("dsb sy" ::: "memory");
                    uint64_t tw = mach_absolute_time();
                    uint32_t rc = bar[0xc1604 / 4];
                    while ((rc & 0xffffu) != 0x1d1eu && x9_ms_since(tw) < 200) {
                        IODelay(500);
                        rc = bar[0xc1604 / 4];
                    }
                    bool wfOk = (rc & 0xffffu) == 0x1d1eu;
                    IOLog("X9 WIFI WFSYS: reset %s after %llu ms romcode=%08x rgu_wf_rst=%08x emi_ctl0=%08x mcu_own=%08x lpctl=%08x\n",
                        wfOk ? "DONE, ROM idle" : "TIMEOUT", x9_ms_since(tw), rc, bar[0x1f8600 / 4], bar[0x101100 / 4],
                        bar[0x1f5030 / 4], bar[0xe0010 / 4]);
                    x9_wifi_pend(bar, mac, "after WFSYS reset");
                    if (wfOk && (bar[0xe0010 / 4] & (1u << 2))) {   /* lost driver own in the reset: take it again */
                        bar[0xe0010 / 4] = 1u << 1;
                        __asm__ volatile ("dsb sy" ::: "memory");
                        uint64_t to = mach_absolute_time();
                        while ((bar[0xe0010 / 4] & (1u << 2)) && x9_ms_since(to) < 200) {
                            IODelay(100);
                        }
                        IOLog("X9 WIFI WFSYS: driver own again %s lpctl=%08x\n",
                            (bar[0xe0010 / 4] & (1u << 2)) ? "NOT taken" : "taken", bar[0xe0010 / 4]);
                    }
                    x9_pcie_cfg_wr(mac, 1, 0, 0x04, (epcmd & 0xfff8u) | 0x6u);   /* MEM | BUS MASTER */
                    IOLog("X9 WIFI EP: bus master on cmd/sta=%08x\n", x9_pcie_cfg_rd(mac, 1, 0, 0x04));
                    x9_wifi_pend(bar, mac, "right after bus master on");
                    IODelay(1000);
                    x9_wifi_pend(bar, mac, "1 ms after bus master on");
                    if (wfOk && !(bar[0xe0010 / 4] & (1u << 2))) {
                        x9_wifi_stage6(bar);
                    } else {
                        x9_wifi_rc_diag("after failed WFSYS reset");
                        IOLog("X9 WIFI WFSYS: DMA skipped (ROM not idle or no driver own)\n");
                    }
                    bar[0x101100 / 4] = emi0;
                    __asm__ volatile ("dsb sy" ::: "memory");
                    x9_pcie_cfg_wr(mac, 1, 0, 0x04, (epcmd & 0xfff8u) | 0x2u);   /* MEM only again */
                    bar[0xe0128 / 4] = det0;
                    bar[0x1f5214 / 4] = slp0;
                    __asm__ volatile ("dsb sy" ::: "memory");
                    x9_ep_bar = NULL;
                    IOLog("X9 WIFI EP: bus master off cmd/sta=%08x ltssm=%08x link=%08x\n",
                        x9_pcie_cfg_rd(mac, 1, 0, 0x04), mac[0x150 / 4], mac[0x154 / 4]);
                }
                bar[0xe0010 / 4] = 1u << 0;                         /* HOST_SET_OWN: back to FW */
                __asm__ volatile ("dsb sy" ::: "memory");
                IODelay(1000);
                IOLog("X9 WIFI MCU: fw own requested lpctl=%08x\n", bar[0xe0010 / 4]);
            }
        } else {
            IOLog("X9 WIFI MMIO: ml_io_map(0x50000000) failed\n");
        }
        IOLog("X9 WIFI EP: after MMIO ltssm=%08x link=%08x cmd/sta=%08x\n", mac[0x150 / 4], mac[0x154 / 4], x9_pcie_cfg_rd(mac, 1, 0, 0x04));
        x9_pcie_cfg_wr(mac, 1, 0, 0x04, epcmd & 0xfff8u);
        x9_pcie_cfg_wr(mac, 1, 0, 0x10, bar0);
        if (is64) x9_pcie_cfg_wr(mac, 1, 0, 0x14, bar1);
        tt[0] = tt0[0]; tt[1] = tt0[1]; tt[2] = tt0[2]; tt[3] = tt0[3]; tt[4] = tt0[4];
        __asm__ volatile ("dsb sy" ::: "memory");
    }
undo_rc:
    x9_pcie_cfg_wr(mac, 0, 0, 0x04, rccmd & 0xffffu);
    x9_pcie_cfg_wr(mac, 0, 0, 0x20, rcmem);
    x9_pcie_cfg_wr(mac, 0, 0, 0x18, rcbus);
    if (stage >= 6) {
        x9_pcie_cfg_wr(mac, 0, 0, 0x2c, rcpflu);
        x9_pcie_cfg_wr(mac, 0, 0, 0x28, rcpfbu);
        x9_pcie_cfg_wr(mac, 0, 0, 0x24, rcpf);
    }
    mac[0x140 / 4] = (1u << 20) | (0xfu << 16);
    mac[0x1a8 / 4] = axi0;
    __asm__ volatile ("dsb sy" ::: "memory");
    IOLog("X9 WIFI EP: undone (RC cmd/bus/mem, EP cmd/BAR, tt0, axi_if)\n");
}

/*
 * Stage 3 (46.txt follow-up): stage 2 showed faultb=1 after the pull-up (MT6653
 * PMIC up, no fault) but every PCIe0 MAC and PHY register read 0 and the RC
 * writes (setting/rst) did not stick. The MAC/PHY resets are a ti,syscon-reset
 * in pextp0cfg_ao (mt6993.dts: assert = write 1 to +0x8, deassert = write 1 to
 * +0xc, MAC bit 0, PHY bit 1, STATUS_NONE), so the 0 we read at +0x8 never
 * meant "released". mtk_pcie_power_up does reset_control_deassert(mac) before
 * the MAC is touched and deassert(phy) before phy_init. The MTCMOS status (ap_pd
 * 0x1c052900 bits 16/17, clk-mt6993-pd.c PD_AP_HWV_0 sta 0x12900) and the
 * pextp0 clock gates (cg_sta 0) were already on in 45/46.txt. No PHY efuse
 * calibration yet (nvmem pcie0_calibration), so a link, if any, is uncalibrated.
 */
static void
x9_wifi_pcie_try(volatile uint32_t *mac, volatile uint32_t *cg, uint64_t t0, unsigned stage)
{
    volatile uint32_t *phy = (volatile uint32_t *)ml_io_map(0x16900000, 0x10000);
    if (stage >= 3) {
        IOLog("X9 WIFI PCIE: stage 3 before reset release: base_conf=%08x setting=%08x phy glb_4c=%08x\n",
            mac[0x14 / 4], mac[0x80 / 4], phy ? phy[0x4c / 4] : 0);
        cg[0xc / 4] = 1u << 0;                             /* deassert PCIe0 MAC reset */
        __asm__ volatile ("dsb sy" ::: "memory");
        IODelay(100);
        IOLog("X9 WIFI PCIE: MAC reset released: base_conf=%08x setting=%08x rst=%08x ltssm=%08x\n",
            mac[0x14 / 4], mac[0x80 / 4], mac[0x148 / 4], mac[0x150 / 4]);
        cg[0xc / 4] = 1u << 1;                             /* deassert PCIe0 PHY reset */
        __asm__ volatile ("dsb sy" ::: "memory");
        IODelay(100);
    }
    if (phy) {
        IOLog("X9 WIFI PCIE: phy trx_64=%08x rx_30=%08x glb_4c=%08x misc=%08x\n",
            phy[0x3064 / 4], phy[0x5030 / 4], phy[0x4c / 4], mac[0x348 / 4]);
    }
    uint32_t req0 = cg[0x7c / 4];
    const uint32_t req_orig = req0;
    x9_rc_mac = mac;
    x9_rc_pextp = cg;
    /*
     * Stage 6b (50.txt: TX15 stuck with TX_DMA_BUSY, i.e. the EP's first read of host
     * memory never completed): do what Linux does before link start on mt6993
     * (pcie-mediatek-gen3.c mtk_pcie_pre_init_6993) and hold the SW votes that keep
     * the RC out of HW low-power mode (control_vote_v2: PEXTP_PWRCTL_6 = RC driver,
     * PWRCTL_8 = Wi-Fi driver, HWMODE_EN bit 0 = 0), then wait for PEXTP_SLPPROT_RDY
     * bits 2|3 (port 0 MAC/PHY sleep protection) to clear. All restored below.
     */
    uint32_t sv_res = mac[0xd2c / 4], sv_wcpl = mac[0x340 / 4], sv_spc2 = mac[0xd8 / 4];
    uint32_t sv_pw0 = cg[0x40 / 4], sv_pw6 = cg[0x58 / 4], sv_pw8 = cg[0x74 / 4];
    if (stage >= 6) {
        x9_wifi_rc_diag("as LK left it");
        mac[0xd2c / 4] = (sv_res & ~0xffu) | (1u << 10) | 0xau;   /* APSRC_ACK, SYS_CLK_RDY 10 us */
        mac[0x340 / 4] = 0x4u;                                    /* write completion timeout 4 ms */
        mac[0xd8 / 4] = sv_spc2 | 1u;                             /* record cplto cfg address */
        req0 |= 1u << 12;                                         /* RG_PEXTP_DDREN_ACK_SEL (kept below) */
        cg[0x40 / 4] = sv_pw0 | 0xff00u;                          /* PEXTP_TIMER_SET max */
        cg[0x58 / 4] = sv_pw6 & ~1u;                              /* RC vote: HW mode off */
        cg[0x74 / 4] = sv_pw8 & ~1u;                              /* Wi-Fi vote: HW mode off */
        __asm__ volatile ("dsb sy" ::: "memory");
        IODelay(500);
        uint64_t tv = mach_absolute_time();
        while ((cg[0x264 / 4] & 0xcu) && x9_ms_since(tv) < 10) {
            IODelay(10);
        }
        IOLog("X9 WIFI RC: votes held: pwrctl6=%08x pwrctl8=%08x slpprot_rdy=%08x (%s after %llu ms)\n",
            cg[0x58 / 4], cg[0x74 / 4], cg[0x264 / 4], (cg[0x264 / 4] & 0xcu) ? "STILL PROTECTED" : "released",
            x9_ms_since(tv));
    }
    IOLog("X9 WIFI PCIE: before setting=%08x cfgctrl=%08x rst=%08x ltssm=%08x link=%08x req_ctrl=%08x base_conf=%08x\n",
        mac[0x80 / 4], mac[0x84 / 4], mac[0x148 / 4], mac[0x150 / 4], mac[0x154 / 4], req0, mac[0x14 / 4]);
    mac[0x80 / 4] = mac[0x80 / 4] | 1u;                    /* RC mode */
    cg[0x7c / 4] = req0 | (1u << 0) | (1u << 4);           /* 26M force on, BBCK1 bypass */
    if (mac[0x84 / 4] & (1u << 2)) {
        mac[0x84 / 4] = mac[0x84 / 4] & ~(1u << 2);        /* LTSSM enable */
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    uint32_t rst = mac[0x148 / 4] | 0xfu;
    mac[0x148 / 4] = rst;                                  /* assert MAC/PHY/BRG/PE */
    __asm__ volatile ("dsb sy" ::: "memory");
    IOSleep(50);
    rst &= ~0x7u;
    mac[0x148 / 4] = rst;                                  /* release MAC/PHY/BRG */
    __asm__ volatile ("dsb sy" ::: "memory");
    IOSleep(50);
    rst &= ~0x8u;
    mac[0x148 / 4] = rst;                                  /* release PERST# */
    __asm__ volatile ("dsb sy" ::: "memory");
    uint64_t tp = mach_absolute_time();
    IOLog("X9 WIFI PCIE: t=%llu ms PERST released rst=%08x\n", x9_ms_since(t0), mac[0x148 / 4]);
    uint32_t lastS = ~0u, link = 0, polls = 0;
    while (x9_ms_since(tp) < 500) {
        uint32_t l = mac[0x150 / 4];
        uint32_t st = (l >> 24) & 0x1f;
        link = mac[0x154 / 4];
        if (st != lastS) {
            IOLog("X9 WIFI PCIE: +%llu ms ltssm_state=0x%02x reg=%08x link=%08x\n", x9_ms_since(tp), st, l, link);
            lastS = st;
        }
        if (link & (1u << 8)) {
            break;
        }
        polls++;
        IODelay(20);
    }
    IOLog("X9 WIFI PCIE: %s after %llu ms polls=%u ltssm=%08x link=%08x\n",
        (link & (1u << 8)) ? "LINK UP" : "link down", x9_ms_since(tp), polls, mac[0x150 / 4], link);
    if ((link & (1u << 8)) && stage >= 4) {
        x9_wifi_pcie_stage4(mac, stage);
        IOSleep(200);
        IOLog("X9 WIFI PCIE: after stage 4 ltssm=%08x link=%08x\n", mac[0x150 / 4], mac[0x154 / 4]);
    } else if (link & (1u << 8)) {
        uint32_t id[5];
        static const unsigned where[5] = { 0x00, 0x08, 0x10, 0x14, 0x2c };
        for (unsigned i = 0; i < 5; i++) {
            mac[0x140 / 4] = (1u << 20) | (0xfu << 16) | (1u << 8);   /* force BE, BE=f, bus 1 devfn 0 */
            __asm__ volatile ("dsb sy" ::: "memory");
            id[i] = mac[(0x1000 + where[i]) / 4];
        }
        mac[0x140 / 4] = (1u << 20) | (0xfu << 16);           /* back to RC config */
        IOLog("X9 WIFI PCIE: EP id=%08x class/rev=%08x bar0=%08x bar1=%08x subsys=%08x (want 665314c3)\n",
            id[0], id[1], id[2], id[3], id[4]);
        IOSleep(1000);
        IOLog("X9 WIFI PCIE: after 1 s ltssm=%08x link=%08x\n", mac[0x150 / 4], mac[0x154 / 4]);
    }
    mac[0x148 / 4] = mac[0x148 / 4] | 0x8u;                /* PERST# asserted again */
    if (stage >= 6) {
        req0 = req_orig;
        mac[0xd2c / 4] = sv_res;
        mac[0x340 / 4] = sv_wcpl;
        mac[0xd8 / 4] = sv_spc2;
        cg[0x40 / 4] = sv_pw0;
        cg[0x58 / 4] = sv_pw6;
        cg[0x74 / 4] = sv_pw8;
    }
    cg[0x7c / 4] = req0;
    __asm__ volatile ("dsb sy" ::: "memory");
    IOLog("X9 WIFI PCIE: PERST asserted, req_ctrl restored=%08x rst=%08x\n", cg[0x7c / 4], mac[0x148 / 4]);
    if (stage >= 3) {
        cg[0x8 / 4] = (1u << 0) | (1u << 1);               /* MAC + PHY reset asserted again (LK state) */
        __asm__ volatile ("dsb sy" ::: "memory");
        IOLog("X9 WIFI PCIE: MAC/PHY reset asserted again: base_conf=%08x\n", mac[0x14 / 4]);
    }
}
static void
x9_wifi_power_thread(void *arg, wait_result_t)
{
    unsigned stage = (unsigned)(uintptr_t)arg;
    IOSleep(20000);
    x9_gpio = (volatile uint32_t *)ml_io_map(0x1002d000, 0x1000);
    volatile uint32_t *mac = (volatile uint32_t *)ml_io_map(0x16910000, 0x2000);
    volatile uint32_t *cg = (volatile uint32_t *)ml_io_map(0x169b0000, 0x1000);
    volatile uint32_t *pd = (volatile uint32_t *)ml_io_map(0x1c052000, 0x1000);
    volatile uint32_t *io = (volatile uint32_t *)ml_io_map(X9_IOCFG_LM0, 0x1000);
    if (!x9_gpio || !mac || !cg || !pd || !io) {
        IOLog("X9 WIFI POWER: map failed; nothing written\n");
        thread_terminate(current_thread());
        return;
    }
    uint32_t pdsta = pd[0x900 / 4], cgsta = cg[0x14 / 4];
    bool pcieReadable = ((pdsta >> 16) & 1) && !(cgsta & 0x3003u);
    IOLog("X9 WIFI POWER: stage %u before: PMIC_EN in=%u WF_RST in=%u faultb in=%u pcie0 readable=%d iocfg ies=%08x pupd=%08x r0=%08x r1=%08x\n",
        stage, x9_gpio_in(251), x9_gpio_in(250), x9_gpio_in(65), pcieReadable,
        io[0x50 / 4], io[0x90 / 4], io[0xb0 / 4], io[0xc0 / 4]);
    uint64_t t0 = mach_absolute_time();
    x9_gpio_out(250, 1);                               /* wf_rst_on */
    IOLog("X9 WIFI POWER: WF_RST high (GPIO250 in=%u)\n", x9_gpio_in(250));
    IOSleep(30);
    x9_gpio_out(251, 1);                               /* connsys-pin-pmic-en-set */
    IOLog("X9 WIFI POWER: PMIC_EN high (GPIO251 in=%u)\n", x9_gpio_in(251));
    IOSleep(20);
    if (stage >= 2) {
        x9_gpio_rmw(0x300 + (65 / 4) * 0x10, 0x1fu << ((65 % 4) * 8), 0);   /* GPIO65 function 0 */
        x9_gpio_rmw((65 / 32) * 0x10, 1u << (65 % 32), 0);                   /* input */
        x9_faultb_bias(io, true);                      /* connsys-pin-pmic-faultb-enable */
        IOSleep(1);
        IOLog("X9 WIFI POWER: faultb pull-up on: faultb in=%u pupd=%08x r0=%08x r1=%08x\n",
            x9_gpio_in(65), io[0x90 / 4], io[0xb0 / 4], io[0xc0 / 4]);
    }
    IOSleep(10);
    if (stage >= 2 && pcieReadable) {
        x9_wifi_pcie_try(mac, cg, t0, stage);
    } else if (stage >= 2) {
        IOLog("X9 WIFI PCIE: skipped, PCIe0 MAC not powered/clocked (pd=%08x cg=%08x)\n", pdsta, cgsta);
    }
    uint32_t lastL = ~0u, lastK = ~0u, lastF = ~0u;
    for (unsigned i = 0; i < 60; i++) {                /* 3 s, every 50 ms; log changes only */
        uint32_t f = x9_gpio_in(65), l = pcieReadable ? mac[0x150 / 4] : 0, k = pcieReadable ? mac[0x154 / 4] : 0;
        if (f != lastF || l != lastL || k != lastK || i == 59) {
            IOLog("X9 WIFI POWER: t=%llu ms faultb=%u ltssm=%08x link=%08x rst_ctrl=%08x\n",
                x9_ms_since(t0), f, l, k, pcieReadable ? mac[0x148 / 4] : 0);
            lastF = f; lastL = l; lastK = k;
        }
        IOSleep(50);
    }
    if (stage >= 2) {
        x9_faultb_bias(io, false);                     /* connsys-pin-pmic-faultb-default */
    }
    IOSleep(20);
    x9_gpio_out(251, 0);                               /* connsys-pin-pmic-en-clr */
    IOSleep(200);                                      /* gen4m RST_PIN_MIN_WAIT_TIME */
    x9_gpio_out(250, 0);                               /* wf_rst_off */
    IOLog("X9 WIFI POWER: off again: PMIC_EN in=%u WF_RST in=%u faultb in=%u\n",
        x9_gpio_in(251), x9_gpio_in(250), x9_gpio_in(65));
    thread_terminate(current_thread());
}
static void
x9_wifi_power_test(void)
{
    int stage = sweet_x9_arg("x9_wifi_power", 0);
    if (stage < 1 || stage > 6) {
        return;
    }
    thread_t t = NULL;
    if (kernel_thread_start(x9_wifi_power_thread, (void *)(uintptr_t)stage, &t) == KERN_SUCCESS) {
        thread_deallocate(t);
        IOLog("X9 WIFI POWER: armed: stage %d power-on test in 20 s\n", stage);
    }
}

/* B0: print the shim's read-only copy of LK's Wi-Fi DT facts (/chosen/x9-lk-wifi-dt). */
static void
x9_wifi_print_lk_dt(void)
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *d = chosen ? OSDynamicCast(OSData, chosen->getProperty("x9-lk-wifi-dt")) : NULL;
    const uint8_t *p = d ? static_cast<const uint8_t *>(d->getBytesNoCopy()) : NULL;
    unsigned len = d ? d->getLength() : 0;
    if (!p || len < 9 || memcmp(p, "X9DT", 4)) {
        IOLog("X9 LK DT: no Wi-Fi DT copy (placeholder %s)\n", d ? "empty" : "absent");
        OSSafeReleaseNULL(chosen);
        return;
    }
    unsigned used = p[4] | (unsigned)p[5] << 8 | (unsigned)p[6] << 16, lines = 0;
    if (used > len - 9) {
        used = len - 9;
    }
    const char *text = reinterpret_cast<const char *>(p + 8);
    for (unsigned at = 0; at < used;) {
        unsigned end = at;
        while (end < used && text[end] != '\n') {
            end++;
        }
        IOLog("X9 LK DT: %.*s\n", (int)(end - at), text + at);
        lines++;
        at = end + 1;
    }
    IOLog("X9 LK DT: %u lines, %u bytes\n", lines, used);
    OSSafeReleaseNULL(chosen);
}
