// SPDX-License-Identifier: MIT
//! Real runtime operations missing from the Objective-C object adapter.
use super::*;
use metal2vulkan::metal::{DepthClipMode, TextureSwizzle, TextureSwizzleChannels, ResourceUsage, BarrierScope};
use metal2vulkan_runtime::{ResourceRef, StoreActionOptions, IoSurfaceYCbCr};

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_make_aliasable(p:*mut NmbBuffer)->bool {
 guard(false,|| { get(p)?.0.make_aliasable().map_err(e("make buffer aliasable"))?;Ok(true) })
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_make_aliasable(p:*mut NmbTexture)->bool {
 guard(false,|| { get(p)?.0.make_aliasable().map_err(e("make texture aliasable"))?;Ok(true) })
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_is_aliasable(p:*const NmbBuffer)->bool {guard(false,||Ok(get(p)?.0.is_aliasable()))}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_is_aliasable(p:*const NmbTexture)->bool {guard(false,||Ok(get(p)?.0.is_aliasable()))}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_query(p:*const NmbTexture,query:u32)->u64 {
 guard(0,|| {let t=&get(p)?.0;Ok(match query {
  0=>t.heap_offset(),1=>t.parent_relative_level(),2=>t.parent_relative_slice(),
  3=>t.hazard_tracking_mode().raw(),4=>u64::from(t.allow_gpu_optimized_contents()),
  5=>u64::from(t.is_sparse()),6=>t.gpu_resource_id().0,
  _=>return Err("unknown texture property".into()),
 })})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_heap_offset(p:*const NmbBuffer)->u64 {guard(0,||Ok(get(p)?.0.heap_offset()))}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_texture_swizzle(p:*const NmbTexture,out:*mut u64)->bool {
 guard(false,||{if out.is_null(){return Err("null swizzle".into())}let s=get(p)?.0.swizzle();
 unsafe{std::slice::from_raw_parts_mut(out,4)}.copy_from_slice(&[s.red.as_raw() as u64,s.green.as_raw() as u64,s.blue.as_raw() as u64,s.alpha.as_raw() as u64]);Ok(true)})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_texture_new_view_swizzle(p:*mut NmbTexture,format:u64,kind:u64,levels:*const u64,slices:*const u64,swizzle:*const u64)->*mut NmbTexture {
 guard(std::ptr::null_mut(),|| {
 if levels.is_null()||slices.is_null()||swizzle.is_null(){return Err("null view ranges/swizzle".into())}
 let l=unsafe{std::slice::from_raw_parts(levels,2)};let s=unsafe{std::slice::from_raw_parts(slices,2)};let w=unsafe{std::slice::from_raw_parts(swizzle,4)};
 let swizzle=TextureSwizzleChannels::new(raw::<TextureSwizzle>(w[0],"swizzle")?,raw::<TextureSwizzle>(w[1],"swizzle")?,raw::<TextureSwizzle>(w[2],"swizzle")?,raw::<TextureSwizzle>(w[3],"swizzle")?);
 let t=get(p)?.0.new_texture_view_with_swizzle(raw::<PixelFormat>(format,"format")?,raw::<TextureType>(kind,"texture type")?,l[0]..l[0].checked_add(l[1]).ok_or("levels overflow")?,s[0]..s[0].checked_add(s[1]).ok_or("slices overflow")?,swizzle).map_err(e("swizzled texture view"))?;
 Ok(boxed(NmbTexture(t))) })
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_kernel_time(p:*const NmbCommandBuffer,end:bool)->f64 {guard(0.0,||{let b=&get(p)?.0;Ok(if end{b.kernel_end_time()}else{b.kernel_start_time()})})}

fn resource<'a>(buffer:*const NmbBuffer,texture:*const NmbTexture)->Result<ResourceRef<'a>,String>{
 if !buffer.is_null()&&texture.is_null(){Ok(ResourceRef::Buffer(&get(buffer)?.0))}
 else if !texture.is_null()&&buffer.is_null(){Ok(ResourceRef::Texture(&get(texture)?.0))}
 else{Err("one native resource is required".into())}
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_use_resource(p:*mut NmbEncoder,buffer:*const NmbBuffer,texture:*const NmbTexture,usage:u64,stages:u64)->bool {
 guard(false,||{let r=resource(buffer,texture)?;let usage=raw::<ResourceUsage>(usage,"resource usage")?;
 match get_mut(p)?{NmbEncoder::Compute(c)=>c.use_resource(r,usage),NmbEncoder::Render(c)=>c.use_resource(r,usage,raw::<RenderStages>(stages,"render stages")?),NmbEncoder::Blit(_)=>return Err("blit has no resource residency declaration".into())}.map_err(e("use resource"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_use_heap(p:*mut NmbEncoder,heap:*const NmbHeap,stages:u64)->bool {
 guard(false,||{let h=&get(heap)?.0;
 match get_mut(p)?{NmbEncoder::Compute(c)=>c.use_heap(h),NmbEncoder::Render(c)=>c.use_heap(h,raw::<RenderStages>(stages,"render stages")?),NmbEncoder::Blit(_)=>return Err("blit has no heap residency declaration".into())}.map_err(e("use heap"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_barrier(p:*mut NmbEncoder,scope:u64)->bool {
 guard(false,|| {let NmbEncoder::Compute(c)=get_mut(p)? else{return Err("not compute".into())};c.memory_barrier_with_scope(raw::<BarrierScope>(scope,"barrier scope")?);Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_dispatch_indirect(p:*mut NmbEncoder,buffer:*const NmbBuffer,offset:u64,threads:*const u64)->bool {
 guard(false,|| {let NmbEncoder::Compute(c)=get_mut(p)? else{return Err("not compute".into())};
 if threads.is_null(){return Err("null threadgroup size".into())}let t=unsafe{std::slice::from_raw_parts(threads,3)};
 c.dispatch_threadgroups_indirect(&get(buffer)?.0,offset,Size::new(t[0],t[1],t[2])).map_err(e("dispatch indirect"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_stage_in(p:*mut NmbEncoder,words:*const u64,indirect:*const NmbBuffer,offset:u64)->bool {
 guard(false,|| {let NmbEncoder::Compute(c)=get_mut(p)? else{return Err("not compute".into())};
 if indirect.is_null(){c.set_stage_in_region(region(words)?)}else{c.set_stage_in_region_indirect(&get(indirect)?.0,offset)}.map_err(e("stage-in region"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_imageblock(p:*mut NmbEncoder,width:u64,height:u64)->bool {
 guard(false,|| {let NmbEncoder::Compute(c)=get_mut(p)? else{return Err("not compute".into())};c.set_imageblock_size(width,height).map_err(e("imageblock"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_store_action(p:*mut NmbEncoder,which:u32,index:u32,action:u64,options:bool)->bool {
 guard(false,|| {let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};
 if options {let a=StoreActionOptions::from_raw(action).map_err(e("store options"))?;match which{0=>c.set_color_store_action_options(a,index),1=>c.set_depth_store_action_options(a),2=>c.set_stencil_store_action_options(a),_=>return Err("unknown attachment".into())}}
 else {let a=StoreAction::from_raw(action).map_err(e("store action"))?;match which{0=>c.set_color_store_action(a,index),1=>c.set_depth_store_action(a),2=>c.set_stencil_store_action(a),_=>return Err("unknown attachment".into())}}.map_err(e("store action"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_depth_clip(p:*mut NmbEncoder,mode:u64)->bool {
 guard(false,|| {let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};c.set_depth_clip_mode(raw::<DepthClipMode>(mode,"depth clip")?).map_err(e("depth clip"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_viewports(p:*mut NmbEncoder,words:*const f64,count:usize)->bool {
 guard(false,|| {if count>16||(count>0&&words.is_null()){return Err("invalid viewport array".into())}let input=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(words,count*6)}};
 let values:Vec<_>=input.chunks_exact(6).map(|v|Viewport{origin_x:v[0],origin_y:v[1],width:v[2],height:v[3],znear:v[4],zfar:v[5]}).collect();
 let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};c.set_viewports(&values).map_err(e("viewports"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_scissors(p:*mut NmbEncoder,words:*const u64,count:usize)->bool {
 guard(false,|| {if count>16||(count>0&&words.is_null()){return Err("invalid scissor array".into())}let input=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(words,count*4)}};
 let values:Vec<_>=input.chunks_exact(4).map(|v|ScissorRect{x:v[0],y:v[1],width:v[2],height:v[3]}).collect();
 let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};c.set_scissor_rects(&values).map_err(e("scissors"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_texture_hint(p:*mut NmbEncoder,texture:*const NmbTexture,operation:u32,slice:u64,level:u64)->bool {
 guard(false,|| {let NmbEncoder::Blit(c)=get_mut(p)? else{return Err("not blit".into())};let t=&get(texture)?.0;
 match operation{0=>c.synchronize_texture(t,slice,level),1=>c.optimize_contents_for_gpu_access(t),2=>c.optimize_contents_for_gpu_access_slice_level(t,slice,level),3=>c.optimize_contents_for_cpu_access(t),4=>c.optimize_contents_for_cpu_access_slice_level(t,slice,level),_=>return Err("unknown texture operation".into())}.map_err(e("blit texture operation"))?;Ok(true)})
}

/// `newTextureWithDescriptor:iosurface:plane:` over a surface with planes: `offset` and `bytes_per_row` are the asked plane's, and a
/// two-plane surface also gives its OSType, the asked plane, its size and plane 1's offset and rows (`chroma`, 0 and 0 for one plane),
/// which a two-plane texture reads its second plane from.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_on_surface(buffer: *mut NmbBuffer, descriptor: *const NmbTextureDescriptor, offset: u64,
                                             bytes_per_row: u64, pixel_format: u32, plane: u64, width: u64, height: u64,
                                             chroma_offset: u64, chroma_bytes_per_row: u64) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        let buffer = &get(buffer)?.0;
        let mut surface = IoSurfacePlane::new(offset, bytes_per_row, true);
        if chroma_bytes_per_row != 0 {
            surface = surface.with_ycbcr(IoSurfaceYCbCr::new(pixel_format, plane, [width, height], chroma_offset, chroma_bytes_per_row));
        }
        let texture = buffer.new_texture_with_iosurface_plane(&descriptor, surface);
        Ok(boxed(NmbTexture(texture.map_err(e("texture over surface"))?)))
    })
}


#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_sampler_lod(p:*mut NmbEncoder,stage:u32,sampler:*const NmbSampler,min:f32,max:f32,index:u32)->bool {
 guard(false,||{let s=if sampler.is_null(){None}else{Some(&get(sampler)?.0)};
 match get_mut(p)? {
 NmbEncoder::Compute(c) if stage==0=>c.set_sampler_state_with_lod_clamps(s,min,max,index),
 NmbEncoder::Render(c) if stage==1=>c.set_vertex_sampler_states(&[s],Some((&[min],&[max])),index),
 NmbEncoder::Render(c) if stage==2=>c.set_fragment_sampler_states(&[s],Some((&[min],&[max])),index),
 _=>return Err("sampler stage mismatch".into())}.map_err(e("sampler LOD"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_barrier(p:*mut NmbEncoder,scope:u64,after:u64,before:u64)->bool {
 guard(false,||{let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};c.memory_barrier_with_scope(raw::<BarrierScope>(scope,"scope")?,raw::<RenderStages>(after,"after stages")?,raw::<RenderStages>(before,"before stages")?);Ok(true)})
}
#[repr(C)]
pub struct NmbBarrierResource {pub buffer:*const NmbBuffer,pub texture:*const NmbTexture}
/// # Safety
/// `resources` holds `count` pairs of live handles for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_barrier_resources(p:*mut NmbEncoder,resources:*const NmbBarrierResource,count:usize,after:u64,before:u64)->bool {
 guard(false,||{
  if count>65536||(count>0&&resources.is_null()){return Err("invalid barrier resources".into())}
  let input=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(resources,count)}};
  let mut refs=Vec::with_capacity(count);
  for r in input {
   refs.push(match (r.buffer.is_null(),r.texture.is_null()) {
    (false,true)=>ResourceRef::Buffer(&get(r.buffer)?.0),
    (true,false)=>ResourceRef::Texture(&get(r.texture)?.0),
    _=>return Err("one resource per barrier entry required".into()),
   });
  }
  let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};
  c.memory_barrier_with_resources(&refs,raw::<RenderStages>(after,"after stages")?,raw::<RenderStages>(before,"before stages")?).map_err(e("render resource barrier"))?;
  Ok(true)
 })
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_indirect(p:*mut NmbEncoder,primitive:u64,indirect:*const NmbBuffer,offset:u64,indices:*const NmbBuffer,index_type:u64,index_offset:u64)->bool {
 guard(false,||{let NmbEncoder::Render(c)=get_mut(p)? else{return Err("not render".into())};
 let primitive=raw::<PrimitiveType>(primitive,"primitive")?;let indirect=&get(indirect)?.0;
 if indices.is_null(){c.draw_primitives_indirect(primitive,indirect,offset)}else{c.draw_indexed_primitives_indirect(primitive,raw::<IndexType>(index_type,"index type")?,&get(indices)?.0,index_offset,indirect,offset)}.map_err(e("indirect draw"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_synchronize_resource(p:*mut NmbEncoder,buffer:*const NmbBuffer,texture:*const NmbTexture)->bool {
 guard(false,||{let r=resource(buffer,texture)?;let NmbEncoder::Blit(c)=get_mut(p)? else{return Err("not blit".into())};c.synchronize_resource(r).map_err(e("synchronize resource"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_pipeline_query(compute:*const NmbComputePipeline,render:*const NmbRenderPipeline,which:u32,x:u64,y:u64,z:u64)->u64 {
 guard(0,||{if !compute.is_null()&&render.is_null(){let p=&get(compute)?.0;Ok(match which{0=>p.static_threadgroup_memory_length() as u64,1=>u64::from(p.support_indirect_command_buffers()),2=>p.imageblock_memory_length_for_dimensions(Size::new(x,y,z)),3=>p.gpu_resource_id().0,_=>return Err("unknown compute pipeline property".into())})}
 else if !render.is_null()&&compute.is_null(){let p=&get(render)?.0;Ok(match which{0=>p.max_total_threads_per_threadgroup() as u64,1=>u64::from(p.support_indirect_command_buffers()),2=>p.imageblock_memory_length_for_dimensions(Size::new(x,y,z)).map_err(e("pipeline imageblock"))?,3=>p.gpu_resource_id().0,4=>u64::from(p.threadgroup_size_matches_tile_size()),5=>p.imageblock_sample_length().map_err(e("imageblock sample length"))?,_=>return Err("unknown render pipeline property".into())})}else{Err("one pipeline required".into())}})
}

pub struct NmbArgumentEncoder(metal2vulkan_runtime::ArgumentEncoder);
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_new(function:*const NmbFunction,index:u32)->*mut NmbArgumentEncoder {
 guard(std::ptr::null_mut(),||Ok(boxed(NmbArgumentEncoder(get(function)?.0.new_argument_encoder_with_buffer_index(index).map_err(e("argument encoder"))?))))
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_release(p:*mut NmbArgumentEncoder){release(p)}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_query(p:*const NmbArgumentEncoder,alignment:bool)->u64 {guard(0,||{let p=&get(p)?.0;Ok(if alignment{p.alignment()}else{p.encoded_length()})})}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_target(p:*mut NmbArgumentEncoder,buffer:*const NmbBuffer,offset:u64,element:u64)->bool {
 guard(false,||{let b=if buffer.is_null(){None}else{Some(&get(buffer)?.0)};get_mut(p)?.0.set_argument_buffer_start_offset_array_element(b,offset,element).map_err(e("argument buffer target"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_buffer(p:*const NmbArgumentEncoder,buffer:*const NmbBuffer,offset:u64,index:u32)->bool {
 guard(false,||{let b=if buffer.is_null(){None}else{Some(&get(buffer)?.0)};get(p)?.0.set_buffer(b,offset,index).map_err(e("argument buffer"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_texture(p:*const NmbArgumentEncoder,texture:*const NmbTexture,index:u32)->bool {
 guard(false,||{let t=if texture.is_null(){None}else{Some(&get(texture)?.0)};get(p)?.0.set_texture(t,index).map_err(e("argument texture"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_sampler(p:*const NmbArgumentEncoder,sampler:*const NmbSampler,index:u32)->bool {
 guard(false,||{let s=if sampler.is_null(){None}else{Some(&get(sampler)?.0)};get(p)?.0.set_sampler_state(s,index).map_err(e("argument sampler"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_constant(p:*const NmbArgumentEncoder,index:u32)->*mut c_void {
 guard(std::ptr::null_mut(),||Ok(get(p)?.0.constant_data(index).map_err(e("argument constant"))?.as_ptr() as *mut c_void))
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_nested(p:*const NmbArgumentEncoder,index:u32)->*mut NmbArgumentEncoder {
 guard(std::ptr::null_mut(),||Ok(boxed(NmbArgumentEncoder(get(p)?.0.new_argument_encoder_for_buffer_at_index(index).map_err(e("nested argument encoder"))?))))
}

pub struct NmbFunctionHandle(metal2vulkan_runtime::FunctionHandle);
pub struct NmbVisibleTable(metal2vulkan_runtime::VisibleFunctionTable,u64);
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_handle(compute:*const NmbComputePipeline,render:*const NmbRenderPipeline,function:*const NmbFunction,stages:u64)->*mut NmbFunctionHandle {
 guard(std::ptr::null_mut(),||{let f=&get(function)?.0;let h=if !compute.is_null()&&render.is_null(){get(compute)?.0.function_handle(f)}else if !render.is_null()&&compute.is_null(){get(render)?.0.function_handle(f,raw::<RenderStages>(stages,"render stage")?)}else{return Err("one pipeline required".into())}.map_err(e("function handle"))?;Ok(h.map_or(std::ptr::null_mut(),|h|boxed(NmbFunctionHandle(h))))})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_handle_name(p:*const NmbFunctionHandle,out:*mut c_char,size:usize)->usize {guard(0,||Ok(copy_out(get(p)?.0.name(),out,size)))}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_handle_release(p:*mut NmbFunctionHandle){release(p)}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_visible_table_new(compute:*const NmbComputePipeline,render:*const NmbRenderPipeline,count:u64,stages:u64)->*mut NmbVisibleTable {
 guard(std::ptr::null_mut(),||{let d=metal2vulkan_runtime::VisibleFunctionTableDescriptor::new(count);let t=if !compute.is_null()&&render.is_null(){get(compute)?.0.new_visible_function_table(&d)}else if !render.is_null()&&compute.is_null(){get(render)?.0.new_visible_function_table(&d,raw::<RenderStages>(stages,"render stage")?)}else{return Err("one pipeline required".into())}.map_err(e("visible function table"))?;Ok(boxed(NmbVisibleTable(t,count)))})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_visible_table_release(p:*mut NmbVisibleTable){release(p)}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_visible_table_set(p:*const NmbVisibleTable,function:*const NmbFunctionHandle,index:u32)->bool {
 guard(false,||{get(p)?.0.set_function(if function.is_null(){None}else{Some(&get(function)?.0)},index).map_err(e("visible function"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_visible_table_query(p:*const NmbVisibleTable,which:u32)->u64 {
 guard(0,||{let t=&get(p)?.0;Ok(match which{0=>t.allocated_size(),1=>t.gpu_resource_id().0,2=>get(p)?.1,3=>t.resource_options().raw(),4=>t.heap_offset(),5=>u64::from(t.is_aliasable()),_=>return Err("unknown table property".into())})})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_visible_table(p:*mut NmbEncoder,stage:u32,table:*const NmbVisibleTable,index:u32)->bool {
 guard(false,||{let t=if table.is_null(){None}else{Some(&get(table)?.0)};match get_mut(p)?{
 NmbEncoder::Compute(c) if stage==0=>c.set_visible_function_table(t,index),
 NmbEncoder::Render(c) if stage==1=>c.set_vertex_visible_function_table(t,index),
 NmbEncoder::Render(c) if stage==2=>c.set_fragment_visible_function_table(t,index),
 _=>return Err("visible table stage mismatch".into())}.map_err(e("bind visible table"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_pipeline(p:*const NmbArgumentEncoder,compute:*const NmbComputePipeline,render:*const NmbRenderPipeline,index:u32)->bool {
 guard(false,||{let p=&get(p)?.0;
 if !render.is_null(){p.set_render_pipeline_state(Some(&get(render)?.0),index)}else{p.set_compute_pipeline_state(if compute.is_null(){None}else{Some(&get(compute)?.0)},index)}.map_err(e("argument pipeline"))?;Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_visible_table(p:*const NmbArgumentEncoder,table:*const NmbVisibleTable,index:u32)->bool {
 guard(false,||{get(p)?.0.set_visible_function_table(if table.is_null(){None}else{Some(&get(table)?.0)},index).map_err(e("argument function table"))?;Ok(true)})
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_constant_info(p:*const NmbFunction,index:usize,name:*mut c_char,size:usize,words:*mut u64)->bool {
 guard(false,||{if words.is_null(){return Err("null constant metadata".into())}let map=get(p)?.0.function_constants_dictionary().map_err(e("function constants"))?;
 let Some((key,value))=map.iter().nth(index) else {return Ok(false)};
 let ty=value.data_type();let at=value.index();let required=u64::from(value.is_required());
 copy_out(key,name,size);unsafe{std::slice::from_raw_parts_mut(words,3)}.copy_from_slice(&[ty.as_raw() as u64,at as u64,required as u64]);Ok(true)})
}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_query(p:*const NmbFunction,which:u32)->u64 {
 guard(0,||{let f=&get(p)?.0;Ok(match which{0=>f.patch_type().map_err(e("patch type"))?.as_raw() as u64,1=>f.patch_control_point_count().map_err(e("patch control points"))?.unwrap_or(0) as u64,2=>f.function_constants_dictionary().map_err(e("function constants"))?.len() as u64,_=>return Err("unknown function property".into())})})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_pipeline_adding(compute:*const NmbComputePipeline,render:*const NmbRenderPipeline,functions:*const *const NmbFunction,count:usize,stages:*const u32)->*mut c_void {
 guard(std::ptr::null_mut(),||{if count>4096||(count>0&&(functions.is_null()||(!render.is_null()&&stages.is_null()))){return Err("invalid binary functions".into())}
 let input=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(functions,count)}};let mut added=Vec::with_capacity(count);for f in input{added.push(get(*f)?.0.clone())}
 if !compute.is_null()&&render.is_null(){Ok(boxed(NmbComputePipeline(get(compute)?.0.new_compute_pipeline_state_with_additional_binary_functions(&added).map_err(e("additional compute functions"))?)) as *mut c_void)}
 else if !render.is_null()&&compute.is_null(){let mut d=metal2vulkan_runtime::RenderPipelineFunctionsDescriptor::new();let stages=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(stages,count)}};
 for (f,s) in added.into_iter().zip(stages){match s{1=>d.vertex_additional_binary_functions.push(f),2=>d.fragment_additional_binary_functions.push(f),3=>d.tile_additional_binary_functions.push(f),_=>return Err("unknown function stage".into())}}
 Ok(boxed(NmbRenderPipeline(get(render)?.0.new_render_pipeline_state_with_additional_binary_functions(&d).map_err(e("additional render functions"))?)) as *mut c_void)}else{Err("one pipeline required".into())}})
}

fn blit_option(value:u64)->Result<BlitOption,String>{match value{0=>Ok(BlitOption::None),1=>Ok(BlitOption::DepthFromDepthStencil),2=>Ok(BlitOption::StencilFromDepthStencil),4=>Ok(BlitOption::RowLinearPVRTC),_=>Err("unknown blit options".into())}}
#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_buffer_texture_options(p:*mut NmbEncoder,buffer:*const NmbBuffer,texture:*const NmbTexture,offset:u64,row:u64,image:u64,size:*const u64,origin:*const u64,slice:u64,level:u64,options:u64,to_texture:bool)->bool {
 guard(false,||{if size.is_null()||origin.is_null(){return Err("null copy dimensions".into())}let s=unsafe{std::slice::from_raw_parts(size,3)};let o=unsafe{std::slice::from_raw_parts(origin,3)};let s=Size::new(s[0],s[1],s[2]);let o=Origin::new(o[0],o[1],o[2]);
 let NmbEncoder::Blit(c)=get_mut(p)? else{return Err("not blit".into())};let b=&get(buffer)?.0;let t=&get(texture)?.0;let opt=blit_option(options)?;
 if to_texture{c.copy_from_buffer_to_texture(b,offset,row,image,s,t,slice,level,o,opt)}else{c.copy_from_texture_to_buffer(t,slice,level,o,s,b,offset,row,image,opt)}.map_err(e("buffer/texture copy options"))?;Ok(true)})
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_argument_encoder_descriptors(device:*const NmbDevice,words:*const u64,count:usize)->*mut NmbArgumentEncoder {
 guard(std::ptr::null_mut(),||{if count>4096||(count>0&&words.is_null()){return Err("invalid argument descriptors".into())}let data=if count==0{&[][..]}else{unsafe{std::slice::from_raw_parts(words,count*6)}};
 let mut descriptors=Vec::with_capacity(count);for v in data.chunks_exact(6){let mut d=metal2vulkan::bind::ArgumentDescriptor::new(raw::<DataType>(v[0],"argument data type")?,u32::try_from(v[1]).map_err(e("argument index"))?);
 d.array_length=u32::try_from(v[2]).map_err(e("argument length"))?;
 d.access=raw::<metal2vulkan::metal::BindingAccess>(v[3],"argument access")?;
 d.texture_type=raw::<TextureType>(v[4],"argument texture type")?;d.constant_block_alignment=u32::try_from(v[5]).map_err(e("argument alignment"))?;descriptors.push(d)}
 Ok(boxed(NmbArgumentEncoder(get(device)?.0.new_argument_encoder_with_arguments(&descriptors).map_err(e("argument descriptors"))?)))})
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_device_heap_size(device:*const NmbDevice,descriptor:*const NmbTextureDescriptor,length:u64,resource_options:u64,out:*mut u64)->bool {
 guard(false,||{if out.is_null(){return Err("null heap size result".into())}
 let device=&get(device)?.0;
 let v=if descriptor.is_null(){device.heap_buffer_size_and_align(length,options(resource_options)?)}
 else{device.heap_texture_size_and_align(&texture_descriptor(get(descriptor)?)?).map_err(e("heap texture size"))?};
 unsafe{std::slice::from_raw_parts_mut(out,2)}.copy_from_slice(&[v.size,v.align]);Ok(true)})
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_amplification(encoder:*mut NmbEncoder,count:u64,mappings:*const u32)->bool {
 guard(false,||{if count>8{return Err("amplification count past 8".into())}
 let values=if mappings.is_null(){None}else{Some(unsafe{std::slice::from_raw_parts(mappings,count as usize*2)}.chunks_exact(2).map(|v|metal2vulkan_runtime::VertexAmplificationViewMapping::new(v[0],v[1])).collect::<Vec<_>>())};
 let NmbEncoder::Render(c)=get_mut(encoder)? else{return Err("not render".into())};
 c.set_vertex_amplification_count(count,values.as_deref()).map_err(e("vertex amplification"))?;Ok(true)})
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_io_rows(p:*const NmbTexture,height:u64)->u64 {
 guard(0,||{let t=&get(p)?.0;let block=metal2vulkan::metal::format::block_extent(t.pixel_format()).unwrap_or([1,1]);Ok(height.div_ceil(u64::from(block[1])))})
}

#[cfg(x9_runtime_latest)]
pub struct NmbCounter(metal2vulkan_runtime::CounterSampleBuffer);
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub extern "C" fn nmb_counter_supported(device:*const NmbDevice)->bool {guard(false,||Ok(get(device)?.0.supports_timestamp_counters()))}
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub extern "C" fn nmb_counter_new(device:*const NmbDevice,count:usize)->*mut NmbCounter {guard(std::ptr::null_mut(),||Ok(boxed(NmbCounter(get(device)?.0.new_counter_sample_buffer(count).map_err(e("counter sample buffer"))?))))}
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub extern "C" fn nmb_counter_release(p:*mut NmbCounter){release(p)}
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_counter_resolve(p:*const NmbCounter,start:usize,count:usize,out:*mut u64)->bool {
 guard(false,||{if count>0&&out.is_null(){return Err("null counter result".into())}let values=get(p)?.0.resolve(start,count).map_err(e("resolve counters"))?;if count>0{unsafe{std::slice::from_raw_parts_mut(out,count)}.copy_from_slice(&values)}Ok(true)})
}
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub extern "C" fn nmb_counter_sample(encoder:*mut NmbEncoder,p:*const NmbCounter,index:usize,barrier:bool)->bool {
 guard(false,||{let counter=&get(p)?.0;match get_mut(encoder)?{NmbEncoder::Compute(e)=>e.sample_counters_in_buffer(counter,index,barrier),NmbEncoder::Blit(e)=>e.sample_counters_in_buffer(counter,index,barrier),NmbEncoder::Render(e)=>e.sample_counters_in_buffer(counter,index,barrier)}.map_err(e("sample counter"))?;Ok(true)})
}
#[cfg(x9_runtime_latest)]
#[unsafe(no_mangle)]
pub extern "C" fn nmb_counter_gpu_resolve(encoder:*mut NmbEncoder,p:*const NmbCounter,start:usize,count:usize,destination:*const NmbBuffer,offset:u64)->bool {
 guard(false,||{let NmbEncoder::Blit(encoder)=get_mut(encoder)? else{return Err("counter resolve requires blit encoder".into())};encoder.resolve_counters(&get(p)?.0,start,count,&get(destination)?.0,offset).map_err(e("resolve counters into buffer"))?;Ok(true)})
}
