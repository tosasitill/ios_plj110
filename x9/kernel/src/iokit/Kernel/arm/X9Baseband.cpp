// X9 baseband board adapter for the QcomBaseband iOS-side bridge (x9/baseband/qcom, BSD-2, HoshinoYumeka / chiba233).
//
// The bridge expects a Qualcomm modem behind QRTR. Ours is a MediaTek MD that speaks AT over CCCI (x9_modem*.inc), so this file is
// the "QRTR node" the bridge talks to: it announces QMI services, receives the QMI requests CommCenter/ABM make and answers them.
// Iteration 1 (this file): publish the AppleBaseband services, announce DMS/NAS/UIM/WDS/WDA/DPM, answer every request with
// QMI_ERR_NOT_SUPPORTED and LOG what was asked, so the first iOS run shows exactly which messages the emulation must implement.
//
// One kernel thread owns configure/poll/QRTR delivery, as the bridge requires.
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_types.h>
#include "../../../../../baseband/qcom/baseband/coachz_baseband.cpp"

// Data plane hooks the bridge expects from the board (stage 6: DPMAIF). Until then packets are dropped.
static void (*x9bb_rx)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version);
extern "C" int coachz_modem_data_send(uint32_t mux, const uint8_t *ip, uint32_t length)
{
  (void)mux; (void)ip; (void)length;
  return -1;
}
extern "C" void coachz_modem_data_receiver(void (*receiver)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version))
{
  x9bb_rx = receiver;
}

extern "C" void delay_for_interval(uint32_t interval, uint32_t scale_factor);

namespace {
const uint32_t X9BB_NODE = 1;
const uint32_t X9BB_PORT_BASE = 0x2000;
struct x9bb_service { uint32_t id; const char *name; };
const x9bb_service x9bb_services[] = {
  { 0x01, "WDS" }, { 0x02, "DMS" }, { 0x03, "NAS" }, { 0x0b, "UIM" }, { 0x1a, "WDA" }, { 0x2f, "DPM" },
};
uint32_t x9bb_logged;

const char *x9bb_name(uint32_t port)
{
  for (const x9bb_service &s : x9bb_services)
    if (X9BB_PORT_BASE + s.id == port) return s.name;
  return "?";
}

// QMI SDU on QRTR: flags, txn (2), message id (2), TLV length (2), TLVs. Answer: flags 2, same txn/id, result TLV.
void x9bb_reply_error(uint32_t local_port, uint32_t port, const uint8_t *req, uint32_t len, uint16_t qmi_error)
{
  if (len < 7) return;
  uint8_t r[7 + 7];
  r[0] = 0x02;                    // response
  r[1] = req[1]; r[2] = req[2];   // transaction
  r[3] = req[3]; r[4] = req[4];   // message id
  r[5] = 7; r[6] = 0;             // TLV length
  r[7] = 0x02; r[8] = 4; r[9] = 0;                 // result TLV
  r[10] = 1; r[11] = 0;                            // QMI_RESULT_FAILURE
  r[12] = (uint8_t)qmi_error; r[13] = (uint8_t)(qmi_error >> 8);
  coachz_baseband_qrtr(local_port, X9BB_NODE, port, r, sizeof r);
}

// Runs on the bridge thread (called from coachz_baseband_poll with the bridge lock released).
void x9bb_modem_send(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length)
{
  const uint8_t *m = static_cast<const uint8_t *>(message);
  if (x9bb_logged < 400) {
    x9bb_logged++;
    if (length >= 7)
      printf("X9 BB: QMI request to %s(0x%x) from port 0x%x: flags %u txn %u msg 0x%04x len %u tlvlen %u\n",
             x9bb_name(port), port - X9BB_PORT_BASE, local_port, m[0], m[1] | m[2] << 8, m[3] | m[4] << 8, length, m[5] | m[6] << 8);
    else
      printf("X9 BB: short QMI message to node %u port 0x%x (%u bytes)\n", node, port, length);
  }
  x9bb_reply_error(local_port, port, m, length, 94);   // QMI_ERR_NOT_SUPPORTED
}

void x9bb_del_client(uint32_t local_port) { printf("X9 BB: DEL_CLIENT port 0x%x\n", local_port); }

void x9bb_thread(void *, wait_result_t)
{
  // Wait for IOKit's resource service (the bridge publishes under it).
  for (unsigned i = 0; i < 600 && !IOService::getResourceService(); i++) delay_for_interval(100, 1000 * NSEC_PER_USEC);
  coachz_baseband_configure(x9bb_modem_send, x9bb_del_client);
  for (const x9bb_service &s : x9bb_services)
    coachz_baseband_new_server(s.id, 1, X9BB_NODE, X9BB_PORT_BASE + s.id);
  printf("X9 BB: bridge configured, %u QMI services announced on node %u\n", (unsigned)(sizeof x9bb_services / sizeof x9bb_services[0]), X9BB_NODE);
  for (unsigned second = 0;; ) {
    for (unsigned i = 0; i < 1000; i++) {
      coachz_baseband_poll();
      delay_for_interval(1, 1000 * NSEC_PER_USEC);
    }
    second++;
    if (second % 10 == 0) coachz_baseband_report(second);
  }
}
}

extern "C" void x9_baseband_start(void)
{
  static int started;
  if (started || !coachz_baseband_enabled()) return;
  started = 1;
  thread_t t;
  if (kernel_thread_start(x9bb_thread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}

// Native UFS root initialization calls this before launchd and before starting
// the slow MD worker. Only the PDP helper services/lock are prepared; the QMI
// transport, interfaces and polling thread still start after physical HS2.
extern "C" void x9_baseband_prepare(void)
{
  if (!coachz_baseband_enabled()) return;
  const int result = coachz_baseband_prepare_helpers();
  printf("X9 BB: pre-launchd PDP helper preparation result %d; transport waits for MD HS2\n", result);
}
