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
static void
x9_wifi_pcie_try(volatile uint32_t *mac, volatile uint32_t *cg, uint64_t t0)
{
    volatile uint32_t *phy = (volatile uint32_t *)ml_io_map(0x16900000, 0x10000);
    if (phy) {
        IOLog("X9 WIFI PCIE: phy trx_64=%08x rx_30=%08x glb_4c=%08x misc=%08x\n",
            phy[0x3064 / 4], phy[0x5030 / 4], phy[0x4c / 4], mac[0x348 / 4]);
    }
    uint32_t req0 = cg[0x7c / 4];
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
    if (link & (1u << 8)) {
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
    cg[0x7c / 4] = req0;
    __asm__ volatile ("dsb sy" ::: "memory");
    IOLog("X9 WIFI PCIE: PERST asserted, req_ctrl restored=%08x rst=%08x\n", cg[0x7c / 4], mac[0x148 / 4]);
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
        x9_wifi_pcie_try(mac, cg, t0);
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
    if (stage != 1 && stage != 2) {
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
