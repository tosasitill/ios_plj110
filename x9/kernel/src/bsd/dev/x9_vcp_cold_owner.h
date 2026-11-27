/* Cold-start VCP/MMUP bootstrap owner for x9_vcp_bus: SKELETON, NOT BOUND.
 *
 * This translation unit exists so the owner side of the bus contract
 * (x9_vcp_bus.h) has a designated home whose signatures are compile-checked
 * against the real headers. It is deliberately NOT in recipe.json /
 * link-order.txt and nothing calls x9_vcp_bus_create() with it: the bus
 * header forbids a default provider or success stub, and every callback
 * below currently fails closed with ENOTSUP because its physical
 * prerequisites are not yet established by any code or device evidence.
 * See docs/hevc-cold-owner-todo-20261127.md for the per-callback evidence
 * gaps and the activation checklist. */
#ifndef X9_VCP_COLD_OWNER_H
#define X9_VCP_COLD_OWNER_H
#include "x9_vcp_bus.h"
#ifdef KERNEL
/* Owner template for a future x9_vcp_bus_create() call. power_token is 0
 * here on purpose: the real token is the x9_mm_power pin taken by the
 * activation path at create time; a zero token must never reach create(). */
extern const struct x9_vcp_bus_owner x9_vcp_cold_owner;
#endif
#endif
