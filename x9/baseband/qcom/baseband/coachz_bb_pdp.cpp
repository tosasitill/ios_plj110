// SPDX-License-Identifier: BSD-2-Clause
// Included by coachz_baseband.cpp. XNU's raw-IP KPI joins the existing IPA MAPv4 backend.
// CommCenter owns WDS and address configuration; this layer never dials or invents addresses.
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
extern "C" {
#include <net/bpf.h>
}
#include <netinet/in.h>
extern "C" {
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
extern struct ipstat ipstat;
extern struct udpstat udpstat;
}

extern "C" int coachz_modem_data_send(uint32_t, const uint8_t *, uint32_t);
#if COACHZ_NATIVE_DPMAIF
// The native owner supplies the immutable route epoch. Return only the actual
// ifnet_input result, not a void handoff or protocol/application acceptance.
extern "C" void coachz_modem_data_receiver_counted(int (*)(uint32_t, const uint8_t *, uint32_t, uint32_t, uint64_t));
#else
extern "C" void coachz_modem_data_receiver(void (*)(uint32_t, const uint8_t *, uint32_t, uint32_t));
#endif
#if COACHZ_NATIVE_DPMAIF
extern "C" int coachz_modem_data_send_epoch(uint32_t,const uint8_t *,uint32_t,uint64_t);
extern "C" uint32_t coachz_modem_data_state(uint32_t, uint32_t *, uint64_t *);
extern "C" uint32_t coachz_modem_data_room(uint32_t);
static const unsigned BB_PDP_MAX_MTU = 3072;
#else
static const unsigned BB_PDP_MAX_MTU = 1492;
#endif

