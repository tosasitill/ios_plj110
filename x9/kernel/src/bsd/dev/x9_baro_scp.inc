/* OPPO sensor/2.0 sensorhub v2 host protocol, independently implemented.
 * Firmware owns the sensors and I2C; this is not an AP I2C driver.
 * SHARE_BUFFER RX: magic + crc32 + header12 + length4 + data + tail magic.
 * sensor_comm_notify: header4 + crc8 + padding3 + value[5] (28 bytes).
 * source: sensor_comm.{h,c}, share_memory.{h,c}, sensor_list.c, tiny_crc*.h.
 * This decoder deliberately strengthens the vendor's && reply checks to
 * require EVERY transaction identity field to match, before changing gain. */
static uint8_t baro_crc8(const uint8_t *p, unsigned n)
{
 uint8_t crc = 0;
 for (unsigned i = 0; i < n; i++) {
  crc ^= p[i];
  for (unsigned bit = 0; bit < 8; bit++) crc = (crc>>1) ^ ((crc&1) ? 0x8c : 0);
 }
 return crc;
}
static uint32_t baro_crc32(const uint8_t *p, unsigned n)
{
 uint32_t crc = 0xffffffffu;
 for (unsigned i = 0; i < n; i++) {
  crc ^= p[i];
  for (unsigned bit = 0; bit < 8; bit++) crc = (crc>>1) ^ ((crc&1) ? 0xedb88320u : 0);
 }
 return crc ^ 0xffffffffu;
}

static void baro_put_le32(uint8_t *p, uint32_t value)
{
 for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value>>(8*i));
}

int x9_baro_scp_make_list(void *shared_tx, unsigned bytes,
    void *notification, unsigned nbytes, uint8_t sequence)
{
 uint8_t *p = shared_tx, *n = notification;
 /* Build a host snapshot only. The real worker must copy it into the
  * qualified SHARE_BUFFER_W, perform cache ownership and send OUT31. */
 if (!p || !n || bytes < 4096+28 || bytes > 8192 || nbytes < 28) return EINVAL;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : test_mode || scp_list_pending || scp_pending ? EBUSY : 0;
 if (!error) {
  bzero(p, bytes); bzero(n, 28);
  baro_put_le32(p, 0xdeadbeefu); baro_put_le32(p+bytes-4, 0xdeadbeefu);
  /* Vendor LIST: channel0/command0/subcommand0, TX0, RX capacity4096.
   * Inner header sequence is zero; OUT31 sequence owns the transaction. */
  baro_put_le32(p+16, 4096); baro_put_le32(p+20, 4096);
  baro_put_le32(p+4, baro_crc32(p+8, 16));
  n[0] = sequence; n[2] = 9; n[3] = 12; n[4] = baro_crc8(n, 4);
  memcpy(n+8, p+8, 12);
  /* Quarantine the previous gain/stream BEFORE the worker exposes a new
   * request; a synchronous reply must not race a late pending flag. */
  configure_mtk_locked(0);
  scp_list_sequence = sequence; scp_list_pending = 1;
  scp_list_request_time = mach_absolute_time();
 }
 lck_mtx_unlock(&x9_baro_lock); return error;
}

int x9_baro_scp_configure_list(const void *shared_rx, unsigned bytes,
    const void *notification, unsigned nbytes, uint8_t sequence)
{
 const uint8_t *p = shared_rx, *n = notification, *pressure = NULL;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : test_mode ? EBUSY : !scp_list_pending ? EALREADY : 0;
 if (!error && sequence != scp_list_sequence) error = ESTALE;
 if (!error) {
  uint64_t age; absolutetime_to_nanoseconds(mach_absolute_time()-scp_list_request_time, &age);
  /* OPPO share_buffer_seq_comm waits 100ms, not the ENABLE timeout. */
  if (age > 100000000ULL) {
   scp_list_pending = 0; scp_list_request_time = 0; error = ETIMEDOUT;
  }
 }
 if (error) { lck_mtx_unlock(&x9_baro_lock); return error; }
 /* Platform's SENS_SHARE_BUFFER_R is 0x2000; the list request is <=4096.
  * Use the supplied region size, not any untrusted header-derived pointer. */
 if (!p || !n || nbytes != 28 || bytes < 28 || bytes > 8192) error = EINVAL;
 else if (n[0] != sequence || n[1] != 0 || n[2] != 9 || n[3] != 12 ||
     n[4] != baro_crc8(n, 4)) error = EBADMSG;
 else if (le32(p) != 0xdeadbeefu || le32(p+bytes-4) != 0xdeadbeefu) error = EBADMSG;
 if (!error) {
  const uint8_t *header = p+8, *notified_header = n+8;
  uint32_t length = le32(header+8), capacity = le32(p+20);
  /* channel0 / LIST command0 / subcommand0, sensor INVALID0; no TX data.
   * The share_buffer header's sequence is not sensor_comm sequence: vendor
   * host code initializes it to zero. Correlate the OUT31/IN32 notification. */
  if (header[1] || header[2] || header[3] || le32(header+4) ||
      notified_header[1] || notified_header[2] || notified_header[3] ||
      le32(notified_header+4) || le32(notified_header+8) != length ||
      length > 4096 || length > capacity || capacity > 4096 ||
      length > bytes-28 || length % X9_BARO_MTK_INFO_BYTES) error = EBADMSG;
  else if (le32(p+4) != baro_crc32(header, 16+length)) error = EBADMSG;
  else for (unsigned i = 0; i < length; i += X9_BARO_MTK_INFO_BYTES) {
   const uint8_t *info = p+24+i;
   if (info[0] != 6) continue;
   if (pressure || !le32(info+4)) { error = EBADMSG; break; }
   pressure = info;
  }
  if (!error && !pressure) error = ENODEV;
 }
 /* Complete validation first: a partial/malformed reply cannot install gain
  * or publish a sensor. Unrelated/malformed replies do not consume the real
  * outstanding query, and unsolicited/duplicate LIST cannot reset a stream.
  * A matching valid list without PRESSURE completes with ENODEV, not a
  * defaultsensorlist or a fabricated gain. */
 if (!error) configure_mtk_locked(le32(pressure+4));
 else if (error == ENODEV) { scp_list_pending = 0; scp_list_request_time = 0; }
 lck_mtx_unlock(&x9_baro_lock);
 return error;
}

