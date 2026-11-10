/* SPDX-License-Identifier: MIT */
#pragma once
/* Included inside NativeMetalObjects.h's C linkage block. */
NmbBuffer *nmb_buffer_new_no_copy_releasing(NmbDevice *,void *,uint64_t,uint64_t,void (*)(void *,void *,uint64_t),void *);
bool nmb_buffer_make_aliasable(NmbBuffer *);
bool nmb_texture_make_aliasable(NmbTexture *);
bool nmb_buffer_is_aliasable(const NmbBuffer *);
bool nmb_texture_is_aliasable(const NmbTexture *);
uint64_t nmb_buffer_heap_offset(const NmbBuffer *);
uint64_t nmb_texture_query(const NmbTexture *,uint32_t);
bool nmb_texture_swizzle(const NmbTexture *,uint64_t out[4]);
NmbTexture *nmb_texture_new_view_swizzle(NmbTexture *,uint64_t,uint64_t,const uint64_t[2],const uint64_t[2],const uint64_t[4]);
double nmb_command_buffer_kernel_time(const NmbCommandBuffer *,bool);
bool nmb_encoder_use_resource(NmbEncoder *,const NmbBuffer *,const NmbTexture *,uint64_t,uint64_t);
bool nmb_encoder_use_heap(NmbEncoder *,const NmbHeap *,uint64_t);
bool nmb_compute_barrier(NmbEncoder *,uint64_t);
bool nmb_compute_dispatch_indirect(NmbEncoder *,const NmbBuffer *,uint64_t,const uint64_t[3]);
bool nmb_compute_stage_in(NmbEncoder *,const uint64_t[6],const NmbBuffer *,uint64_t);
bool nmb_compute_imageblock(NmbEncoder *,uint64_t,uint64_t);
bool nmb_render_store_action(NmbEncoder *,uint32_t,uint32_t,uint64_t,bool);
bool nmb_render_depth_clip(NmbEncoder *,uint64_t);
bool nmb_render_viewports(NmbEncoder *,const double *,size_t);
bool nmb_render_scissors(NmbEncoder *,const uint64_t *,size_t);
bool nmb_blit_texture_hint(NmbEncoder *,const NmbTexture *,uint32_t,uint64_t,uint64_t);

NmbTexture *nmb_texture_new_on_surface(NmbBuffer *,const struct NmbTextureDescriptor *,uint64_t,uint64_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
bool nmb_encoder_sampler_lod(NmbEncoder *,uint32_t,const NmbSampler *,float,float,uint32_t);
bool nmb_render_barrier(NmbEncoder *,uint64_t,uint64_t,uint64_t);
struct NmbBarrierResource {const NmbBuffer *buffer;const NmbTexture *texture;};
bool nmb_render_barrier_resources(NmbEncoder *,const struct NmbBarrierResource *,size_t,uint64_t,uint64_t);
bool nmb_render_indirect(NmbEncoder *,uint64_t,const NmbBuffer *,uint64_t,const NmbBuffer *,uint64_t,uint64_t);
bool nmb_blit_synchronize_resource(NmbEncoder *,const NmbBuffer *,const NmbTexture *);
uint64_t nmb_pipeline_query(const NmbComputePipeline *,const NmbRenderPipeline *,uint32_t,uint64_t,uint64_t,uint64_t);
typedef struct NmbArgumentEncoder NmbArgumentEncoder;
NmbArgumentEncoder *nmb_argument_encoder_new(const NmbFunction *,uint32_t);
void nmb_argument_encoder_release(NmbArgumentEncoder *);
uint64_t nmb_argument_encoder_query(const NmbArgumentEncoder *,bool);
bool nmb_argument_encoder_target(NmbArgumentEncoder *,const NmbBuffer *,uint64_t,uint64_t);
bool nmb_argument_encoder_buffer(const NmbArgumentEncoder *,const NmbBuffer *,uint64_t,uint32_t);
bool nmb_argument_encoder_texture(const NmbArgumentEncoder *,const NmbTexture *,uint32_t);
bool nmb_argument_encoder_sampler(const NmbArgumentEncoder *,const NmbSampler *,uint32_t);
void *nmb_argument_encoder_constant(const NmbArgumentEncoder *,uint32_t);
NmbArgumentEncoder *nmb_argument_encoder_nested(const NmbArgumentEncoder *,uint32_t);
typedef struct NmbFunctionHandle NmbFunctionHandle;
typedef struct NmbVisibleTable NmbVisibleTable;
NmbFunctionHandle *nmb_function_handle(const NmbComputePipeline *,const NmbRenderPipeline *,const NmbFunction *,uint64_t);
size_t nmb_function_handle_name(const NmbFunctionHandle *,char *,size_t);
void nmb_function_handle_release(NmbFunctionHandle *);
NmbVisibleTable *nmb_visible_table_new(const NmbComputePipeline *,const NmbRenderPipeline *,uint64_t,uint64_t);
void nmb_visible_table_release(NmbVisibleTable *);
bool nmb_visible_table_set(const NmbVisibleTable *,const NmbFunctionHandle *,uint32_t);
uint64_t nmb_visible_table_query(const NmbVisibleTable *,uint32_t);
bool nmb_encoder_visible_table(NmbEncoder *,uint32_t,const NmbVisibleTable *,uint32_t);
bool nmb_argument_encoder_pipeline(const NmbArgumentEncoder *,const NmbComputePipeline *,const NmbRenderPipeline *,uint32_t);
bool nmb_argument_encoder_visible_table(const NmbArgumentEncoder *,const NmbVisibleTable *,uint32_t);
bool nmb_function_constant_info(const NmbFunction *,size_t,char *,size_t,uint64_t[3]);
uint64_t nmb_function_query(const NmbFunction *,uint32_t);
void *nmb_pipeline_adding(const NmbComputePipeline *,const NmbRenderPipeline *,const NmbFunction *const *,size_t,const uint32_t *);
bool nmb_blit_buffer_texture_options(NmbEncoder *,const NmbBuffer *,const NmbTexture *,uint64_t,uint64_t,uint64_t,const uint64_t[3],const uint64_t[3],uint64_t,uint64_t,uint64_t,bool);
NmbArgumentEncoder *nmb_argument_encoder_descriptors(const NmbDevice *,const uint64_t *,size_t);

bool nmb_device_heap_size(const NmbDevice *,const struct NmbTextureDescriptor *,uint64_t,uint64_t,uint64_t out[2]);
bool nmb_render_amplification(NmbEncoder *,uint64_t,const uint32_t *);

uint64_t nmb_texture_io_rows(const NmbTexture *,uint64_t height);

typedef struct NmbCounter NmbCounter;
bool nmb_counter_supported(const NmbDevice *);
NmbCounter *nmb_counter_new(const NmbDevice *,size_t);
void nmb_counter_release(NmbCounter *);
bool nmb_counter_resolve(const NmbCounter *,size_t,size_t,uint64_t *);
bool nmb_counter_sample(NmbEncoder *,const NmbCounter *,size_t,bool);
bool nmb_counter_gpu_resolve(NmbEncoder *,const NmbCounter *,size_t,size_t,const NmbBuffer *,uint64_t);