namespace {
struct bb_pdp_if {
  ifnet_t ifp;
  IOLock *drain; // orders dequeue+send between start thread and bridge thread
  mbuf_t head, tail; // EBUSY holdover only; the queue itself is the ifclassq
  uint32_t queued, families, blocked, host_mtu;
  uint64_t tx, rx, dropped, busy, requested;
  uint64_t epoch;
};
static bb_pdp_if bb_pdp_ifaces[CBP_INTERFACES];
static IOLock *bb_pdp_lock;
static uint32_t bb_pdp_initialized;

static errno_t bb_pdp_demux(ifnet_t, mbuf_t m, char *, protocol_family_t *family)
{
  uint8_t first;
  if (mbuf_copydata(m, 0, 1, &first)) return ENOENT;
  *family = first >> 4 == 4 ? PF_INET : first >> 4 == 6 ? PF_INET6 : 0;
  return *family ? 0 : EPROTONOSUPPORT;
}
static errno_t bb_pdp_add_proto(ifnet_t, protocol_family_t f, const ifnet_demux_desc *, uint32_t)
{ return f == PF_INET || f == PF_INET6 ? 0 : EPROTONOSUPPORT; }
static errno_t bb_pdp_del_proto(ifnet_t ifp, protocol_family_t family)
{
  printf("COACHZ pdp: unplumb unit %u family %u\n", ifnet_unit(ifp), family);
  return 0;
}
static errno_t bb_pdp_input(ifnet_t ifp, protocol_family_t f, mbuf_t m, char *)
{
  // proto_input is a separate, possibly asynchronous gate after ifnet_input.
  // Capture metadata before handing over the mbuf; never access it afterwards.
  const uint32_t n = static_cast<uint32_t>(mbuf_pkthdr_len(m));
  const errno_t error = proto_input(f, m);
  if (error) { mbuf_freem(m); ifnet_stat_increment_in(ifp, 0, 0, 1); }
  static uint64_t counts[2];
  const uint64_t seen = __atomic_add_fetch(&counts[error != 0], uint64_t(1), __ATOMIC_RELAXED);
  if (seen <= 4 || !(seen & (seen - 1)))
    printf("X9 CELL PROTO: unit %u family %u bytes %u seen %llu error %d accepted %u; not-socket-delivery\n",
      ifnet_unit(ifp), f, n, seen, error, error == 0);
  return 0;
}
static errno_t bb_pdp_pre_output(ifnet_t, protocol_family_t, mbuf_t *, const sockaddr *, void *, char *, char *)
{ return 0; }  // raw IP: no link header, software checksums, no advertised offloads
// in_domifattach/proto_plumb is used again after CommCenter tears an IP service down.
// Match XNU's utun_attach_proto contract, including EEXIST for an attached protocol.
static errno_t bb_pdp_plumb(ifnet_t ifp, protocol_family_t family)
{
  ifnet_attach_proto_param proto = {};
  proto.input = bb_pdp_input; proto.pre_output = bb_pdp_pre_output;
  const errno_t error = ifnet_attach_protocol(ifp, family, &proto);
  printf("COACHZ pdp: plumb unit %u family %u result %d\n", ifnet_unit(ifp), family, error);
  return error;
}
static errno_t bb_pdp_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
  switch (cmd) {
    case SIOCSIFMTU: {
      const int mtu = static_cast<ifreq *>(data)->ifr_mtu;
#if COACHZ_NATIVE_DPMAIF
      // Host policy owns if_mtu. Only the normal ioctl path changes it, so
      // XNU's caller performs route/nd6/classq/KEV updates. The MD cap is an
      // independent, epoch-scoped TX/RX gate (never a poll-time assignment).
      uint32_t cap=0;uint64_t epoch=0;
      uint32_t families=coachz_modem_data_state(ifnet_unit(ifp)+1,&cap,&epoch);
      if(mtu<((families&2)?1280:68)||mtu>static_cast<int>(BB_PDP_MAX_MTU))return EINVAL;
      uint32_t effective=families&&cap&&cap<static_cast<uint32_t>(mtu)?cap:static_cast<uint32_t>(mtu);
      if((families&2)&&effective<1280)return EINVAL; // cannot represent this dual-family link MTU
      auto *p=static_cast<bb_pdp_if *>(ifnet_softc(ifp));
      if(!p||!p->drain)return ENXIO;
      IOLockLock(p->drain);
      p->host_mtu=static_cast<uint32_t>(mtu); // preserve intent, never poll-restore to the MD default
      errno_t error=ifnet_set_mtu(ifp,effective);
      IOLockUnlock(p->drain);
      return error; // XNU's surrounding ioctl observes the effective change

#else
      if (mtu < 1280 || mtu > static_cast<int>(BB_PDP_MAX_MTU)) return EINVAL;
      return ifnet_set_mtu(ifp, static_cast<uint32_t>(mtu));
#endif
    }
    case SIOCSIFFLAGS: case SIOCSIFADDR: case SIOCSIFDSTADDR: return 0;
    default:
      printf("COACHZ pdp: ioctl 0x%lx unit %u unsupported\n", cmd, ifnet_unit(ifp));
      return EOPNOTSUPP;
  }
}
// Outcome probes do not count as traffic or alter the receiver result. No IP
// contents. First four and powers of two bound hot-path success/error logging.
static void bb_pdp_rx_probe(bb_pdp_if *p,uint32_t mux,uint32_t n,uint32_t version,
    uint64_t epoch,unsigned reason,int error)
{
  static uint64_t counts[7]; uint64_t seen=__atomic_add_fetch(&counts[reason],uint64_t(1),__ATOMIC_RELAXED);
  if(seen>4&&(seen&(seen-1)))return;
  uint32_t families=0,blocked=0,flags=0,mtu=0;uint64_t current_epoch=0;bool present=false;
  if(p&&bb_pdp_lock){
    IOLockLock(bb_pdp_lock);ifnet_t ifp=p->ifp;families=p->families;blocked=p->blocked;current_epoch=p->epoch;
    IOLockUnlock(bb_pdp_lock);present=ifp!=nullptr;
    if(ifp){flags=ifnet_flags(ifp);mtu=ifnet_mtu(ifp);}
  }
  printf("X9 CELL INPUT: reason %u seen %llu mux %u version %u bytes %u epoch %llu/%llu ifp %u flags 0x%x families %u blocked %u MTU %u error %d accepted %u\n",
    reason,seen,mux,version,n,epoch,current_epoch,present,flags,families,blocked,mtu,error,reason==0);
}
static int bb_pdp_receive_error(bb_pdp_if *p, int error)
{
  IOLockLock(bb_pdp_lock); p->dropped++; ifnet_t ifp = p->ifp; IOLockUnlock(bb_pdp_lock);
  if (ifp) ifnet_stat_increment_in(ifp, 0, 0, 1);
  return error;
}
static int bb_pdp_receive_counted(uint32_t mux, const uint8_t *ip, uint32_t n,
    uint32_t version, uint64_t epoch)
{
  if (!bb_pdp_lock || !cbp_mux_ok(mux) || !ip || !n || (version != 4 && version != 6)) {
    bb_pdp_rx_probe(nullptr,mux,n,version,epoch,1,EINVAL);return EINVAL;
  }
  auto *p = &bb_pdp_ifaces[mux - 1u];
  IOLockLock(bb_pdp_lock);
  ifnet_t ifp = p->ifp;
  const bool usable = ifp && (p->families & cbp_family_bit(version)) && !p->blocked
#if COACHZ_NATIVE_DPMAIF
    && epoch && p->epoch == epoch
#endif
    ;
  IOLockUnlock(bb_pdp_lock);
#if !COACHZ_NATIVE_DPMAIF
  (void)epoch;
#endif
  if (!usable || !(ifnet_flags(ifp) & IFF_UP)) {
    bb_pdp_rx_probe(p,mux,n,version,epoch,2,ENETDOWN);return bb_pdp_receive_error(p, ENETDOWN);
  }
  // 236: 1500 B downlink was refused (EMSGSIZE) because CommCenter set if_mtu
  // 1450 from the RA. MTU bounds what we SEND; XNU/Linux/ccmni accept larger
  // inbound packets. Bound RX by the receive buffer (MRU) only.
  if (n > BB_PDP_MAX_MTU) {
    bb_pdp_rx_probe(p,mux,n,version,epoch,3,EMSGSIZE);return bb_pdp_receive_error(p, EMSGSIZE);
  }
  mbuf_t m = nullptr;
  errno_t error = mbuf_allocpacket(MBUF_DONTWAIT, n, nullptr, &m);
  if (error || !m) {
    bb_pdp_rx_probe(p,mux,n,version,epoch,4,error?error:ENOBUFS);return bb_pdp_receive_error(p, error ? error : ENOBUFS);
  }
  error = mbuf_copyback(m, 0, n, ip, MBUF_DONTWAIT);
  if (error) { mbuf_freem(m);bb_pdp_rx_probe(p,mux,n,version,epoch,5,error);return bb_pdp_receive_error(p, error); }
  mbuf_pkthdr_setrcvif(m, ifp);
  mbuf_pkthdr_setheader(m, nullptr);
  bpf_tap_in(ifp, DLT_RAW, m, nullptr, 0);
  ifnet_stat_increment_param inc = {};
  inc.packets_in = 1; inc.bytes_in = n;
  // XNU ifnet_input consumes the mbuf on BOTH success and failure (including
  // a detached interface). Do not double-free its failed input here.
  error = ifnet_input(ifp, m, &inc);
  if (error) { bb_pdp_rx_probe(p,mux,n,version,epoch,6,error);return bb_pdp_receive_error(p, error); }
  IOLockLock(bb_pdp_lock); p->rx++; IOLockUnlock(bb_pdp_lock);
  bb_pdp_rx_probe(p,mux,n,version,epoch,0,0);
  return 0;
}
#if !COACHZ_NATIVE_DPMAIF
static void bb_pdp_receive(uint32_t mux, const uint8_t *ip, uint32_t n, uint32_t version)
{
  (void)bb_pdp_receive_counted(mux, ip, n, version, 0);
}
#endif
static uint32_t bb_pdp_room(uint32_t i)
{
#if COACHZ_NATIVE_DPMAIF
  return coachz_modem_data_room(i + 1u);
#else
  (void)i; return 16; // IPA backend reports EBUSY; holdover covers the rest
#endif
}
static void bb_pdp_free_chain(mbuf_t m)
{
  while (m) { mbuf_t next = mbuf_nextpkt(m); mbuf_setnextpkt(m, nullptr); mbuf_freem(m); m = next; }
}
// Pull packets from the ifclassq (or the EBUSY holdover) only as far as the
// modem path has room. Called from the ifnet start thread and every bridge
// pass, so freed room is used within one poll. One drain at a time keeps
// per-flow order. Never holds bb_lock or bb_pdp_lock across a send.
static void bb_pdp_drain(bb_pdp_if *p)
{
  if (!p->ifp || !p->drain) return;
  const uint32_t i = uint32_t(p - bb_pdp_ifaces);
  IOLockLock(p->drain);
  for (unsigned pass = 0; pass < 8; pass++) {
    IOLockLock(bb_pdp_lock);
    const uint32_t families = p->blocked ? 0 : p->families;
    const uint64_t epoch = p->epoch;
    mbuf_t m = p->head; p->head = p->tail = nullptr; p->queued = 0;
    IOLockUnlock(bb_pdp_lock);
    if (!families || !(ifnet_flags(p->ifp) & IFF_UP)) {
      bb_pdp_free_chain(m); ifnet_purge(p->ifp); break;
    }
    if (!m) {
      const uint32_t room = bb_pdp_room(i);
      uint32_t count = 0;
      if (!room || ifnet_dequeue_multi(p->ifp, room, &m, nullptr, &count, nullptr) || !m) break;
      __atomic_fetch_add(&p->requested, uint64_t(count), __ATOMIC_RELAXED);
    }
    bool stalled = false;
    while (m) {
      mbuf_t next = mbuf_nextpkt(m); mbuf_setnextpkt(m, nullptr);
      uint8_t ip[BB_PDP_MAX_MTU];
      const uint32_t n = static_cast<uint32_t>(mbuf_pkthdr_len(m));
      int error = ENETDOWN;
      if (n && n <= sizeof(ip) && n <= ifnet_mtu(p->ifp) && !mbuf_copydata(m, 0, n, ip) &&
          (families & cbp_family_bit(ip[0] >> 4))) {
#if COACHZ_NATIVE_DPMAIF
        error = coachz_modem_data_send_epoch(i + 1u, ip, n,epoch);
#else
        error = coachz_modem_data_send(i + 1u, ip, n);
#endif
      }
      if (error == EBUSY) {
        // Keep order: this packet and the rest of the chain go back in front.
        mbuf_setnextpkt(m, next);
        unsigned held = 0; mbuf_t last = m;
        for (mbuf_t t = m; t; t = mbuf_nextpkt(t)) { held++; last = t; }
        IOLockLock(bb_pdp_lock);
        const bool same = p->families == families && p->epoch == epoch && !p->blocked && !p->head;
        if (same) { p->head = m; p->tail = last; p->queued = held; p->busy++; }
        IOLockUnlock(bb_pdp_lock);
        if (!same) bb_pdp_free_chain(m); // route changed: never send old-call packets
        stalled = true; break;
      }
      if (!error) { bpf_tap_out(p->ifp, DLT_RAW, m, nullptr, 0); p->tx++; ifnet_stat_increment_out(p->ifp, 1, n, 0); }
      else { p->dropped++; ifnet_stat_increment_out(p->ifp, 0, 0, 1); }
      mbuf_freem(m); m = next;
    }
    if (stalled) break;
  }
  IOLockUnlock(p->drain);
}
static void bb_pdp_start(ifnet_t ifp)
{
  auto *p = static_cast<bb_pdp_if *>(ifnet_softc(ifp));
  if (p) bb_pdp_drain(p);
}
// The helper can be opened before modem READY. Prepare only its traffic-gate
// lock here; do not attach interfaces or announce a usable data transport.
// The board serializes this before starting the bridge worker.
static bool bb_pdp_prepare_helper()
{
  if (!bb_pdp_lock) bb_pdp_lock = IOLockAlloc();
  return bb_pdp_lock != nullptr;
}
static void bb_pdp_init()
{
  if (bb_pdp_initialized) return;
  if (!bb_pdp_prepare_helper()) return;
  const errno_t ipv4 = proto_register_plumber(PF_INET, IFNET_FAMILY_CELLULAR, bb_pdp_plumb, nullptr);
  const errno_t ipv6 = proto_register_plumber(PF_INET6, IFNET_FAMILY_CELLULAR, bb_pdp_plumb, nullptr);
  printf("COACHZ pdp: protocol plumbers IPv4 %d IPv6 %d\n", ipv4, ipv6);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    auto *p = &bb_pdp_ifaces[i];
    ifnet_init_eparams init = {};
    init.ver = IFNET_INIT_CURRENT_VERSION; init.len = sizeof init;
    // Start model: the stack queues in ifclassq (FQ-CoDel) and the driver pulls
    // only what the modem path can take. The legacy output path had to drop
    // with ENOBUFS at 64 queued packets, which XNU TCP answers by cutting cwnd.
    init.flags = IFNET_INIT_NX_NOAUTO;
    init.start = bb_pdp_start; init.output_sched_model = IFNET_SCHED_MODEL_NORMAL;
    init.sndq_maxlen = 512;
    if (!p->drain) p->drain = IOLockAlloc();
    init.name = "pdp_ip"; init.unit = i;
    init.family = IFNET_FAMILY_CELLULAR; init.type = IFT_CELLULAR; init.softc = p;
    init.demux = bb_pdp_demux;
    init.add_proto = bb_pdp_add_proto; init.del_proto = bb_pdp_del_proto; init.ioctl = bb_pdp_ioctl;
    errno_t error = ifnet_allocate_extended(&init, &p->ifp);
    if (!error) {
      ifnet_set_hdrlen(p->ifp, 0); ifnet_set_addrlen(p->ifp, 0);
#if COACHZ_NATIVE_DPMAIF
      ifnet_set_mtu(p->ifp, 1500); // Raw IP has no Qualcomm MAPv4 header tax.
#else
      ifnet_set_mtu(p->ifp, 1492);  // IPA MTU minus MAPv4 headers, as rmnet_vnd_headroom
#endif
      ifnet_set_flags(p->ifp, IFF_POINTOPOINT | IFF_MULTICAST, 0xffff);
      ifnet_set_eflags(p->ifp, IFEF_NOAUTOIPV6LL, IFEF_NOAUTOIPV6LL);
      error = ifnet_attach(p->ifp, nullptr);
      if (error) { ifnet_release(p->ifp); p->ifp = nullptr; }
    }
    if (!error) {
      error = bb_pdp_plumb(p->ifp, PF_INET);
      if (!error || error == EEXIST) error = bb_pdp_plumb(p->ifp, PF_INET6);
      if (!error || error == EEXIST) error = bpf_attach(p->ifp, DLT_RAW, 0, nullptr, nullptr);
    }
    printf("COACHZ pdp: pdp_ip%u mux %u registered result %d (initially down)\n", i, i + 1u, error);
  }
