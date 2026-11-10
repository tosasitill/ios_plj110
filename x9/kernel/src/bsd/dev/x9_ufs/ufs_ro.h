#pragma once
/* This copy is linked only into the native kernel, never into the shim. */
#define X9U_EXTENT_WRITES 1
#include <stddef.h>
#include <stdint.h>

/* Kernel-only bounce aperture. Each PRD is limited to 256 KiB (UFSHCI);
 * four PRDs permit 1 MiB, while bring-up defaults to one 256 KiB PRD.
 * The shim retains its separate, qualified 64 KiB allocation/layout. */
#define X9U_DATA_OFFSET 0x4000U
#define X9U_DATA_SIZE 0x100000U
#define X9U_DMA_SIZE (X9U_DATA_OFFSET + X9U_DATA_SIZE)
#define X9U_PRD_SIZE 0x40000U
#define X9U_PRD_COUNT (X9U_DATA_SIZE / X9U_PRD_SIZE)
#define X9U_DEFAULT_DATA_SIZE X9U_PRD_SIZE
#define X9U_COMMAND_TIMEOUT_US 1000000U

enum x9u_error {
 X9U_OK=0, X9U_ARGUMENT=-1, X9U_NOT_READY=-2, X9U_BUSY=-3,
 X9U_DMA_UNVERIFIED=-4, X9U_UNSUPPORTED=-5, X9U_TIMEOUT=-6,
 X9U_RESPONSE=-7, X9U_GEOMETRY=-8, X9U_CRC=-9, X9U_NOT_FOUND=-10,
 X9U_DMA_PENDING=-11, X9U_BOUNDS=-12
};
/* DMA sync must include required barriers/cache maintenance. No storage-write
 * or arbitrary CDB interface is exported. All polling is bounded. */
struct x9u_io {
 void *context;
 uint32_t (*read32)(void *,unsigned);
 void (*write32)(void *,unsigned,uint32_t);
 void (*sync)(void *,unsigned,size_t,int); /* int=0 CPU->DMA, 1 DMA->CPU */
 uint64_t (*now_us)(void *);
 void (*delay_us)(void *,unsigned);
 /* Optional native platform guard, called before and after every command.
  * A failure poisons that view; the adapter must poison all shared views too. */
 int (*platform_check)(void *);
};
struct x9u_snapshot {
 uint32_t cap,version,is,ie,hcs,hce,list_lo,list_hi,doorbell,run;
 uint32_t task_doorbell,auto_h8,memcfg;
};
struct x9u_partition;
struct x9u_extent;
struct x9u_host {
 struct x9u_io io;
 uint8_t *dma;
 uint64_t dma_pa;
 struct x9u_snapshot saved;
 uint64_t blocks[3];
 uint32_t block_size[3],data_limit;
 uint32_t last_ocs,last_is;
 uint64_t io_commands,io_read_bytes,io_write_bytes,io_device_us,io_total_us,io_max_device_us;
 uint64_t io_poll_count,io_write_range_steps;
 uint8_t last_response[64],tur_response[64],tag;
 unsigned attached,poisoned;
#if defined(X9U_EXTENT_WRITES)
 const struct x9u_extent *write_map;
 size_t write_count,write_hint;
 uint64_t write_first,write_partition_blocks,write_blocks;
 unsigned write_lun,write_enabled;
#endif
 unsigned sense_key,sense_asc,sense_ascq,sense_valid,sense_from_upiu;
 unsigned failure_phase,tur_failure_phase,tur_ocs,ready_attempts,sense_requests;
 int sense_request_error;
 unsigned ready_unit_attentions,ready_last_category,ready_last_asc,ready_last_ascq;
};
struct x9u_partition { uint64_t first,count; uint8_t guid[16]; };
struct x9u_extent { uint64_t file,part,count; }; /* 4KiB blocks, partition-relative */

void x9u_snapshot(const struct x9u_io *,struct x9u_snapshot *);
int x9u_open(struct x9u_host *,const struct x9u_io *,void *,uint64_t,int);
int x9u_close(struct x9u_host *);
int x9u_device_init(struct x9u_host *,unsigned *stage);
int x9u_inquiry(struct x9u_host *,unsigned,void *,size_t);
int x9u_lun_ready(struct x9u_host *,unsigned);
int x9u_capacity(struct x9u_host *,unsigned);
int x9u_read(struct x9u_host *,unsigned,uint64_t,uint32_t,void *);
int x9u_gpt_find(struct x9u_host *,unsigned,const char *,struct x9u_partition *);
uint32_t x9u_crc32(uint32_t,const void *,size_t);
int x9u_extent_validate(const struct x9u_extent *,size_t,uint64_t,uint64_t);
int x9u_image_read(struct x9u_host *,unsigned,const struct x9u_partition *,
                   const struct x9u_extent *,size_t,uint64_t,uint64_t,void *,size_t);

/* Requires a fully validated, immutable map for this partition/geometry.
 * Selected extent and request bounds are still checked on every read. */
int x9u_image_read_mapped(struct x9u_host *,unsigned,const struct x9u_partition *,
                   const struct x9u_extent *,size_t,uint64_t,uint64_t,void *,size_t);

#if defined(X9U_EXTENT_WRITES)
/* The kernel binds only its validated private map after GPT/header checks.
 * No arbitrary media-write entry point is exported; shim never defines this. */
int x9u_bind_extent_writes(struct x9u_host *,unsigned,const struct x9u_partition *,
                          const struct x9u_extent *,size_t,uint64_t);
int x9u_image_write(struct x9u_host *,uint64_t,const void *,size_t);
int x9u_sync_image(struct x9u_host *);
#endif
