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
/* real MT6653 backend (x9_wifi_real=1): the radio thread in stage 6 fills rbss and calls scan_done */
bool realWanted, realUp;
volatile bool scanReq;
volatile bool joinReq;
bss_info joinTarget;
bss_info rbss[32];
unsigned rcount;

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
  bss_count = realWanted ? 0 : 2; stats.scans++;
  const sta_events *ev = events;
  IOLockUnlock(lock);
  log("fake radio: scan done, %u networks", realWanted ? 0u : 2u);
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
  realWanted = sweet_x9_arg("x9_wifi_real", 0) == 1;
  make(bss[0], "X9-FAKE-OPEN", 1, 2437, -48, false);
  make(bss[1], "X9-FAKE-WPA2", 2, 5180, -61, true);
  bss_count = 0; stats.stage = 1;
  return 0;
}
void sta_set_events(const sta_events *ev) { IOLockLock(fake::lock); fake::events = ev; IOLockUnlock(fake::lock); }
int sta_scan(const uint8_t *, size_t) {
  using namespace fake;
  IOLockLock(lock);
  if (status.state == STA_IDLE) status.state = STA_SCANNING;
  bool up = realUp;
  if (up) { scanReq = true; IOLockWakeup(lock, (event_t)&scanReq, false); }
  IOLockUnlock(lock);
  if (!up) after(scanCall, 800);     /* fake radio, or the real one is not up yet: 0 networks */
  return 0;
}
int sta_bss_count(void) {
  IOLockLock(fake::lock); int n = (int)(fake::realUp ? fake::rcount : fake::bss_count); IOLockUnlock(fake::lock); return n;
}
int sta_bss_get(int i, bss_info *out) {
  using namespace fake;
  int r = -1;
  IOLockLock(lock);
  if (realUp) { if (i >= 0 && (unsigned)i < rcount) { memcpy(out, &rbss[i], sizeof *out); r = 0; } }
  else if (i >= 0 && (unsigned)i < bss_count) { memcpy(out, &bss[i], sizeof *out); r = 0; }
  IOLockUnlock(lock);
  return r;
}
int sta_connect(const uint8_t *ssid, size_t len, const uint8_t *bssid, const char *pass, size_t plen) {
  using namespace fake;
  int found = -1;
  IOLockLock(lock);
  if (realWanted) {
    int ti = -1;
    for (unsigned i = 0; i < rcount; i++)
      if ((bssid && !memcmp(bssid, rbss[i].bssid, 6)) ||
          (len && len == rbss[i].ssid_len && !memcmp(ssid, rbss[i].ssid, len))) { ti = (int)i; break; }
    if (ti < 0) {
      joinResult = RES_NOT_FOUND; status.state = STA_AUTH; IOLockUnlock(lock);
      log("real radio: join target not found");
      after(joinCall, 50);
      return 0;
    }
    if (rbss[ti].caps & 0x0010) {              /* Privacy bit: encrypted (WPA/enterprise) not supported yet */
      joinResult = RES_UNSUPPORTED; status.state = STA_AUTH; IOLockUnlock(lock);
      log("real radio: '%.*s' is encrypted; only open networks supported for now",
          rbss[ti].ssid_len, (const char *)rbss[ti].ssid);
      after(joinCall, 50);
      return 0;
    }
    memcpy(&joinTarget, &rbss[ti], sizeof joinTarget);
    memcpy(status.bssid, rbss[ti].bssid, 6); status.ssid_len = rbss[ti].ssid_len;
    memcpy(status.ssid, rbss[ti].ssid, 32); status.freq_mhz = rbss[ti].freq_mhz;
    status.signal_dbm = rbss[ti].signal_dbm; status.state = STA_AUTH;
    joinReq = true; IOLockWakeup(lock, (event_t)&scanReq, false);
    IOLockUnlock(lock);
    log("real radio: join '%.*s' requested (open)", rbss[ti].ssid_len, (const char *)rbss[ti].ssid);
    return 0;
  }
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
  log("en0 is up (%s radio), hardware address %02x:%02x:%02x:%02x:%02x:%02x", fake::realWanted ? "MT6653" : "FAKE", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
}
thread_call_t gStartCall;
void start_fired(thread_call_param_t, thread_call_param_t) { start(); }

}  // namespace
}  // namespace x9wifi