#if COACHZ_NATIVE_DPMAIF
  coachz_modem_data_receiver_counted(bb_pdp_receive_counted);
#else
  coachz_modem_data_receiver(bb_pdp_receive);
#endif
  bb_pdp_initialized = 1;
}
// Wi-Fi thread only; never hold bb_lock while calling the network stack or ringing GSI.
static void bb_pdp_poll()
{
  if (!bb_pdp_initialized) return;
  uint32_t active[CBP_INTERFACES];
  IOLockLock(bb_lock);
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) {
    if (!bb_qmux.client[i].used || bb_qmux.client[i].service != CBD_SVC_WDS ||
        bb_pdp_owners[i] != (1u + bb_qmux.client[i].cid + (uint32_t(bb_qmux.client[i].channel) << 8)))
      bzero(&bb_pdp_clients[i], sizeof bb_pdp_clients[i]);
  }
#if COACHZ_NATIVE_DPMAIF
  const bool ready = true; // Further intersected with confirmed native routes below.
#else
  const bool ready = bb_data.step == CBD_DONE && !bb_data.dpm_error && !bb_data.wda_error &&
    bb_data.llp == CBD_LLP_RAW_IP && bb_data.ul_dap == CBD_DAP_QMAPV4 && bb_data.dl_dap == CBD_DAP_QMAPV4;
