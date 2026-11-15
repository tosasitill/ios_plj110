/* Read the shim-qualified PLJ110 v2 layout, no SMC/MMIO. Shared by ALS
 * and pressure; neither service's registration proves firmware readiness. */
_Static_assert(sizeof(struct x9_scp_handoff) == X9_SCP_HANDOFF_BYTES, "SCP ABI");
static int x9_scp_read_layout(struct x9_scp_handoff *out)
{
 DTEntry chosen; const void *p; unsigned size;
 struct x9_scp_handoff h;

 if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
     SecureDTGetProperty(chosen, "x9-scp-handoff", &p, &size) != kSuccess ||
     size != sizeof(h)) return EAGAIN;
 memcpy(&h, p, sizeof(h));
 if (h.version != 2 || h.flags != (X9_SCP_LAYOUT_VERIFIED|X9_SCP_BOOT_VERIFIED) ||
     h.reserve_base != 0xe8000000 || h.reserve_size != 0x18c0000 ||
     !h.dump_size || h.memory[0].size != h.dump_size ||
     h.allocated_size > h.reserve_size ||
     h.mbox_base != 0x1dfb0000 || h.mbox_set != 0x1dfb0100 ||
     h.mbox_clear != 0x1dfb010c || h.mbox_init != 0x1df2a024 ||
     h.sram_base != 0x1d000000 || h.sram_size != 0x400000 || h.region_info_op != 10) return EAGAIN;
 uint32_t end = 0;
 for (unsigned i = 0; i < X9_SCP_MEM_COUNT; i++) {
  uint32_t start = h.memory[i].offset, bytes = h.memory[i].size;
  if (start < end || start > h.allocated_size || bytes > h.allocated_size-start) return EAGAIN;
  end = start+bytes;
 }
 if (end != h.allocated_size || h.memory[2].size != 0x100000 ||
     h.memory[8].size != 0x10000 || h.memory[9].size != 0x2000 || h.memory[10].size != 0x2000) return EAGAIN;
 const unsigned channels[] = {29,30,31,32}, lengths[] = {64,8,28,28};
 for (unsigned i = 0; i < 4; i++) {
  const struct x9_scp_pin *pin = &h.sensor_pin[i];
  if (pin->channel != channels[i] || pin->message_bytes != lengths[i] ||
      pin->pin_index >= 32 || pin->offset_bytes != pin->pin_index*8 ||
      pin->offset_bytes > 256-pin->message_bytes) return EAGAIN;
  for (unsigned j = 0; j < i; j++) {
   const struct x9_scp_pin *other = &h.sensor_pin[j];
   unsigned endj = other->offset_bytes+((other->message_bytes+7)&~7u);
   unsigned endi = pin->offset_bytes+((pin->message_bytes+7)&~7u);
   if (pin->offset_bytes < endj && other->offset_bytes < endi) return EAGAIN;
  }
 }
 for (unsigned i = 0; i < 7; i++) if (h.reserved[i]) return EAGAIN;
 if (h.ready_pin.channel != 22 || h.ready_pin.message_bytes != 4 ||
     h.ready_pin.pin_index >= 32 || h.ready_pin.offset_bytes != h.ready_pin.pin_index*8 ||
     h.mbox0_base != 0x1dfa0000 || h.mbox0_set != 0x1dfa0100 ||
     h.mbox0_clear != 0x1dfa010c || h.mbox0_init != 0x1df2a020 ||
     h.core0_base != 0x1df30000 || h.core1_base != 0x1df40000 ||
     h.scpsys_base != 0x10001000 || h.vlp_base != 0x1c200000 ||
     h.firmware_base != 0xea000000 || h.firmware_size != 0x15a0000) return EAGAIN;
 for (unsigned i = 0; i < 18; i++) if (h.boot_reserved[i]) return EAGAIN;
 *out = h; return 0;
}