int x9_baro_scp_make_enable(void *request, unsigned capacity,
    uint8_t sequence, uint64_t interval_ns)
{
 uint8_t *p = request;
 if (!p || capacity < 24 || interval_ns < 20000000ULL || interval_ns > 1000000000ULL)
  return EINVAL;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : test_mode || scp_list_pending || scp_pending ? EBUSY : !mtk_gain ? EAGAIN : 0;
 if (!error) {
  bzero(p, 24); p[0] = sequence; p[1] = 6; p[2] = 1; p[3] = 16;
  p[4] = baro_crc8(p, 4);
  /* sensor_comm_batch: signed64 delay ns, signed64 latency ns (zero).
   * ENABLE carries the batch itself; there is no separate fake batch ACK. */
  for (unsigned i = 0; i < 8; i++) p[8+i] = (uint8_t)(interval_ns >> (8*i));
  scp_sequence = sequence; scp_pending = 1; scp_request_time = mach_absolute_time();
  scp_request_interval_ns = interval_ns;
  scp_enabled = valid = 0;
 }
 lck_mtx_unlock(&x9_baro_lock); return error;
}

int x9_baro_scp_accept_enable(const void *ack, unsigned length)
{
 const uint8_t *p = ack;
 if (!p || length != 8) return EINVAL;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : test_mode ? EBUSY : !scp_pending ? EALREADY : 0;
 if (!error) {
  uint64_t age; absolutetime_to_nanoseconds(mach_absolute_time()-scp_request_time, &age);
  if (age > 1000000000ULL) error = ETIMEDOUT;
 }
 if (!error && (p[0] != scp_sequence || p[1] != 6 || p[2] != 1 ||
     p[4] != baro_crc8(p, 4))) error = EBADMSG;
 if (!error && (int8_t)p[3] < 0) error = EIO;
 if (!error) {
  report_interval_us = (uint32_t)((scp_request_interval_ns+999)/1000);
  scp_pending = 0; scp_enabled = 1; scp_request_time = scp_request_interval_ns = 0;
 }
 else if (error == ETIMEDOUT || error == EIO) {
  scp_pending = scp_enabled = valid = 0; mtk_gain = 0; last_scp_time = 0;
  scp_request_time = 0;
  scp_request_interval_ns = 0;
 }
 /* Duplicate ACKs, unrelated transactions and malformed packets do not
  * revoke an established stream or consume an outstanding ENABLE. The
  * transport must invalidate on its own send/timeout/reset failure. */
 lck_mtx_unlock(&x9_baro_lock); return error;
}

int x9_baro_scp_receive_data(const void *record, unsigned length)
{
 uint32_t pressure; uint64_t scp_time;
 lck_mtx_lock(&x9_baro_lock);
 int error = !active ? ENXIO : test_mode ? EBUSY : !scp_enabled || !mtk_gain ? EAGAIN : 0;
 if (!error) error = decode_mtk(record, length, mtk_gain, &pressure, &scp_time);
 if (!error && scp_time <= last_scp_time) error = EALREADY;
 if (!error) { last_scp_time = scp_time; publish(pressure, scp_time, 0); }
 lck_mtx_unlock(&x9_baro_lock); return error;
}