#endif
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) active[i] = ready ? cbp_active_families(bb_pdp_clients, CBQ_MAX_CLIENTS, i + 1u) : 0;
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    auto *p = &bb_pdp_ifaces[i];
    if (!p->ifp) continue;
    uint64_t epoch = 0;
#if COACHZ_NATIVE_DPMAIF
    uint32_t mtu = 0;
    // Never acquire the native route lock while holding bb_lock/pdp_lock.
    active[i] &= coachz_modem_data_state(i + 1u, &mtu, &epoch);
    // No ifnet_set_mtu here: poll must not overwrite carrier/host policy or
    // bypass the surrounding XNU ioctl notifications. MD caps gate DPMAIF.
#endif
    // Serialize with bb_pdp_drain: no chain taken under the old epoch may be
    // sent after the route changes.
    if (p->drain) IOLockLock(p->drain);
    IOLockLock(bb_pdp_lock);
    const bool changed = p->families != active[i] || p->epoch != epoch;
    mbuf_t stale = nullptr;
    unsigned discarded = 0;
    if (changed) {
      stale = p->head; discarded = p->queued; p->head = p->tail = nullptr; p->queued = 0;
      p->dropped += discarded;
    }
    p->epoch = epoch;
    p->families = active[i];
    IOLockUnlock(bb_pdp_lock);
    while (stale) { mbuf_t next = mbuf_nextpkt(stale); mbuf_setnextpkt(stale, nullptr); mbuf_freem(stale); stale = next; }
    if (discarded) ifnet_stat_increment_out(p->ifp, 0, 0, discarded);
    if (changed) {
      ifnet_set_flags(p->ifp, active[i] ? IFF_RUNNING : 0, IFF_RUNNING);
      printf("COACHZ pdp: pdp_ip%u active families %u\n", i, active[i]);
    }
    if (changed) ifnet_purge(p->ifp); // new call epoch or down: queued packets are not its traffic
    if (p->drain) IOLockUnlock(p->drain);
    bb_pdp_drain(p);
  }
}
static IOReturn bb_pdp_block(uint32_t unit, uint32_t blocked)
{
  if (unit >= CBP_INTERFACES || !bb_pdp_lock) return kIOReturnBadArgument;
  IOLockLock(bb_pdp_lock); auto *p = &bb_pdp_ifaces[unit]; p->blocked = !!blocked;
  mbuf_t stale = nullptr; unsigned discarded = 0;
  if (blocked) { stale = p->head; discarded = p->queued; p->head = p->tail = nullptr; p->queued = 0; p->dropped += discarded; }
  IOLockUnlock(bb_pdp_lock);
  while (stale) { mbuf_t next = mbuf_nextpkt(stale); mbuf_setnextpkt(stale, nullptr); mbuf_freem(stale); stale = next; }
  if (discarded && p->ifp) ifnet_stat_increment_out(p->ifp, 0, 0, discarded);
  if (blocked && p->ifp) ifnet_purge(p->ifp);
  return kIOReturnSuccess;
}
static void bb_pdp_report(unsigned second)
{
  if (!bb_pdp_lock) return;
  IOLockLock(bb_pdp_lock);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    const auto &p = bb_pdp_ifaces[i];
    printf("COACHZ pdp: %u s unit %u families %u blocked %u queued %u tx %llu rx %llu drop %llu busy %llu\n",
      second, i, p.families, p.blocked, p.queued, p.tx, p.rx, p.dropped, p.busy);
    if (p.ifp && (p.requested || p.rx)) printf("COACHZ pdp stack: %u s unit %u requested %llu flags 0x%x mtu %u\n",
      second, i, p.requested, ifnet_flags(p.ifp), ifnet_mtu(p.ifp));
  }
  IOLockUnlock(bb_pdp_lock);
  printf("COACHZ pdp IP: %u s in %u delivered %u checksum %u short %u badlen %u noroute %u UDP in %u out %u checksum %u noport %u full %u\n",
    second, ipstat.ips_total, ipstat.ips_delivered, ipstat.ips_badsum, ipstat.ips_tooshort, ipstat.ips_badlen, ipstat.ips_noroute,
    udpstat.udps_ipackets, udpstat.udps_opackets, udpstat.udps_badsum, udpstat.udps_noport, udpstat.udps_fullsock);
}
}