/* Called when configd first opens IONetworkStack: BSD networking is up by then. */
static void
x9_wifi_maybe_start(void)
{
    if (sweet_x9_arg("x9_wifi_fake", 0) != 1 && sweet_x9_arg("x9_wifi_real", 0) != 1) {
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
/* Device NVRAM (rev252bz): generated/x9_wifi_nvram.inc from .local/x9/wifi-nvram/WIFI, stub when absent. */
#include "../../../generated/x9_wifi_nvram.inc"
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
/* LK mblock-52 consys_wifi_emi = the wifi node's conninfra-emi-addr/size (mt6993.dts size 0xc00000, LK fills the
 * address) = gen4m mt6653 rEmiInfo (EMI_ALLOC_TYPE_LK); mblock-54 wifi-misc-reserve-memory = EMI2. */
#define X9_WIFI_EMI_PA 0xe1000000ULL
#define X9_WIFI_EMI_SIZE 0xc00000ULL
#define X9_WIFI_MISC_PA 0xe2600000ULL
#define X9_WIFI_MISC_EMI2_SIZE 0x9a100u          /* sum of mt6653_wifi_misc_rsv_mem_info sizes */
#define X9_EMI_BLK 0x10000u
#define X9_EMI_NBLK ((unsigned)(X9_WIFI_EMI_SIZE / X9_EMI_BLK))
/* rev252bc panicked: one 12 MiB ml_io_map_wcomb does not fit io_map's 8 MiB permanent submap (kmem_alloc failed 3,
 * same as rev247-lo), and io_submap mappings are KMA_PERMANENT (cannot be unmapped). ml_io_map_unmappable maps in
 * kernel_map (KMA_DATA) and ml_io_unmap frees it there. Walk the EMI through 1 MiB windows, unmapped after use. */
#define X9_EMI_WIN 0x100000u
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
static bool x9_emi_have_before;
static uint32_t x9_emi_sum_before[X9_EMI_NBLK];
/* report=false: fill out[]; report=true: compare with x9_emi_sum_before and log changed blocks. */
static void
x9_emi_walk(const char *when, bool report)
{
    static uint32_t now[X9_EMI_NBLK];
    uint32_t *out = report ? now : x9_emi_sum_before;
    unsigned nz = 0, changed = 0, mapped = 0;
    for (unsigned c = 0; c < (unsigned)(X9_WIFI_EMI_SIZE / X9_EMI_WIN); c++) {
        volatile uint32_t *va = (volatile uint32_t *)ml_io_map_unmappable(
            (vm_offset_t)(X9_WIFI_EMI_PA + (uint64_t)c * X9_EMI_WIN), X9_EMI_WIN, 0x6u /* VM_WIMG_WCOMB, arm memory_types.h */);
        if (!va) continue;
        mapped++;
        for (unsigned j = 0; j < X9_EMI_WIN / X9_EMI_BLK; j++) {
            unsigned b = c * (X9_EMI_WIN / X9_EMI_BLK) + j;
            const volatile uint32_t *w = va + (size_t)j * (X9_EMI_BLK / 4);
            uint32_t h = 0x811c9dc5u, any = 0;
            for (unsigned k = 0; k < X9_EMI_BLK / 4; k++) {
                uint32_t v = w[k];
                any |= v;
                h = (h ^ v) * 0x01000193u;
            }
            out[b] = h;
            nz += any != 0;
            if (report && x9_emi_have_before && h != x9_emi_sum_before[b] && ++changed <= 12) {
                unsigned first = 0;
                while (first < X9_EMI_BLK / 4 && w[first] == 0) first++;
                if (first > X9_EMI_BLK / 4 - 8) first = X9_EMI_BLK / 4 - 8;
                IOLog("X9 WIFI EMI %s: block +%06x changed; first nonzero +%06x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    when, b * X9_EMI_BLK, b * X9_EMI_BLK + first * 4, w[first], w[first + 1], w[first + 2], w[first + 3],
                    w[first + 4], w[first + 5], w[first + 6], w[first + 7]);
            }
            if (!report && b == 0) {
                IOLog("X9 WIFI EMI: first words %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3]);
            }
        }
        ml_io_unmap((vm_offset_t)va, X9_EMI_WIN);
    }
    if (report) {
        IOLog("X9 WIFI EMI %s: %u of %u 64K blocks changed since setup, %u nonzero, %u/12 windows mapped\n", when,
            changed, X9_EMI_NBLK, nz, mapped);
    } else {
        x9_emi_have_before = mapped == X9_WIFI_EMI_SIZE / X9_EMI_WIN;
        IOLog("X9 WIFI EMI: %u of %u 64K blocks nonzero as LK left it, %u/12 windows mapped\n", nz, X9_EMI_NBLK, mapped);
    }
}

/* 76.txt: after WIFI_START the RAM code sets N9_DONE (conn_on_misc 2, ep cfg 0x48c b29:28 = 2) but never INIT_DONE
 * and never answers. Look for firmware assert/exception text in the EMI (coredump/log areas). */
static void
x9_emi_find_text(const char *when)
{
    static const char *const pat[] = { "ASSERT", "assert", "xception", "Exception", "panic", "FATAL", "fail" };
    unsigned hits = 0;
    for (unsigned c = 0; c < (unsigned)(X9_WIFI_EMI_SIZE / X9_EMI_WIN) && hits < 16; c++) {
        const volatile uint8_t *va = (const volatile uint8_t *)ml_io_map_unmappable(
            (vm_offset_t)(X9_WIFI_EMI_PA + (uint64_t)c * X9_EMI_WIN), X9_EMI_WIN, 0x6u);
        if (!va) continue;
        for (uint32_t o = 0; o + 8 < X9_EMI_WIN && hits < 16; o++) {
            uint8_t b0 = va[o];
            for (unsigned k = 0; k < sizeof pat / sizeof pat[0]; k++) {
                const char *q = pat[k];
                if (b0 != (uint8_t)q[0]) continue;
                unsigned j = 1;
                while (q[j] && o + j < X9_EMI_WIN && va[o + j] == (uint8_t)q[j]) j++;
                if (q[j]) continue;
                char line[121];
                uint32_t st = o >= 40 ? o - 40 : 0;
                unsigned n = 0;
                for (uint32_t z = st; z < X9_EMI_WIN && n < 120; z++, n++) {
                    uint8_t ch = va[z];
                    line[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
                }
                line[n] = 0;
                IOLog("X9 WIFI EMI %s: text '%s' at +%06x: %s\n", when, q, c * X9_EMI_WIN + o, line);
                hits++;
                o += 80;
                break;
            }
        }
        ml_io_unmap((vm_offset_t)va, X9_EMI_WIN);
    }
    IOLog("X9 WIFI EMI %s: %u text hits\n", when, hits);
}

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
    unsigned wait_ms = (cid == 0x07 || cid == 0x02) ? 10000 : 3000;
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
/* RAM-mode unified command (gen4m asicConnac3xFillUniCmdTxdInfo, struct CONNAC3X_WIFI_UNI_CMD): 32-byte TXD (same
 * DW0/DW1 as init commands: len | PKT_FT CMD 2 << 23, HF COMMAND 1 << 14), then u16 u2Length (= total - 32), u16
 * u2CID, rsv, ucPktTypeID 0xA0, ucFragNum, ucSeqNum, u16 checksum, ucS2DIndex 0, ucOption (b1 UNI_CMD, b2 SET,
 * b0 ACK), 4 rsv -> payload at 48. Events (RX7): 32-byte RXD + WIFI_UNI_EVENT {u16 len incl. this 12-byte header,
 * u16 type, EID, seq, option (b1 UNI_EVENT, b2 unsolicited), rsv, ext EID, rsv2[2], s2d} -> data at 44.
 * Waits for an event with eid == want (and the command's seq unless it is unsolicited), logging and skipping others.
 * Returns the event length (bytes in out) or -1. */
static int
x9_wd_unicmd(x9_wdma *d, uint16_t cid, uint8_t opt, const void *payload, unsigned plen, uint8_t want, uint8_t *out,
    unsigned cap, unsigned wait_ms)
{
    uint8_t *c = d->va + X9_B_CMD;
    unsigned len = 48 + plen;
    if (len > 0x1000) return -1;
    memset(c, 0, 48);
    uint32_t dw0 = (len & 0xffffu) | (2u << 23), dw1 = 1u << 14;
    memcpy(c + 0, &dw0, 4);
    memcpy(c + 4, &dw1, 4);
    uint16_t l2 = (uint16_t)(len - 32);
    memcpy(c + 32, &l2, 2);
    memcpy(c + 34, &cid, 2);
    c[37] = 0xa0;
    uint8_t seq = ++d->seq;
    c[39] = seq;
    c[43] = opt;
    memcpy(c + 48, payload, plen);
    if (!x9_wd_tx(d, 15, X9_B_CMD, len)) {
        IOLog("X9 WIFI UNI: cid 0x%04x seq %u not consumed\n", cid, seq);
        return -1;
    }
    if (!want) {
        return 0;                            /* send only */
    }
    uint64_t tw = mach_absolute_time();
    for (unsigned k = 0; k < 32; k++) {
        unsigned left = x9_ms_since(tw) >= wait_ms ? 0 : wait_ms - (unsigned)x9_ms_since(tw);
        int n = x9_wd_rx(d, left ? left : 1, out, cap);
        if (n < 0) {
            IOLog("X9 WIFI UNI: cid 0x%04x seq %u: no eid 0x%02x in %u ms\n", cid, seq, want, wait_ms);
            return -1;
        }
        uint16_t elen = 0, etype = 0;
        memcpy(&elen, out + 32, 2);
        memcpy(&etype, out + 34, 2);
        uint8_t eid = out[36], eseq = out[37], eopt = out[38];
        IOLog("X9 WIFI UNI: cid 0x%04x seq %u -> rx len=%d evt len=%u type=%04x eid=0x%02x seq=%u opt=%02x ext=%02x after %llu ms data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            cid, seq, n, elen, etype, eid, eseq, eopt, out[40], x9_ms_since(tw), out[44], out[45], out[46], out[47],
            out[48], out[49], out[50], out[51]);
        if (eid == want && (eseq == seq || (eopt & 4))) {
            return n;
        }
    }
    return -1;
}

/* gen4m nicUniCmdNicCapability / wlanQueryNicCapabilityV2: UNI_CMD_ID_CHIP_CONFIG 0x0E, query (opt UNI_CMD b1),
 * payload {4 rsv, TLV tag UNI_CMD_CHIP_CONFIG_TAG_NIC_CAPABILITY 3, len 4}; answer UNI_EVENT_ID_CHIP_CAPABILITY 0x43:
 * {u16 u2TotalElementNum, 2 pad} + TLVs {u16 tag, u16 len incl. 4-byte header} (TAG_CAP_*: 0 TX_RESOURCE,
 * 1 EFUSE ADDR, 5 HW_VERSION, 6 SW_VERSION, 7 MAC_ADDR, 8 PHY_CAP, 9 MAC_CAP, 0x14 ADIE_VERSION, ...). */
static bool
x9_wifi_nic_cap(x9_wdma *d)
{
    static uint8_t ev[2048];
    uint8_t pl[8] = { 0, 0, 0, 0, 3, 0, 4, 0 };
    int n = x9_wd_unicmd(d, 0x0e, 0x02, pl, sizeof pl, 0x43, ev, sizeof ev, 3000);
    if (n < 48) {
        IOLog("X9 WIFI CAP: NIC capability query FAILED\n");
        return false;
    }
    uint16_t elen, num;
    memcpy(&elen, ev + 32, 2);
    memcpy(&num, ev + 44, 2);
    unsigned end = 32 + (unsigned)elen;
    if (end > (unsigned)n) end = (unsigned)n;
    IOLog("X9 WIFI CAP: NIC capability event %u B, %u elements\n", elen, num);
    unsigned off = 48, shown = 0;
    while (off + 4 <= end && shown < 48) {
        uint16_t tag, tl;
        memcpy(&tag, ev + off, 2);
        memcpy(&tl, ev + off + 2, 2);
        if (tl < 4 || off + tl > end) {
            IOLog("X9 WIFI CAP: bad TLV tag %u len %u at %u\n", tag, tl, off);
            break;
        }
        const uint8_t *v = ev + off + 4;
        unsigned vl = tl - 4u;
        if (tag == 7 && vl >= 6) {
            IOLog("X9 WIFI CAP: tag 7 MAC_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n", v[0], v[1], v[2], v[3], v[4], v[5]);
        } else {
            uint32_t w[6] = {};
            memcpy(w, v, vl < sizeof w ? vl : sizeof w);
            IOLog("X9 WIFI CAP: tag 0x%02x len %u: %08x %08x %08x %08x %08x %08x\n", tag, vl, w[0], w[1], w[2], w[3],
                w[4], w[5]);
        }
        off += tl;
        shown++;
    }
    return true;
}

/* ---------------- real radio (x9_wifi_real=1): keeps the MT6653 up and serves scans ----------------
 * RX7 without waiting (no "no event" diagnostics). */
static int
x9_wd_rx7_try(x9_wdma *d, uint8_t *out, unsigned cap)
{
    unsigned i = d->r7;
    volatile uint32_t *r = (volatile uint32_t *)(void *)(d->va + X9_D_RX7 + i * 16);
    x9_wd_sync(d, X9_D_RX7 + (i & ~3u) * 16, 64);
    if (!(r[1] & (1u << 31))) {
        return -1;
    }
    return x9_wd_rx(d, 1, out, cap);
}

/* HW RX ring 4 (= gen4m RX_RING_DATA0, mt6653RxRingSwIdx2HwIdx): 32 x 2 KiB in the LK wifi pool after the first
 * 64 KiB. Ring regs WFDMA 0x500 + 4*0x10: base, cnt | addr[35:32] << 16, cidx, didx. */
#define X9_R4_N 32u
#define X9_R4_PA (X9_WIFI_RSV_PA + 0x10000ULL)
#define X9_R4_SIZE 0x11000u
static uint8_t *x9_r4_va;
static unsigned x9_r4_i;
static void
x9_r4_desc(unsigned i)
{
    volatile uint32_t *r = (volatile uint32_t *)(void *)(x9_r4_va + i * 16);
    uint64_t b = X9_R4_PA + 0x1000 + i * X9_BUF;
    r[0] = (uint32_t)b;
    r[1] = X9_BUF << 16;
    r[2] = (uint32_t)(b >> 32) & 0xf;
    r[3] = 0;
}
static bool
x9_r4_setup(x9_wdma *d)
{
    if (!x9_r4_va) {
        x9_r4_va = (uint8_t *)ml_io_map_wcomb((vm_offset_t)X9_R4_PA, X9_R4_SIZE);
    }
    if (!x9_r4_va) {
        IOLog("X9 WIFI RADIO: RX4 map failed\n");
        return false;
    }
    memset(x9_r4_va, 0, 0x1000);
    for (unsigned i = 0; i < X9_R4_N; i++) x9_r4_desc(i);
    __asm__ volatile ("dsb sy" ::: "memory");
    x9_wd_wr(d, 0x280, 1u << 4);                                  /* RST_DRX_PTR ring 4 */
    x9_wd_wr(d, 0x540, (uint32_t)X9_R4_PA);
    x9_wd_wr(d, 0x544, X9_R4_N | (uint32_t)((X9_R4_PA >> 32) & 0xf) << 16);
    x9_wd_wr(d, 0x548, X9_R4_N - 1);
    x9_r4_i = 0;
    IOLog("X9 WIFI RADIO: RX4 ring %08x/%08x cidx=%u didx=%u\n", x9_wd_rd(d, 0x540), x9_wd_rd(d, 0x544),
        x9_wd_rd(d, 0x548), x9_wd_rd(d, 0x54c));
    return true;
}
/* Next RX4 packet or -1. */
static int
x9_r4_try(x9_wdma *d, const uint8_t **pkt)
{
    (void)d;
    unsigned i = x9_r4_i;
    volatile uint32_t *r = (volatile uint32_t *)(void *)(x9_r4_va + i * 16);
    __asm__ volatile ("dsb sy" ::: "memory");
    if (!(r[1] & (1u << 31))) return -1;
    unsigned len = (r[1] >> 16) & 0x3fff;
    *pkt = x9_r4_va + 0x1000 + i * X9_BUF;
    return (int)(len > X9_BUF ? X9_BUF : len);
}
static void
x9_r4_done(x9_wdma *d)
{
    unsigned i = x9_r4_i;
    x9_r4_desc(i);
    __asm__ volatile ("dsb sy" ::: "memory");
    x9_wd_wr(d, 0x548, i);
    x9_r4_i = (i + 1) % X9_R4_N;
}

/* Find an 802.11 beacon / probe response inside an RX packet (RXD length varies with the RXD groups, so search
 * the first 160 bytes for a mgmt header whose fixed fields and SSID element are consistent) and add it. */
static unsigned x9_scan_frames, x9_scan_bcn;
/* rev252bw: probe responses (fc 0x50) apart from beacons. A probe response answers the firmware's probe request, so
 * any at all proves our radio transmits on air (86.txt: Auth got MPDU_ERROR, no ACK, 15 tries). */
static unsigned x9_scan_presp;
static uint8_t x9_scan_presp_da[6];
static void
x9_scan_take(const uint8_t *p, unsigned n)
{
    x9_scan_frames++;
    for (unsigned o = 16; o + 24 + 12 + 2 <= n && o <= 160; o += 2) {
        uint8_t fc = p[o];
        if ((fc != 0x80 && fc != 0x50) || p[o + 1] != 0) continue;
        const uint8_t *body = p + o + 24;
        unsigned blen = n - o - 24;
        if (body[12] != 0 || body[13] > 32 || 14u + body[13] > blen) continue;   /* SSID element first */
        uint16_t bint, caps;
        memcpy(&bint, body + 8, 2);
        memcpy(&caps, body + 10, 2);
        if (bint == 0 || bint > 10000) continue;
        x9_scan_bcn++;
        if (fc == 0x50) { if (!x9_scan_presp) memcpy(x9_scan_presp_da, p + o + 4, 6); x9_scan_presp++; }
        const uint8_t *bssid = p + o + 16;
        unsigned ielen = blen - 12;
        uint8_t ch = 0;
        for (unsigned q = 0; q + 2 <= ielen; ) {
            uint8_t id = body[12 + q], l = body[12 + q + 1];
            if (q + 2u + l > ielen) break;
            if (id == 3 && l >= 1) ch = body[12 + q + 2];
            q += 2u + l;
        }
        using namespace x9wifi::fake;
        IOLockLock(lock);
        unsigned k = 0;
        while (k < rcount && memcmp(rbss[k].bssid, bssid, 6)) k++;
        if (k == rcount && rcount < 32) rcount++;
        if (k < rcount) {
            x9wifi::bss_info &b = rbss[k];
            memset(&b, 0, sizeof b);
            memcpy(b.bssid, bssid, 6);
            b.ssid_len = body[13];
            memcpy(b.ssid, body + 14, b.ssid_len);
            b.freq_mhz = ch == 0 ? 0 : ch <= 14 ? (ch == 14 ? 2484u : 2407u + 5u * ch) : 5000u + 5u * ch;
            b.signal_dbm = -55;          /* RCPI not decoded yet */
            b.caps = caps;
            b.beacon_int = bint;
            b.ie_len = (uint16_t)(ielen < sizeof b.ie ? ielen : sizeof b.ie);
            memcpy(b.ie, body + 12, b.ie_len);
        }
        IOLockUnlock(lock);
        return;
    }
}

/* rev252bz: the device NVRAM (TX power / calibration / front-end settings) as gen4m wlanLoadManufactureData sends
 * it on MT6653 (is_support_nvram_fragment): 37 fragments (generated table), each UNI_CMD_ID_NVRAM_SETTINGS 0x2E
 * {4 pad, tag FRAGMENT 0 len 1504, CMD_NVRAM_FRAGMENT 1500 B = fragment bytes then zeros}. We never sent it before:
 * 86/87 showed TX attempted (TX_DONE MPDU_ERROR x15) but never heard (no ACK, no probe responses). ACK requested
 * so each fragment's CMD_RESULT is visible. Boot-arg x9_wifi_nvram=0 skips it. */
static void
x9_radio_nvram(x9_wdma *d)
{
#if X9_WIFI_NVRAM_PRESENT
    int on = 1;
    PE_parse_boot_argn("x9_wifi_nvram", &on, sizeof(on));
    if (!on) { IOLog("X9 WIFI NVRAM: skipped (x9_wifi_nvram=0)\n"); return; }
    static uint8_t pl[4 + 4 + 1500];
    static uint8_t ev[512];
    const unsigned count = sizeof x9_wifi_nvram_frag / sizeof x9_wifi_nvram_frag[0];
    unsigned answered = 0, bad = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned off = x9_wifi_nvram_frag[i][0], len = x9_wifi_nvram_frag[i][1];
        if (off + len > sizeof x9_wifi_nvram || len > 1500) { bad++; continue; }
        memset(pl, 0, sizeof pl);
        pl[4] = 0; pl[6] = (uint8_t)((4 + 1500) & 0xff); pl[7] = (uint8_t)((4 + 1500) >> 8);   /* tag 0, len 1504 */
        memcpy(pl + 8, x9_wifi_nvram + off, len);
        int n = x9_wd_unicmd(d, 0x2e, 0x07, pl, sizeof pl, 0x01, ev, sizeof ev, 1000);
        if (n > 0) answered++;
        if (n <= 0 || i < 2 || i + 1 == count)
            IOLog("X9 WIFI NVRAM: fragment %u/%u off %u len %u id %u -> %s (result %02x %02x %02x %02x %02x %02x %02x %02x)\n",
                  i + 1, count, off, len, x9_wifi_nvram_frag[i][2], n > 0 ? "answered" : "NO CMD_RESULT",
                  n > 0 ? ev[44] : 0, n > 0 ? ev[45] : 0, n > 0 ? ev[46] : 0, n > 0 ? ev[47] : 0,
                  n > 0 ? ev[48] : 0, n > 0 ? ev[49] : 0, n > 0 ? ev[50] : 0, n > 0 ? ev[51] : 0);
    }
    IOLog("X9 WIFI NVRAM: %u/%u fragments answered, %u bad, %u B, sha256 %.16s\n", answered, count, bad,
          (unsigned)sizeof x9_wifi_nvram, X9_WIFI_NVRAM_SHA256);
#else
    (void)d;
    IOLog("X9 WIFI NVRAM: not embedded (no .local/x9/wifi-nvram/WIFI at build time)\n");
#endif
}

/* gen4m nicUniCmdBssActivateCtrl for wlan0 as Android logs it: DevInfo[OMAC=0, DBDC=255], BssInfo0[DBDC=255,
 * OMAC=0, WMM=3, ConnType=65537, ConnState=1, BcIdx=0, PhyMode=0, DTIM=0] + MLD tag (OwnMldId 32, link 0). */
static void
x9_radio_activate(x9_wdma *d, const uint8_t mac[6])
{
    static uint8_t ev[2048];
    uint8_t dev[16] = { 0, 0xff, 0, 0,  0, 0, 12, 0,  1, 0 };
    memcpy(dev + 10, mac, 6);
    int n = x9_wd_unicmd(d, 0x01, 0x07, dev, sizeof dev, 0x01, ev, sizeof ev, 1000);
    IOLog("X9 WIFI RADIO: DEVINFO active -> %s\n", n > 0 ? "answered" : "no CMD_RESULT");
    uint8_t bss[4 + 32 + 20] = {};
    uint8_t *b = bss + 4;                                          /* BASIC tag */
    b[0] = 0; b[2] = 32;
    b[4] = 1; b[5] = 0; b[6] = 0; b[7] = 0xff;                     /* active, omac, hw bss, dbdc auto */
    uint32_t ct = 65537;
    memcpy(b + 8, &ct, 4);
    b[12] = 1; b[13] = 3;                                          /* conn state, wmm */
    uint16_t sta = 0xfe;
    memcpy(b + 26, &sta, 2);                                       /* u2StaRecIdxOfAP (24 dtim, 25 phymode) */
    uint8_t *m = bss + 36;                                         /* MLD tag 0x1a */
    m[0] = 0x1a; m[2] = 20;
    m[4] = 0xff; m[5] = 32;
    memcpy(m + 6, mac, 6);
    m[12] = 0xff; m[13] = 0xff;
    n = x9_wd_unicmd(d, 0x02, 0x07, bss, sizeof bss, 0x01, ev, sizeof ev, 1000);
    IOLog("X9 WIFI RADIO: BSSINFO basic+mld -> %s\n", n > 0 ? "answered" : "no CMD_RESULT");
}

/* gen4m nicUniCmdScanReqV2 with Android's values: active, SSID type 1 (wildcard, no list), channel type 4 with
 * 1-13, 36-64, 149-165, func mask 0x11, source 1, zero BSSID, random MAC in MISC. */
static void
x9_radio_scan_req(x9_wdma *d, uint8_t seq, const uint8_t mac[6])
{
    static const uint8_t chs[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 36, 40, 44, 48, 52, 56, 60, 64, 149, 153,
        157, 161, 165 };
    uint8_t pl[4 + 20 + 16 + 8 + 64 + 16] = {};
    unsigned at = 0;
    pl[0] = seq; pl[1] = 0;                                        /* seq, bss 0 */
    at = 4;
    uint8_t *t = pl + at;                                          /* SCAN_REQ tag 1, 20 B */
    t[0] = 1; t[2] = 20;
    t[4] = 1; t[5] = 0; t[6] = 0x11; t[7] = 1;
    at += 20;
    t = pl + at;                                                   /* BSSID tag 11, 16 B */
    t[0] = 11; t[2] = 16;
    t[11] = 10; t[13] = 10;                                        /* match ssid / short ssid ind = OOB max */
    at += 16;
    t = pl + at;                                                   /* CHANNEL tag 12 */
    unsigned nch = sizeof chs;
    unsigned clen = 8 + ((nch * 2 + 3) & ~3u);
    t[0] = 12; t[2] = (uint8_t)clen;
    t[4] = 4; t[5] = (uint8_t)nch;
    for (unsigned k = 0; k < nch; k++) {
        t[8 + 2 * k] = chs[k] <= 14 ? 1 : 2;                       /* BAND_2G4 1, BAND_5G 2 */
        t[8 + 2 * k + 1] = chs[k];
    }
    at += clen;
    t = pl + at;                                                   /* MISC tag 14, 16 B */
    t[0] = 14; t[2] = 16;
    memcpy(t + 4, mac, 6);
    at += 16;
    uint8_t ev[64];
    x9_wd_unicmd(d, 0x16, 0x06, pl, at, 0, ev, sizeof ev, 0);
}

/* ---------------- open-network join (RUC-Web): CH privilege -> STAREC -> Auth -> Assoc -> connected ----------------
 * All mgmt frames go on the command ring (TX15) as a 32-byte connac3 TXD + the 802.11 frame, packet format COMMAND,
 * header format 802.11 normal. Bit layout from include/nic/nic_connac3x_tx.h (DS_VER 3600). widx/own-mac from BSS0. */
#define X9_JOIN_WIDX 1u
#define X9_JOIN_OMAC 0u
#define X9_JOIN_PID 0x20u                                       /* mgmt PID as Android's first one (PID[32]) */

static unsigned
x9_txd_mgmt(uint8_t *buf, unsigned framelen, unsigned machdr, uint8_t subtype)
{
    uint32_t dw[8] = {};
    unsigned total = 32 + framelen;
    unsigned etoff = (16 + machdr) >> 1;                       /* pse(16)+machdr, in words */
    dw[0] = (total & 0xffff) | ((etoff & 0x7f) << 16) | (2u << 23) | (17u << 25);  /* COMMAND, MCU_Q1|ALTX */
    dw[1] = (X9_JOIN_WIDX & 0xfff) | (2u << 14) | (((machdr >> 1) & 0x1f) << 16) | (0u << 21) | (X9_JOIN_OMAC << 25)
        | (1u << 31);                                          /* FIXED_RATE (Android: Rate mode 1 = MANUAL_DESC) */
    dw[2] = (subtype & 0xf) | (0u << 4) | (0x7fu << 16);       /* subtype, type MGMT 0, life-time max */
    dw[3] = (15u << 11);                                       /* remaining tx count 15, HW assigns SN */
    dw[5] = X9_JOIN_PID | (1u << 9);                           /* u2DW5_0: PID, TX_STATUS_TO_MCU (-> EVENT_TX_DONE) */
    /* DW6: msdu_count 1, DA_SRC_SEL, fixed-rate index 1 = FIXED_RATE_INDEX_OFDM_6M (Android RateIdx=1), FR_BW 8.
     * VALID_TXD_ARRIVAL_TIME (b28) left clear: MT6653 builds CFG_WIFI_TX_FIXED_RATE_NO_VTA=1. */
    dw[6] = (1u << 10) | (1u << 2) | (1u << 16) | (8u << 22);
    memcpy(buf, dw, 32);
    return total;
}

/* Send a prebuilt 802.11 mgmt frame (at X9_B_FW+32 already, with TXD space at X9_B_FW) on TX15. */
static bool
x9_tx_mgmt(x9_wdma *d, uint8_t subtype, const uint8_t *frame, unsigned framelen)
{
    uint8_t *b = d->va + X9_B_FW;
    unsigned total = x9_txd_mgmt(b, framelen, 24, subtype);
    memcpy(b + 32, frame, framelen);
    return x9_wd_tx(d, 15, X9_B_FW, total);
}

/* Locate an 802.11 mgmt frame of a given subtype addressed to us inside an RX7/RX4 packet. Returns body offset or 0. */
static unsigned
x9_find_mgmt(const uint8_t *p, unsigned n, uint8_t subtype, const uint8_t mac[6])
{
    uint8_t fc0 = (uint8_t)(subtype << 4);                     /* type mgmt (00), subtype in high nibble */
    for (unsigned o = 16; o + 24 <= n && o <= 196; o += 2) {
        if (p[o] != fc0 || (p[o + 1] & 0x0c)) continue;
        if (memcmp(p + o + 4, mac, 6) != 0) continue;          /* DA = us */
        return o;
    }
    return 0;
}

/* Wait up to ms for a mgmt frame of subtype from the AP on RX7 (events ignored) or RX4; copies body to out. */
static int
x9_wait_mgmt(x9_wdma *d, uint8_t subtype, const uint8_t mac[6], uint8_t *out, unsigned cap, unsigned ms)
{
    static uint8_t ev[2048];
    uint64_t t0 = mach_absolute_time();
    /* rev252br: what arrived while waiting, logged on timeout (diagnostic only) */
    unsigned n4 = 0, n7 = 0, ne = 0;
    uint8_t lastEid = 0;
    while (x9_ms_since(t0) < ms) {
        const uint8_t *pk;
        int pn;
        bool any = false;
        while ((pn = x9_r4_try(d, &pk)) >= 0) {
            any = true;
            n4++;
            unsigned o = x9_find_mgmt(pk, (unsigned)pn, subtype, mac);
            if (o) { unsigned l = (unsigned)pn - o; if (l > cap) l = cap; memcpy(out, pk + o, l); x9_r4_done(d); return (int)l; }
            x9_r4_done(d);
        }
        while ((pn = x9_wd_rx7_try(d, ev, sizeof ev)) >= 0) {
            any = true;
            uint32_t rxd0;
            memcpy(&rxd0, ev, 4);
            if (n7 + ne < 6)                                   /* rev252bt: what each rx7 packet is (TX_DONE eid 0x0f?) */
                IOLog("X9 WIFI WAIT: rx7 len=%d pkt_type=%u eid=0x%02x opt=%02x bytes36=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                      pn, rxd0 >> 27, ev[36], ev[38], ev[36], ev[37], ev[38], ev[39], ev[40], ev[41], ev[42], ev[43]);
            if ((rxd0 >> 27) == 7 && ev[36] == 0x2d && pn >= 80) {
                /* rev252bu: UNI_EVENT_ID_STATUS_TO_HOST {4 pad} + UNI_EVENT_TX_DONE tag at 48 (gen4m nic_uni_cmd_event.h).
                 * status = TX_RESULT_*: 0 success (ACKed), 1 life timeout, 2 RTS error, 3 MPDU error (no ACK),
                 * 4 aging timeout, 5 flushed, 7 unspecified, 33 dropped in FW, 35 inactive BSS. */
                static const char *const res[] = { "SUCCESS", "LIFE_TIMEOUT", "RTS_ERROR", "MPDU_ERROR(no ACK)",
                    "AGING_TIMEOUT", "FLUSHED", "BIP_ERROR", "UNSPECIFIED" };
                const uint8_t *g = ev + 48;
                uint16_t gtag, sn, rate;
                uint32_t delay, ts;
                memcpy(&gtag, g, 2);
                memcpy(&sn, g + 6, 2);
                memcpy(&rate, g + 10, 2);
                memcpy(&delay, g + 20, 4);
                memcpy(&ts, g + 24, 4);
                IOLog("X9 WIFI TXDONE: tag=%u pid=0x%02x status=%u (%s) sn=%u widx=%u tx_count=%u rate=0x%04x flag=%02x "
                      "rsp_rate=%u rate_idx=%u bw=%u txpwr=%u flush=%u delay_us=%u tsf=%u\n", gtag, g[4], g[5],
                      g[5] < 8 ? res[g[5]] : (g[5] == 33 ? "DROPPED_IN_FW" : (g[5] == 35 ? "INACTIVE_BSS" : "other")),
                      sn, g[8], g[9], rate, g[12], g[14], g[15], g[16], g[17], g[18], delay * 32, ts);
                ne++; lastEid = ev[36];
                continue;
            }
            if ((rxd0 >> 27) == 7 && (ev[36] == 0x01 || ev[36] == 0x0e || ev[36] == 0x0f)) { ne++; lastEid = ev[36]; continue; }  /* event, not a frame */
            n7++;
            unsigned o = x9_find_mgmt(ev, (unsigned)pn, subtype, mac);
            if (o) { unsigned l = (unsigned)pn - o; if (l > cap) l = cap; memcpy(out, ev + o, l); return (int)l; }
        }
        if (!any) IOSleep(5);
    }
    IOLog("X9 WIFI WAIT: subtype %u timed out after %u ms: rx4 packets %u, rx7 frames %u, rx7 events %u (last eid 0x%02x)\n",
          subtype, ms, n4, n7, ne, lastEid);
    return -1;
}

static bool
x9_radio_ch_privilege(x9_wdma *d, const x9wifi::bss_info *b, uint8_t token)
{
    static uint8_t ev[512];
    uint8_t ch = b->freq_mhz >= 4900 ? (uint8_t)((b->freq_mhz - 5000) / 5) : (uint8_t)((b->freq_mhz - 2407) / 5);
    uint8_t band = b->freq_mhz >= 4900 ? 2 : 1;                /* eRfBand: 1=2.4G, 2=5G (ChReq b=1 for 2.4 in Android) */
    uint8_t pl[4 + 24] = {};
    uint8_t *t = pl + 4;
    t[0] = 0; t[2] = 24;                                       /* tag CH_PRIVILEGE_REQ, len 24 */
    t[4] = 0;                                                  /* ucBssIndex 0 */
    t[5] = token;
    t[6] = ch;                                                /* ucPrimaryChannel */
    t[7] = 0;                                                  /* ucRfSco */
    t[8] = band;                                              /* ucRfBand */
    t[9] = 0;                                                  /* ucRfChannelWidth 20 MHz */
    t[10] = ch;                                               /* seg1 */
    t[11] = 0;                                                /* seg2 */
    t[12] = 0; t[13] = ch; t[14] = 0;                         /* fromAP width/seg1/seg2 */
    t[15] = 0;                                                /* ucReqType CH_REQ_TYPE_JOIN */
    uint32_t iv = 4000;
    memcpy(t + 16, &iv, 4);                                   /* u4MaxInterval */
    t[20] = 0xff;                                             /* ucDBDCBand = AUTO */
    int n = x9_wd_unicmd(d, 0x27, 0x06, pl, sizeof pl, 0x27, ev, sizeof ev, 2000);
    if (n < 68) { IOLog("X9 WIFI JOIN: no CH grant (event len %d)\n", n); return false; }
    /* Event data at 44 = UNI_EVENT_CNM {4 pad}; tag at 48 = UNI_EVENT_CNM_CH_PRIVILEGE_GRANT {u16 tag, u16 len, bss,
     * token, status, primary ch, sco, band, width, seg1, seg2, req type, dbdc, rsv, u32 interval}. rev252bs and
     * earlier read the padding (ev[46]/ev[47]), so their "status=0 ch=0" said nothing about the grant. */
    const uint8_t *g = ev + 48;
    uint16_t gtag, glen;
    memcpy(&gtag, g, 2);
    memcpy(&glen, g + 2, 2);
    uint32_t giv;
    memcpy(&giv, g + 16, 4);
    IOLog("X9 WIFI JOIN: CH grant tag=%u len=%u bss=%u token=%u status=%u ch=%u sco=%u band=%u width=%u s1=%u s2=%u "
          "req=%u dbdc=%u interval=%u ms (asked ch=%u band=%u)\n", gtag, glen, g[4], g[5], g[6], g[7], g[8], g[9], g[10],
          g[11], g[12], g[13], g[14], giv, ch, band);
    if (gtag != 0 || g[6] != 0 || g[7] != ch)                 /* warn only for now: keep the Auth attempt for data */
        IOLog("X9 WIFI JOIN: CH grant NOT as asked (status/channel/tag); continuing anyway\n");
    return true;
}

/* Release the channel privilege, as Android after every join attempt (aisFsmReleaseCh -> nicUniCmdChAbortPrivilege:
 * "bss=2,token=3,dbdc=255"). No event is expected. */
static void
x9_radio_ch_abort(x9_wdma *d, uint8_t token)
{
    uint8_t pl[4 + 12] = {};
    uint8_t *t = pl + 4;
    t[0] = 1; t[2] = 12;                                      /* tag CH_PRIVILEGE_ABORT, len 12 */
    t[4] = 0;                                                 /* ucBssIndex 0 */
    t[5] = token;
    t[6] = 0xff;                                              /* ucDBDCBand AUTO */
    int n = x9_wd_unicmd(d, 0x27, 0x06, pl, sizeof pl, 0, nullptr, 0, 0);
    IOLog("X9 WIFI JOIN: CH privilege token %u abort %s\n", token, n == 0 ? "sent" : "not consumed");
}

/* AP rate sets from its beacon IEs (Supported Rates 1, Extended 50) as gen4m RATE_SET bits: 1M b0, 2M b1, 5.5M b2,
 * 11M b3, (22M b4, 33M b5 unused), 6M b6 .. 54M b13; basic = rates with the high bit. rev252bs sent basic 0x00ff and
 * desired 0x0fff, which set the PBCC 22M/33M bits and claimed 9M as basic. */
static void
x9_ap_rates(const x9wifi::bss_info *b, uint16_t *op, uint16_t *basic, uint8_t *dtim)
{
    static const uint8_t unit[14] = { 2, 4, 11, 22, 44, 66, 12, 18, 24, 36, 48, 72, 96, 108 };
    *op = 0; *basic = 0; *dtim = 1;
    for (unsigned o = 0; o + 2 <= b->ie_len;) {
        uint8_t id = b->ie[o], l = b->ie[o + 1];
        if (o + 2u + l > b->ie_len) break;
        if (id == 1 || id == 50) {
            for (unsigned k = 0; k < l; k++) {
                uint8_t r = b->ie[o + 2 + k];
                for (unsigned s = 0; s < 14; s++) {
                    if (s == 4 || s == 5 || unit[s] != (r & 0x7f)) continue;
                    *op |= (uint16_t)(1u << s);
                    if (r & 0x80) *basic |= (uint16_t)(1u << s);
                }
            }
        } else if (id == 5 && l >= 2 && b->ie[o + 3]) {
            *dtim = b->ie[o + 3];                              /* TIM: DTIM count, DTIM period */
        }
        o += 2u + l;
    }
    if (!*op) { *op = 0x3fcf; *basic = 0x000f; }               /* no IEs kept: 11b/g set, 11b basic */
    if (!*basic) *basic = *op & 0x000f ? (uint16_t)(*op & 0x000f) : (uint16_t)(*op & 0x0040);
}

/* gen4m nicUpdateBssEx -> nicUniCmdSetBssInfo (UNI_CMD_ID_BSSINFO 0x02) with the tags it always sends for an AIS
 * BSS: BASIC, RLM + PROTECT + IFS_TIME, RATE, SEC, QBSS, MLD. Android sends it before the channel request
 * (ConnState 1, BSSID 0, eBand 1) and after the association (ConnState 0, BSSID = AP, StaRecIdxOfAP = AP widx). */
static void
x9_radio_bssinfo(x9_wdma *d, const uint8_t mac[6], const x9wifi::bss_info *b, bool connected)
{
    static uint8_t ev[512];
    uint8_t ch = b->freq_mhz >= 4900 ? (uint8_t)((b->freq_mhz - 5000) / 5) : (uint8_t)((b->freq_mhz - 2407) / 5);
    bool g24 = b->freq_mhz < 4900;
    uint16_t op, basic;
    uint8_t dtim;
    x9_ap_rates(b, &op, &basic, &dtim);
    uint8_t pl[4 + 32 + 16 + 8 + 20 + 16 + 8 + 8 + 20] = {};
    unsigned at = 4;                                           /* ucBssInfoIdx 0 + 3 pad */
    uint8_t *t = pl + at;                                      /* BASIC tag 0, len 32 */
    t[0] = 0; t[2] = 32;
    t[4] = 1; t[5] = X9_JOIN_OMAC; t[6] = X9_JOIN_OMAC; t[7] = 0xff;   /* active, omac, hw bss, dbdc auto */
    uint32_t ct = 65537;                                       /* CONNECTION_INFRA_STA */
    memcpy(t + 8, &ct, 4);
    t[12] = connected ? 0 : 1;                                 /* MEDIA_STATE_CONNECTED 0 / DISCONNECTED 1 */
    t[13] = 3;                                                 /* WMM set as at activation */
    if (connected) memcpy(t + 14, b->bssid, 6);
    uint16_t bcmc = 0, bint = b->beacon_int ? b->beacon_int : 100, sta = connected ? X9_JOIN_WIDX : 0xfe;
    memcpy(t + 20, &bcmc, 2);
    memcpy(t + 22, &bint, 2);
    t[24] = dtim;
    t[25] = g24 ? (uint8_t)((1u << 1) | (1u << 2)) : (uint8_t)(1u << 0);   /* WMODE_B|WMODE_G, or WMODE_A */
    memcpy(t + 26, &sta, 2);                                   /* u2StaRecIdxOfAP (a WLAN index) */
    uint16_t nonht = g24 ? 1 : 2;                              /* PHY_TYPE_ERP_INDEX / PHY_TYPE_OFDM_INDEX */
    memcpy(t + 28, &nonht, 2);
    at += 32;
    t = pl + at;                                               /* RLM tag 2, len 16 */
    t[0] = 2; t[2] = 16;
    t[4] = ch; t[5] = ch; t[6] = 0; t[7] = 0;                  /* primary, seg0, seg1, BW_20 */
    t[8] = 1; t[9] = 1;                                        /* tx/rx streams (legacy) */
    t[10] = 0; t[11] = 0;                                      /* HT op info1, SCO none */
    t[12] = g24 ? 1 : 2;                                       /* BAND_2G4 1 / BAND_5G 2 */
    at += 16;
    t = pl + at;                                               /* PROTECT tag 3, len 8: none */
    t[0] = 3; t[2] = 8;
    at += 8;
    t = pl + at;                                               /* IFS_TIME tag 0x17, len 20 */
    t[0] = 0x17; t[2] = 20;
    t[4] = 1;                                                  /* fgSlotValid */
    uint16_t slot = (b->caps & (1u << 10)) ? 9 : 20;           /* short slot time capability */
    memcpy(t + 8, &slot, 2);
    at += 20;
    t = pl + at;                                               /* RATE tag 0x0b, len 16 */
    t[0] = 0x0b; t[2] = 16;
    memcpy(t + 4, &op, 2);
    memcpy(t + 6, &basic, 2);
    at += 16;
    t = pl + at;                                               /* SEC tag 0x10, len 8: open, EncStatus 1 (Android) */
    t[0] = 0x10; t[2] = 8;
    t[4] = 0; t[5] = 1;
    at += 8;
    t = pl + at;                                               /* QBSS tag 0x0f, len 8: no WMM (assoc has no WMM IE) */
    t[0] = 0x0f; t[2] = 8;
    at += 8;
    t = pl + at;                                               /* MLD tag 0x1a, len 20 as at activation */
    t[0] = 0x1a; t[2] = 20;
    t[4] = 0xff; t[5] = 32;
    memcpy(t + 6, mac, 6);
    t[12] = 0xff; t[13] = 0xff;
    at += 20;
    int n = x9_wd_unicmd(d, 0x02, 0x07, pl, at, 0x01, ev, sizeof ev, 1000);
    IOLog("X9 WIFI JOIN: BSSINFO %s ch=%u op_rates=%04x basic=%04x dtim=%u bcn=%u slot=%u -> %s\n",
          connected ? "connected" : "pre-join", ch, op, basic, dtim, bint, slot, n > 0 ? "answered" : "no CMD_RESULT");
}

/* gen4m nicUniCmdUpdateStaRec (UNI_CMD_ID_STAREC_INFO 0x03) tags for a legacy AP: BASIC, STATE, PHY_INFO, RA,
 * BA_OFFLOAD, UAPSD (HT/VHT/HE skipped: no HT tags sent, the peer is used as 11b/g). */
static bool
x9_radio_starec(x9_wdma *d, const x9wifi::bss_info *b, uint8_t staState, uint16_t aid)
{
    static uint8_t ev[512];
    uint8_t rcpi = (uint8_t)((b->signal_dbm + 110) * 2);
    uint16_t op, basic;
    uint8_t dtim;
    x9_ap_rates(b, &op, &basic, &dtim);
    bool g24 = b->freq_mhz < 4900;
    uint8_t pl[8 + 20 + 16 + 12 + 16 + 16 + 8] = {};
    pl[0] = 0;                                                /* ucBssInfoIdx 0 */
    pl[1] = X9_JOIN_WIDX;                                     /* ucWlanIdxL (HnVer at 6 = 0) */
    unsigned at = 8;
    uint8_t *t = pl + at;                                     /* STAREC_BASIC tag 0 */
    t[0] = 0; t[2] = 20;
    uint32_t ct = 0x10002;                                    /* CONNECTION_INFRA_AP = STA_TYPE_AP|NETWORK_INFRA */
    memcpy(t + 4, &ct, 4);
    t[8] = 1;                                                 /* ucConnectionState STATE_CONNECTED (gen4m always) */
    t[9] = 0;                                                 /* ucIsQBSS */
    memcpy(t + 10, &aid, 2);                                  /* u2AID */
    memcpy(t + 12, b->bssid, 6);                              /* aucPeerMacAddr */
    uint16_t ei = 3;                                          /* V2 | NEWSTAREC */
    memcpy(t + 18, &ei, 2);
    at += 20;
    t = pl + at;                                              /* STAREC_STATE tag 7 */
    t[0] = 7; t[2] = 16;
    t[4] = staState;                                          /* ucStaState */
    t[13] = 0;                                                /* ucActionType STA_REC_CMD_ACTION_STA */
    at += 16;
    t = pl + at;                                              /* STAREC_PHY_INFO tag 0x15 */
    t[0] = 0x15; t[2] = 12;
    memcpy(t + 4, &basic, 2);                                 /* u2BSSBasicRateSet */
    t[6] = g24 ? 0x03 : 0x04;                                 /* PHY_TYPE_SET_802_11BG / 11A */
    t[9] = rcpi;                                              /* ucRCPI */
    at += 12;
    t = pl + at;                                              /* STAREC_RA tag 1 */
    t[0] = 1; t[2] = 16;
    memcpy(t + 4, &op, 2);                                    /* u2DesiredNonHTRateSet */
    at += 16;
    t = pl + at;                                              /* STAREC_BA_OFFLOAD tag 0x16: no aggregation */
    t[0] = 0x16; t[2] = 16;
    at += 16;
    t = pl + at;                                              /* STAREC_UAPSD tag 0x24: none */
    t[0] = 0x24; t[2] = 8;
    at += 8;
    int n = x9_wd_unicmd(d, 0x03, 0x07, pl, at, 0x01, ev, sizeof ev, 1000);
    IOLog("X9 WIFI JOIN: STAREC state %u aid %u rates %04x basic %04x rcpi %u -> %s\n", staState, aid, op, basic, rcpi,
          n > 0 ? "answered" : "no CMD_RESULT");
    return true;
}

static bool x9_radio_join_granted(x9_wdma *d, const uint8_t mac[6], const x9wifi::bss_info *b);

static bool
x9_radio_join(x9_wdma *d, const uint8_t mac[6], const x9wifi::bss_info *b)
{
    const uint8_t *ap = b->bssid;
    IOLog("X9 WIFI JOIN: '%.*s' %02x:%02x:%02x:%02x:%02x:%02x %u MHz, own (SA/TA, DEVINFO) %02x:%02x:%02x:%02x:%02x:%02x\n",
        b->ssid_len, (const char *)b->ssid, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5], b->freq_mhz,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    x9_radio_bssinfo(d, mac, b, false);                       /* Android: nicUpdateBssEx before the CH request */
    if (!x9_radio_ch_privilege(d, b, 3)) { x9_radio_ch_abort(d, 3); return false; }
    bool ok = x9_radio_join_granted(d, mac, b);
    x9_radio_ch_abort(d, 3);                                  /* Android releases it after success too */
    return ok;
}

static bool
x9_radio_join_granted(x9_wdma *d, const uint8_t mac[6], const x9wifi::bss_info *b)
{
    static uint8_t rx[512];
    const uint8_t *ap = b->bssid;
    x9_radio_starec(d, b, 0, 0);                              /* STA_STATE_1 */

    /* Auth frame, open system, transaction 1 */
    uint8_t fr[128];
    memset(fr, 0, sizeof fr);
    fr[0] = 0xb0;                                             /* FC: mgmt auth */
    memcpy(fr + 4, ap, 6);                                    /* DA */
    memcpy(fr + 10, mac, 6);                                  /* SA */
    memcpy(fr + 16, ap, 6);                                   /* BSSID */
    /* u2SeqCtrl at 22 = 0 (HW fills); body at 24: algo(0), seq(1), status(0) */
    unsigned fl = 24 + 6;
    int n = -1;
    for (unsigned attempt = 1; attempt <= 3 && n < 30; attempt++) {   /* rev252bu: 3 tries -> 3 TX_DONE results */
        if (!x9_tx_mgmt(d, 0xb, fr, fl)) { IOLog("X9 WIFI JOIN: auth TX stuck\n"); return false; }
        IOLog("X9 WIFI JOIN: auth attempt %u queued on TX15 (see X9 WIFI TXDONE for the air result)\n", attempt);
        n = x9_wait_mgmt(d, 0xb, mac, rx, sizeof rx, 600);
    }
    if (n < 30) { IOLog("X9 WIFI JOIN: no auth response\n"); return false; }
    uint16_t astatus;
    memcpy(&astatus, rx + 24 + 4, 2);
    IOLog("X9 WIFI JOIN: auth resp seq=%u status=%u\n", rx[24 + 2], astatus);
    if (astatus != 0) return false;

    /* Assoc request: cap, listen interval, SSID, rates */
    memset(fr, 0, sizeof fr);
    fr[0] = 0x00;                                             /* FC: mgmt assoc req */
    memcpy(fr + 4, ap, 6);
    memcpy(fr + 10, mac, 6);
    memcpy(fr + 16, ap, 6);
    unsigned o = 24;
    uint16_t cap = 0x0021;                                    /* ESS | short preamble */
    memcpy(fr + o, &cap, 2); o += 2;
    uint16_t li = 3;
    memcpy(fr + o, &li, 2); o += 2;
    fr[o++] = 0; fr[o++] = b->ssid_len; memcpy(fr + o, b->ssid, b->ssid_len); o += b->ssid_len;   /* SSID */
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    fr[o++] = 1; fr[o++] = 8; memcpy(fr + o, rates, 8); o += 8;                                    /* supported rates */
    static const uint8_t erates[4] = { 0x30, 0x48, 0x60, 0x6c };
    fr[o++] = 50; fr[o++] = 4; memcpy(fr + o, erates, 4); o += 4;                                  /* ext rates */
    if (!x9_tx_mgmt(d, 0x0, fr, o)) { IOLog("X9 WIFI JOIN: assoc TX stuck\n"); return false; }
    n = x9_wait_mgmt(d, 0x1, mac, rx, sizeof rx, 1500);       /* assoc response subtype 1 */
    if (n < 6) { IOLog("X9 WIFI JOIN: no assoc response\n"); return false; }
    uint16_t sstatus, aid;
    memcpy(&sstatus, rx + 24 + 2, 2);
    memcpy(&aid, rx + 24 + 4, 2);
    IOLog("X9 WIFI JOIN: assoc resp status=%u aid=%u\n", sstatus, aid & 0x3fff);
    if (sstatus != 0) return false;

    x9_radio_bssinfo(d, mac, b, true);                        /* Android aisUpdateBssInfoForJOIN -> nicUpdateBssEx */
    x9_radio_starec(d, b, 2, (uint16_t)(aid & 0x3fff));       /* STA_STATE_3 = connected */
    IOLog("X9 WIFI JOIN: '%.*s' CONNECTED\n", b->ssid_len, (const char *)b->ssid);
    return true;
}

static void
x9_wifi_radio_loop(x9_wdma *d)
{
    using namespace x9wifi::fake;
    static uint8_t ev[2048];
    uint8_t mac[6] = { 0x02, 0x58, 0x39, 0x57, 0x46, 0x01 };
    if (lock) memcpy(mac, status.mac, 6);
    if (!lock) {
        IOLog("X9 WIFI RADIO: en0/station not started (configd not up yet?); waiting\n");
        for (unsigned k = 0; k < 600 && !lock; k++) IOSleep(100);
        if (!lock) { IOLog("X9 WIFI RADIO: no station; giving up\n"); return; }
        memcpy(mac, status.mac, 6);
    }
    if (!x9_r4_setup(d)) return;
    x9_radio_nvram(d);                                   /* Android: wlanLoadManufactureData before networks */
    x9_radio_activate(d, mac);
    IOLockLock(lock);
    rcount = 0;
    realUp = true;
    IOLockUnlock(lock);
    IOLog("X9 WIFI RADIO: up; MAC %02x:%02x:%02x:%02x:%02x:%02x, serving scans\n", mac[0], mac[1], mac[2], mac[3],
        mac[4], mac[5]);
    uint8_t seq = 0;
    unsigned idle_logs = 0;
    for (;;) {
        IOLockLock(lock);
        if (!scanReq) {
            uint64_t dl;
            clock_interval_to_deadline(200, kMillisecondScale, &dl);
            IOLockSleepDeadline(lock, (event_t)&scanReq, dl, THREAD_UNINT);
        }
        bool go = scanReq;
        bool dojoin = joinReq;
        scanReq = false;
        joinReq = false;
        x9wifi::bss_info jt;
        if (dojoin) memcpy(&jt, &joinTarget, sizeof jt);
        IOLockUnlock(lock);
        if (dojoin) {
            bool ok = x9_radio_join(d, mac, &jt);
            if (!ok) IOLog("X9 WIFI JOIN: failed\n");
            IOLockLock(lock);
            joinResult = ok ? x9wifi::RES_OK : x9wifi::RES_FAILED;
            status.state = ok ? x9wifi::STA_UP : x9wifi::STA_IDLE;
            if (ok) {
                memcpy(status.bssid, jt.bssid, 6); status.ssid_len = jt.ssid_len;
                memcpy(status.ssid, jt.ssid, 32); status.freq_mhz = jt.freq_mhz;
            }
            const x9wifi::sta_events *e = events;
            IOLockUnlock(lock);
            if (e && e->connect_done) e->connect_done(ok ? x9wifi::RES_OK : x9wifi::RES_FAILED);
            continue;
        }
        /* keep the rings moving between scans */
        const uint8_t *pk;
        int pn;
        while ((pn = x9_r4_try(d, &pk)) >= 0) { x9_scan_take(pk, (unsigned)pn); x9_r4_done(d); }
        while ((pn = x9_wd_rx7_try(d, ev, sizeof ev)) >= 0) {
            uint32_t rxd0;
            memcpy(&rxd0, ev, 4);
            if ((rxd0 >> 27) != 7) continue;         /* stray frame between scans */
            if (idle_logs < 40) {
                idle_logs++;
                IOLog("X9 WIFI RADIO: idle event eid=0x%02x opt=%02x len=%d\n", ev[36], ev[38], pn);
            }
        }
        if (!go) continue;
        seq = (uint8_t)(seq + 1 ? seq + 1 : 1);
        IOLockLock(lock);
        rcount = 0;
        IOLockUnlock(lock);
        x9_scan_frames = x9_scan_bcn = x9_scan_presp = 0;
        uint64_t ts = mach_absolute_time();
        x9_radio_scan_req(d, seq, mac);
        bool done = false;
        while (!done && x9_ms_since(ts) < 8000) {
            bool any = false;
            while ((pn = x9_r4_try(d, &pk)) >= 0) { any = true; x9_scan_take(pk, (unsigned)pn); x9_r4_done(d); }
            while ((pn = x9_wd_rx7_try(d, ev, sizeof ev)) >= 0) {
                any = true;
                uint32_t rxd0;
                memcpy(&rxd0, ev, 4);
                /* 79/80.txt: the scan frames come on RX7 with RXD type 7 too and "eid" 0; let the parser decide */
                if ((rxd0 >> 27) != 7 || (ev[36] != 0x0e && ev[36] != 0x01)) {
                    if (x9_scan_frames < 3) {
                        IOLog("X9 WIFI RADIO: rx7 frame type %u len %d: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                            rxd0 >> 27, pn, ((uint32_t *)ev)[0], ((uint32_t *)ev)[1], ((uint32_t *)ev)[2],
                            ((uint32_t *)ev)[3], ((uint32_t *)ev)[4], ((uint32_t *)ev)[5], ((uint32_t *)ev)[6],
                            ((uint32_t *)ev)[7]);
                        for (unsigned o = 32; o < 128 && o + 16 <= (unsigned)pn; o += 16) {
                            IOLog("X9 WIFI RADIO:   +%02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                o, ev[o], ev[o + 1], ev[o + 2], ev[o + 3], ev[o + 4], ev[o + 5], ev[o + 6], ev[o + 7],
                                ev[o + 8], ev[o + 9], ev[o + 10], ev[o + 11], ev[o + 12], ev[o + 13], ev[o + 14], ev[o + 15]);
                        }
                    }
                    x9_scan_take(ev, (unsigned)(pn < (int)sizeof ev ? pn : (int)sizeof ev));
                    continue;
                }
                uint8_t eid = ev[36];
                if (eid == 0x0e) {
                    uint8_t cc = pn >= 52 ? ev[52] : 0;
                    uint32_t bc = 0;
                    if (pn >= 60) memcpy(&bc, ev + 56, 4);
                    IOLog("X9 WIFI RADIO: SCAN_DONE seq %u after %llu ms: channels %u, fw beacons %u\n", ev[44],
                        x9_ms_since(ts), cc, bc);
                    done = true;
                } else {
                    IOLog("X9 WIFI RADIO: scan event eid=0x%02x opt=%02x len=%d %02x %02x %02x %02x\n", eid, ev[38], pn,
                        ev[44], ev[45], ev[46], ev[47]);
                }
            }
            if (!any) IOSleep(5);
        }
        IOLockLock(lock);
        unsigned nfound = rcount;
        if (status.state == x9wifi::STA_SCANNING) status.state = x9wifi::STA_IDLE;
        stats.scans++;
        const x9wifi::sta_events *e = events;
        IOLockUnlock(lock);
        IOLog("X9 WIFI RADIO: scan %u %s in %llu ms: rx4 frames %u, beacons %u (probe responses %u, first DA "
            "%02x:%02x:%02x:%02x:%02x:%02x), networks %u\n", seq, done ? "done" : "TIMED OUT", x9_ms_since(ts),
            x9_scan_frames, x9_scan_bcn, x9_scan_presp, x9_scan_presp_da[0], x9_scan_presp_da[1], x9_scan_presp_da[2],
            x9_scan_presp_da[3], x9_scan_presp_da[4], x9_scan_presp_da[5], nfound);
        for (unsigned k = 0; k < nfound && k < 32; k++) {
            IOLog("X9 WIFI RADIO:   %02x:%02x:%02x:%02x:%02x:%02x %4u MHz caps %04x '%.*s'\n", rbss[k].bssid[0],
                rbss[k].bssid[1], rbss[k].bssid[2], rbss[k].bssid[3], rbss[k].bssid[4], rbss[k].bssid[5],
                rbss[k].freq_mhz, rbss[k].caps, rbss[k].ssid_len, (const char *)rbss[k].ssid);
        }
        if (e && e->scan_done) e->scan_done();
    }
}

/* gen4m wlanImageSectionDownloadStage for a connac tailer image (RAM N9 or PHY): tailer = 36 B common (ucRegionNum at
 * [2]) + 40 B per region {crc, realsize, blocksize, rsv, addr, len, feature, type}; region data back to back from 0.
 * Per region: INIT_CMD_ID_DOWNLOAD_CONFIG 0x01 {addr, len, wlanGetDataMode} + raw data on TX16. wlanGetDataMode:
 * feature b0 ENCRY -> RESET b3 | key b1-2 | ENCRYPTION b0 (+ b6 if feature b4); IMG_DL_IDX_PHY_FW adds
 * WORKING_PDA_OPTION b4; ACK b31. *entry = addr of the region with DOWNLOAD_CONFIG_VALID_RAM_ENTRY (feature b5). */
static bool
x9_wifi_dl_image(x9_wdma *d, const char *tag, const uint8_t *rf, uint32_t rlen, bool phy, uint32_t *entry)
{
    unsigned nreg = rlen >= 36 ? rf[rlen - 36 + 2] : 0;
    bool rok = nreg >= 1 && nreg <= 16 && rlen >= 36 + 40 * nreg;
    uint32_t at = 0, rsent = 0;
    uint64_t tr = mach_absolute_time();
    *entry = 0;
    if (!rok) {
        IOLog("X9 WIFI %s: bad tailer (regions %u)\n", tag, nreg);
        return false;
    }
    IOLog("X9 WIFI %s: %u B, %u regions, version %.10s date %.15s\n", tag, rlen, nreg,
        (const char *)(rf + rlen - 36 + 7), (const char *)(rf + rlen - 36 + 17));
    for (unsigned r = 0; rok && r < nreg; r++) {
        const uint8_t *t = rf + rlen - 36 - 40 * (nreg - r);
        uint32_t addr, len;
        memcpy(&addr, t + 16, 4);
        memcpy(&len, t + 20, 4);
        uint8_t feat = t[24];
        if (feat & 0x20) *entry = addr;
        if ((feat & 0x40) || len == 0) {
            IOLog("X9 WIFI %s: region %u addr=%08x len=%u feat=%02x not downloaded\n", tag, r, addr, len, feat);
            at += len;
            continue;
        }
        if (at > rlen - 36 - 40 * nreg || len > rlen - 36 - 40 * nreg - at) {
            IOLog("X9 WIFI %s: region %u runs past the data\n", tag, r);
            return false;
        }
        if (feat & 0x80) {
            IOLog("X9 WIFI %s: region %u addr=%08x feat=%02x is DL_TO_EMI (not implemented)\n", tag, r, addr, feat);
            return false;
        }
        uint32_t mode = 1u << 31;
        if (feat & 0x01) {
            mode |= (1u << 3) | (feat & 0x06u) | 1u;
            if (feat & 0x10) mode |= 1u << 6;
        }
        if (phy) mode |= 1u << 4;
        uint32_t cfg[3] = { addr, len, mode };
        uint8_t res[4] = {};
        bool c = x9_wd_cmd(d, 0x01, cfg, sizeof cfg, 1, res, sizeof res);
        IOLog("X9 WIFI %s: region %u addr=%08x len=%u feat=%02x mode=%08x DOWNLOAD_CONFIG %s status=%u\n",
            tag, r, addr, len, feat, mode, c ? "answered" : "FAILED", res[0]);
        if (!c || res[0] != 0) return false;
        for (uint32_t o = 0; o < len; o += 2048) {
            unsigned chunk = len - o < 2048 ? len - o : 2048;
            memcpy(d->va + X9_B_FW, rf + at + o, chunk);
            if (!x9_wd_tx(d, 16, X9_B_FW, chunk)) {
                IOLog("X9 WIFI %s: region %u data stuck at %u/%u\n", tag, r, o, len);
                return false;
            }
            rsent += chunk;
        }
        at += len;
    }
    IOLog("X9 WIFI %s: %u B in %llu ms, entry %08x\n", tag, rsent, x9_ms_since(tr), *entry);
    return true;
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

    /* gen4m halHifSwInfoInit -> mt6653SetupMcuEmiAddr runs before any firmware download: CONN_ON_EMI_ADDR
     * 0x20060d1c = EMI base >> 16, MCU_SW_CR 0x7c05b2e0 = EMI size, 0x7c05b2ec/0x7c05b2e4 = EMI2 (misc pool)
     * base >> 16 / size. 71/72.txt: FINISH answers 0x11 at once with section 0 only but hangs once section 1 is
     * in -> guess: the patch starts and uses the EMI we never told it about. Boot-arg x9_wifi_emi=1 (default). */
    if (sweet_x9_arg("x9_wifi_emi", 1) == 1) {
        IOLog("X9 WIFI EMI: before emi_addr=%08x emi_size=%08x emi2_addr=%08x emi2_size=%08x\n",
            bar[0xe0d1c / 4], bar[0x1ab2e0 / 4], bar[0x1ab2ec / 4], bar[0x1ab2e4 / 4]);
        bar[0xe0d1c / 4] = (uint32_t)(X9_WIFI_EMI_PA >> 16);
        bar[0x1ab2e0 / 4] = (uint32_t)X9_WIFI_EMI_SIZE;
        bar[0x1ab2ec / 4] = (uint32_t)(X9_WIFI_MISC_PA >> 16);
        bar[0x1ab2e4 / 4] = X9_WIFI_MISC_EMI2_SIZE;
        __asm__ volatile ("dsb sy" ::: "memory");
        IOLog("X9 WIFI EMI: set emi_addr=%08x emi_size=%08x emi2_addr=%08x emi2_size=%08x (pa %llx size %llx, emi2 %llx)\n",
            bar[0xe0d1c / 4], bar[0x1ab2e0 / 4], bar[0x1ab2ec / 4], bar[0x1ab2e4 / 4], X9_WIFI_EMI_PA,
            X9_WIFI_EMI_SIZE, X9_WIFI_MISC_PA);
        x9_emi_walk("setup", false);
    }
    /* 74.txt: with the EMI set, the chip writes host EMI after PATCH_FINISH (7 blocks changed, +1c8000 = 10ff8a55
     * MT6653_WIFI_OFF_MAGIC_NUM) but still never answers. gen4m wlanCopyPlatCfgToSysram ("should be called before
     * patch download", CFG_SUPPORT_XONVRAM=y in configs/MT6653/mobile/defconfig) fills conninfra sysram
     * CONNAC3X_PLAT_CFG_ADDR 0x7c05b28c (64 B, bar 0x1ab28c): XO NVRAM from offset 0 (Android userspace blob, length
     * unknown here -> not written), connv3_get_plat_config() in the LAST 4 bytes (0x7c05b2c8): u16 ext_32K_ticks
     * (connv3 DT ext-32k-ticks 0x8000), byte2 = 1 (1.2 V 32K) | co_clock<<1 (DT co-clock 0) | uvlo(1)<<2 |
     * rising_uvlo(1)<<4 = 0x15, byte3 0 -> LE word 0x00158000. Boot-args x9_wifi_platcfg=1 (default),
     * x9_wifi_platcfg_val=<word>. */
    {
        IOLog("X9 WIFI PLATCFG before: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
            bar[0x1ab28c / 4], bar[0x1ab290 / 4], bar[0x1ab294 / 4], bar[0x1ab298 / 4], bar[0x1ab29c / 4],
            bar[0x1ab2a0 / 4], bar[0x1ab2a4 / 4], bar[0x1ab2a8 / 4], bar[0x1ab2ac / 4], bar[0x1ab2b0 / 4],
            bar[0x1ab2b4 / 4], bar[0x1ab2b8 / 4], bar[0x1ab2bc / 4], bar[0x1ab2c0 / 4], bar[0x1ab2c4 / 4],
            bar[0x1ab2c8 / 4]);
        /* XO NVRAM = the CONNXO NVRAM record (vendor libwifinvram "WR-BUF:XONV" via /dev/wmtWifi -> gen4m
         * wlanXonvBufHandler -> sysram 0x7c05b28c). The phone's calibrated /mnt/vendor/nvdata/APCFG/APRDEB/CONNXO is
         * not in hand; libcustom_nvram.so stConnxoCfgDefault (27 B) = 19 00 98 f8 c1 a8 + 21 zero bytes.
         * Boot-arg x9_wifi_xonv=1 (default) writes that default (padded to 28 B). */
        if (sweet_x9_arg("x9_wifi_xonv", 1) == 1) {
            /* The phone's calibrated CONNXO (Android pull, kept in .local, not in git) is passed as boot-args
             * x9_wifi_xo0..x9_wifi_xo6 (LE words of the 27-byte record padded to 28); defaults = vendor default. */
            static const uint32_t xonv[7] = { 0xf8980019u, 0x0000a8c1u, 0, 0, 0, 0, 0 };
            static const char *const xoname[7] = { "x9_wifi_xo0", "x9_wifi_xo1", "x9_wifi_xo2", "x9_wifi_xo3",
                "x9_wifi_xo4", "x9_wifi_xo5", "x9_wifi_xo6" };
            for (unsigned k = 0; k < 7; k++) {
                int v = 0;
                bar[0x1ab28c / 4 + k] = PE_parse_boot_argn(xoname[k], &v, sizeof v) ? (uint32_t)v : xonv[k];
            }
            __asm__ volatile ("dsb sy" ::: "memory");
            IOLog("X9 WIFI PLATCFG: XO NVRAM (CONNXO 27 B) written, reads %08x %08x %08x %08x %08x\n",
                bar[0x1ab28c / 4], bar[0x1ab290 / 4], bar[0x1ab294 / 4], bar[0x1ab298 / 4], bar[0x1ab29c / 4]);
        }
        if (sweet_x9_arg("x9_wifi_platcfg", 1) == 1) {
            uint32_t pv = (uint32_t)sweet_x9_arg("x9_wifi_platcfg_val", 0x00158000);
            bar[0x1ab2c8 / 4] = pv;
            __asm__ volatile ("dsb sy" ::: "memory");
            IOLog("X9 WIFI PLATCFG: custom option word 0x7c05b2c8 <- %08x, reads %08x\n", pv, bar[0x1ab2c8 / 4]);
        }
    }

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
    /* 69.txt: ROM answers PATCH_START but never PATCH_FINISH. mt76 (connac2/3 load_patch) takes the patch
     * semaphore first (cid 0x10, payload {1,0,0,0}; event status 0 none, 1 already patched, 2 got it, 3 released)
     * and releases it at the end; gen4m's CFG_ROM_PATCH_NO_SEM_CTRL skips it. Boot-arg x9_wifi_sem=1 (default). */
    bool semheld = false, patched = false;
    if (ok && sweet_x9_arg("x9_wifi_sem", 1) == 1) {
        uint8_t sg[4] = { 1, 0, 0, 0 }, sres[4] = {};
        bool sc = x9_wd_cmd(&d, 0x10, sg, sizeof sg, 4, sres, sizeof sres);
        IOLog("X9 WIFI PATCH: PATCH_SEM_CONTROL get %s status=%u (0 none,1 already patched,2 got sem,3 released)\n",
            sc ? "answered" : "no event", sres[0]);
        semheld = sc && sres[0] == 2;
    }
    uint64_t tp = mach_absolute_time();
    unsigned maxsec = (unsigned)sweet_x9_arg("x9_wifi_nsec", 8);   /* experiment: send only the first N sections */
    for (unsigned s = 0; ok && s < nsec && s < maxsec; s++) {
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
    if (ok && sweet_x9_arg("x9_wifi_aliveprobe", 0) == 1) {
        /* 70.txt: the semaphore get is answered (status 2) but PATCH_FINISH and the later release get nothing, so the
         * ROM stops answering after the data. Probe: is it still alive right after the last data chunk? */
        uint8_t pq[4] = { 1, 0, 0, 0 }, pres[4] = {};
        bool pc = x9_wd_cmd(&d, 0x10, pq, sizeof pq, 4, pres, sizeof pres);
        IOLog("X9 WIFI PATCH: probe after data (sem get) %s status=%u\n", pc ? "ANSWERED = ROM alive" : "NO EVENT = ROM silent", pres[0]);
    }
    if (ok) {
        uint8_t fin[4] = { 0, 0, 0, 0 }, res[4] = {};
        bool c = x9_wd_cmd(&d, 0x07, fin, sizeof fin, 1, res, sizeof res);
        IOLog("X9 WIFI PATCH: PATCH_FINISH %s status=%u cid=%02x; %u B in %llu ms -> %s\n",
            c ? "answered" : "FAILED", res[0], res[1], sent, x9_ms_since(tp),
            (c && res[0] == 0) ? "ROM PATCH DOWNLOADED" : "patch download failed");
        if (sweet_x9_arg("x9_wifi_emi", 1) == 1) x9_emi_walk("after PATCH_FINISH", true);
        patched = c && res[0] == 0;
    }
    if (semheld) {
        uint8_t sr[4] = { 0, 0, 0, 0 }, sres[4] = {};
        bool sc = x9_wd_cmd(&d, 0x10, sr, sizeof sr, 4, sres, sizeof sres);
        IOLog("X9 WIFI PATCH: PATCH_SEM_CONTROL release %s status=%u\n", sc ? "answered" : "no event", sres[0]);
    }
    IOLog("X9 WIFI MCU: after patch rom_sram=%08x romcode_index=%08x conn_on_misc=%08x events=%u\n",
        bar[0x1ab160 / 4], bar[0xc1604 / 4], bar[0xe00f0 / 4], d.events);
    /* RAM code (75.txt: ROM patch downloaded). gen4m wlanConnacFormatDownload(N9): tailer = 36 B common
     * (ucRegionNum at [2]) + 40 B per region before it {crc, realsize, blocksize, rsv, addr, len, feature, type};
     * region data back to back from offset 0. Per region (not FW_FEATURE_NOT_DOWNLOAD b6; DL_TO_EMI b7 not handled):
     * INIT_CMD_ID_DOWNLOAD_CONFIG 0x01 {addr, len, wlanGetDataMode(feature)} + raw data on TX16, then
     * wlanConfigWifiFunc: INIT_CMD_ID_WIFI_START 0x02 {override = START_OVERRIDE_START_ADDRESS if a region has
     * DOWNLOAD_CONFIG_VALID_RAM_ENTRY b5, addr = that region's addr}. Ready (CFG_MTK_WIFI_ON_READ_BY_CFG_SPACE):
     * EP cfg 0x48c bits 29:28 == 3. Skipped vs gen4m: kalSyncTimeToFW, queryPmicInfo, phyAction (cal), XO/connfem.
     * Boot-arg x9_wifi_ram=1 (default). */
    if (patched && sweet_x9_arg("x9_wifi_ram", 1) == 1) {
        /* 2026-11-06 Android dmesg (gen4m DEBUG): after the patch it downloads WIFI_MT6653_PHY_RAM_CODE (4 regions,
         * cid 0x01 x4) + WIFI_START ("FW_START EVT success" in 8 ms), THEN the N9 RAM code (6 regions) + WIFI_START
         * (success after 154 ms), then "Ready bit asserted". wlanDownloadPhyFw: WIFI_START override =
         * START_CRC_CHECK b3 (| START_OVERRIDE b0 if an entry region exists), address 0. Boot-arg x9_wifi_phyfw=1. */
        bool rok = true;
        uint32_t entry = 0;
        if (sweet_x9_arg("x9_wifi_phyfw", 1) == 1) {
            uint32_t pentry = 0;
            rok = x9_wifi_dl_image(&d, "PHY", x9_wifi_phy, x9_wifi_phy_len, true, &pentry);
            if (rok) {
                uint32_t st[2] = { (pentry ? 1u : 0u) | (1u << 3), 0 };
                uint8_t res[4] = {};
                bool c = x9_wd_cmd(&d, 0x02, st, sizeof st, 1, res, sizeof res);
                IOLog("X9 WIFI PHY: WIFI_START override=%08x %s status=%u -> %s\n", st[0], c ? "answered" : "FAILED",
                    res[0], (c && res[0] == 0) ? "PHY STARTED" : "PHY start failed");
                rok = c && res[0] == 0;
            }
        }
        if (rok) {
            rok = x9_wifi_dl_image(&d, "RAM", x9_wifi_ram, x9_wifi_ram_len, false, &entry);
        }
        if (rok) {
            uint32_t st[2] = { entry ? 1u : 0u, entry };
            uint8_t res[4] = {};
            IOLog("X9 WIFI RAM: WIFI_START override=%u addr=%08x\n", st[0], entry);
            bool c = x9_wd_cmd(&d, 0x02, st, sizeof st, 1, res, sizeof res);
            IOLog("X9 WIFI RAM: WIFI_START %s status=%u -> %s\n", c ? "answered" : "FAILED", res[0],
                (c && res[0] == 0) ? "RAM CODE STARTED" : "start failed");
            volatile uint32_t *m = x9_rc_mac;
            volatile uint32_t *e0 = (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)X9_WIFI_EMI_PA, 0x4000, 0x6u);
            uint64_t tw = mach_absolute_time();
            uint32_t last = 0xffffffffu, lastmisc = 0xffffffffu, laste[8] = {}, lastrx[14] = {};
            bool first = true, fwready = false;
            unsigned wait = (unsigned)sweet_x9_arg("x9_wifi_ready_ms", 20000);
            while (m && x9_ms_since(tw) < wait) {
                uint32_t v = x9_pcie_cfg_rd(m, 1, 0, 0x48c), misc = bar[0xe00f0 / 4];
                bool ch = first || v != last || misc != lastmisc;
                uint32_t ew[8] = {}, rx[14];
                for (unsigned k = 0; e0 && k < 8; k++) {
                    ew[k] = e0[k];
                    ch |= ew[k] != laste[k];
                }
                for (unsigned k = 0; k < 14; k++) {
                    rx[k] = x9_wd_rd(&d, 0x50c + k * 0x10);
                    ch |= rx[k] != lastrx[k];
                }
                if (ch) {
                    IOLog("X9 WIFI RAM: +%llu ms ep cfg 0x48c=%08x (sync=%u own=%u) conn_on_misc=%08x rom_sram=%08x emi0=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                        x9_ms_since(tw), v, (v >> 28) & 7, (v >> 14) & 1, misc, bar[0x1ab160 / 4], ew[0], ew[1], ew[2],
                        ew[3], ew[4], ew[5], ew[6], ew[7]);
                    IOLog("X9 WIFI RAM: +%llu ms rx didx 0-13: %u %u %u %u %u %u %u %u %u %u %u %u %u %u int_sta=%08x\n",
                        x9_ms_since(tw), rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7], rx[8], rx[9], rx[10],
                        rx[11], rx[12], rx[13], x9_wd_rd(&d, 0x200));
                    last = v;
                    lastmisc = misc;
                    memcpy(laste, ew, sizeof laste);
                    memcpy(lastrx, rx, sizeof lastrx);
                    first = false;
                }
                if (((v >> 28) & 3) == 3) {
                    IOLog("X9 WIFI RAM: FW READY after %llu ms\n", x9_ms_since(tw));
                    fwready = true;
                    break;
                }
                IOSleep(20);
            }
            if (e0) ml_io_unmap((vm_offset_t)e0, 0x4000);
            if (fwready && sweet_x9_arg("x9_wifi_cap", 1) == 1) {
                x9_wifi_nic_cap(&d);
            }
            if (fwready && sweet_x9_arg("x9_wifi_real", 0) == 1) {
                x9_wifi_radio_loop(&d);              /* does not return while the radio is up */
            }
            if (sweet_x9_arg("x9_wifi_emitext", 0) == 1) x9_emi_find_text("after WIFI_START");
            if (sweet_x9_arg("x9_wifi_emi", 1) == 1) x9_emi_walk("after WIFI_START", true);
        }
        IOLog("X9 WIFI MCU: after RAM rom_sram=%08x romcode_index=%08x conn_on_misc=%08x events=%u\n",
            bar[0x1ab160 / 4], bar[0xc1604 / 4], bar[0xe00f0 / 4], d.events);
    }
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
