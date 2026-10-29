#!/usr/bin/env python3
"""Prepare a negotiated native cache-ownership path, keeping Vulkan flags honest."""
from pathlib import Path
w=Path(__file__).resolve().parent;root=w.parents[1]
runtime=w/'metal2vulkan-source/crates/metal2vulkan-runtime/src'
baseline=w/'shared-cache-inputs';baseline.mkdir(exist_ok=True)
def edit(p,old,new):
 p=Path(p);s=p.read_text()
 if new in s:return
 if old not in s:
  if new in s:return
  raise RuntimeError('missing source anchor '+str(p)+' '+old[:50])
 if s.count(old)!=1:raise RuntimeError('ambiguous source anchor '+str(p)+' '+old[:50])
 rel=p.relative_to(root);saved=baseline/rel
 if not saved.exists():saved.parent.mkdir(parents=True,exist_ok=True);saved.write_text(s)
 p.write_text(s.replace(old,new))
general=root/'MaliMetal/X9/General'
# This flag specifies ownership/caching at synchronous submission boundaries;
# it never means hardware snooping or HOST_COHERENT Vulkan memory.
edit(general/'X9RenderABI.h','X9RenderFragment=16u };','X9RenderFragment=16u, X9RenderSubmissionCacheOwnership=32u };\n#define X9_RENDER_CACHE_ATOM 256u')
engine=general/'kernel/SweetX9RenderEngine.inc'
edit(engine,' *caps=resident->caps;',''' uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 uint64_t line=4ULL<<((ctr>>16)&15);
 if(line>X9_RENDER_CACHE_ATOM||X9_RENDER_CACHE_ATOM%line)return false;
 *caps=resident->caps;''')
edit(engine,'X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment;','X9RenderCSFSubmit|X9RenderTiler|X9RenderFragment|X9RenderSubmissionCacheOwnership;')
edit(engine,'r.handle,X9RenderClean,0,0,r.bytes','r.handle,X9RenderClean|X9RenderInvalidate,0,0,r.bytes')
edit(engine,'r.handle,X9RenderInvalidate,0,0,r.bytes','r.handle,X9RenderClean|X9RenderInvalidate,0,0,r.bytes')
edit(engine,' * during this synchronous submit. No HOST_COHERENT capability is implied. */',''' * during this synchronous submit. No HOST_COHERENT capability is implied.
  * CIVAC before GPU access removes dirty host lines as well as speculative
  * clean copies. CIVAC after confirmed GPU quiescence preserves dirty lines
  * of other, unused host resources in a BO while discarding stale clean ones.
  * The runtime must isolate independently owned resources to cache atoms. */''')
edit(general/'X9RenderTransport.h','const char *x9_render_last_error(void);','''const char *x9_render_last_error(void);
/* Returns0 unless this caller negotiated real synchronous cache ownership. */
uint64_t x9_render_submission_cache_atom(void);''')
p=general/'X9RenderTransport.c';s=p.read_text()
if 'uint64_t x9_render_submission_cache_atom(void)' not in s:
 edit(p,'const char *x9_render_last_error(void) {return x9_render_error;}','''const char *x9_render_last_error(void) {return x9_render_error;}
uint64_t x9_render_submission_cache_atom(void) {
 struct X9RenderCaps caps={0};
 struct X9RenderConnection *connection=x9_render_open(&caps);if(!connection)return 0;
 x9_render_close(connection);
 return (caps.capabilities&X9RenderSubmissionCacheOwnership)?X9_RENDER_CACHE_ATOM:0;
}''')
# Unsafe opt-in: a foreign driver cannot gain this behavior just by lacking
# HOST_COHERENT. The embedding adapter must prove its submission contract.
p=runtime/'device/create.rs'
edit(p,'    pub record_executions: bool,','''    pub record_executions: bool,
    pub(super) submission_cache_atom: u64,''')
anchor='/// The devices `system_default_with` and `all_with` made, by the options they were made with'
method='''impl DeviceOptions {
    /// Enable Metal shared storage on a driver that transfers CPU/GPU cache
    /// ownership around every synchronous submission. No Vulkan memory flag changes.
    ///
    /// # Safety
    ///
    /// Every driver submission, including internal/staging submissions, must clean
    /// and invalidate host caches before GPU access and after confirmed GPU
    /// quiescence, before any completion/fence/semaphore becomes visible. Cached
    /// imports and every CPU alias must be maintained. CPU access to GPU-owned
    /// resource atoms must be synchronized. `atom` must cover all CPU cache lines.
    /// The runtime aligns separate placements and refuses partial-atom no-copy
    /// imports; aliases of one resource still share its ownership contract.
    pub unsafe fn with_submission_cache_ownership(mut self, atom: std::num::NonZeroU64) -> Self {
        assert!(atom.get().is_power_of_two());
        self.submission_cache_atom = atom.get();
        self
    }
}

'''
edit(p,anchor,method+anchor)
edit(p,'        record_executions: options.record_executions,','        record_executions: options.record_executions,\n        submission_cache_atom: options.submission_cache_atom,')
edit(p,'        record_executions,\n    } = a;','        record_executions,\n        submission_cache_atom,\n    } = a;')
edit(p,'        && *record_executions == b.record_executions','        && *record_executions == b.record_executions\n        && *submission_cache_atom == b.submission_cache_atom')
p=runtime/'device/mod.rs'
edit(p,'    pub(super) record_executions: bool,','    pub(super) record_executions: bool,\n    pub(super) submission_cache_atom: u64,')
edit(p,'            record_executions,\n            view_reinterpretation,','            record_executions,\n            submission_cache_atom,\n            view_reinterpretation,')
edit(p,'        let memory = Memory::new(','        let mut memory = Memory::new(')
edit(p,'            queue_families.clone(),\n        );','            queue_families.clone(),\n        );\n        memory.set_submission_cache_atom(submission_cache_atom);')
edit(p,'            limits.min_uniform_buffer_offset_alignment,','            limits.min_uniform_buffer_offset_alignment,\n            submission_cache_atom.max(1),')
p=runtime/'device/adopt.rs'
edit(p,'        let parts = Parts {','        let parts = Parts {\n            submission_cache_atom: 0,')
p=runtime/'memory.rs'
edit(p,'    use vk::MemoryPropertyFlags as P;\n    let usable = |index: usize| allowed & (1 << index) != 0;','''    candidates_with_ownership(types, allowed, class, unified, false)
}

fn candidates_with_ownership(
    types: &[vk::MemoryType], allowed: u32, class: MemoryClass,
    unified: bool, maintained: bool,
) -> Vec<u32> {
    use vk::MemoryPropertyFlags as P;
    let usable = |index: usize| allowed & (1 << index) != 0;''')
edit(p,'    let lacks = |flags: P, wanted: P|','''    let required = if maintained { required & !P::HOST_COHERENT } else { required };
    let lacks = |flags: P, wanted: P|''')
edit(p,'    types: Vec<vk::MemoryType>,','    types: Vec<vk::MemoryType>,\n    submission_cache_atom: u64,')
edit(p,'            types,\n            block_bytes:','            types,\n            submission_cache_atom: 0,\n            block_bytes:')
anchor='    /// The bytes of `VkDeviceMemory` allocated so far for blocks of the runtime\'s own memory,'
methods='''    pub(super) fn set_submission_cache_atom(&mut self, atom: u64) {
        assert!(atom == 0 || atom.is_power_of_two());
        self.submission_cache_atom = atom;
    }

    /// Cache ownership granularity:1 for normal coherent-only allocation.
    pub(super) fn submission_cache_atom(&self) -> u64 {
        self.submission_cache_atom.max(1)
    }

    fn candidates(&self, allowed: u32, class: MemoryClass) -> Vec<u32> {
        candidates_with_ownership(&self.types, allowed, class, self.unified, self.submission_cache_atom != 0)
    }

'''
edit(p,anchor,methods+anchor)
edit(p,'        candidates(&self.types, allowed, class, self.unified)','        self.candidates(allowed, class)')
edit(p,'            candidates(&self.types, bits, class, self.unified)','            self.candidates(bits, class)')
# Two identical import loops: intentionally change both, guarded as a pair.
s=p.read_text();old='for type_index in candidates(&self.types, allowed, class, self.unified) {'
if old in s:
 if s.count(old)!=2:raise RuntimeError('import loop count')
 p.write_text(s.replace(old,'for type_index in self.candidates(allowed, class) {'))
edit(p,'    let align = requirements.alignment.max(1);','''    // Different host resources never share a CPU cache atom. Their logical
    // sizes remain unchanged; the next placement also starts at an atom.
    let align = requirements.alignment.max(lock(memory).submission_cache_atom());''')
edit(p,'            candidates(&memory.types, bits, class, memory.unified),','            memory.candidates(bits, class),')
# No-copy host memory must own complete atoms (an imported surface owns its
# full page-rounded backing). Keep the existing typed unsupported result.
p=runtime/'buffer_descriptor.rs'
edit_host_old='        let (pointer, source) = memory.into_parts();\n        let host = host_window(call, importer, pointer, length, source)?;'
edit_host_new='''        let (pointer, source) = memory.into_parts();
        let atom = lock(&shared.memory).submission_cache_atom();
        if !((pointer.as_ptr() as u64) | length).is_multiple_of(atom) {
            return Err(not_importable());
        }
        let host = host_window(call, importer, pointer, length, source)?;'''
s=p.read_text()
if edit_host_old in s:
 if s.count(edit_host_old)!=2:raise RuntimeError('host import entry count')
 saved=baseline/p.relative_to(root);saved.parent.mkdir(parents=True,exist_ok=True)
 if not saved.exists():saved.write_text(s)
 p.write_text(s.replace(edit_host_old,edit_host_new))
# Heap buffer placement already starts at256-byte boundaries. Raise image
# placement too; automatic and explicit heap paths use this same plan.
p=runtime/'texture/heap.rs'
edit(p,'        check_heap_base(size_and_align.align)?;','''        size_and_align.align = size_and_align.align.max(
            crate::device::lock(&self.shared().memory).submission_cache_atom());
        check_heap_base(size_and_align.align)?;''')
# The bridge enables the option only after asking the real X9 kernel service.
p=general/'bridge/NativeMetalBridge.rs'
edit(p,'fn open(loader: &CStr, cache: &CStr) -> Result<Device, String> {','''unsafe extern "C" { fn x9_render_submission_cache_atom() -> u64; }
fn open(loader: &CStr, cache: &CStr) -> Result<Device, String> {''')
edit(p,'    let mut options = DeviceOptions::default();','''    let mut options = DeviceOptions::default();
    // The native service advertises this only with the synchronous execution
    // engine and explicit pre/post cache ownership. Never infer snooping from
    // a GPU model, a loader filename or a Vulkan memory property's absence.
    let atom = unsafe { x9_render_submission_cache_atom() };
    let atom = std::num::NonZeroU64::new(atom)
        .ok_or_else(|| "native submission cache-ownership contract unavailable".to_string())?;
    if atom.get() != 256 { return Err("unexpected native cache atom".into()); }
    // SAFETY: negotiated from the same X9 service used by the only native ICD.
    options = unsafe { options.with_submission_cache_ownership(atom) };''')
# CPU image-copy entrypoints do not pass through the native submission owner.
mesa=w/'mesa-render-source/src/panfrost/vulkan'
edit(mesa/'panvk_vX_physical_device.c','      .EXT_host_image_copy = true,','''#ifdef X9_NATIVE_KMOD
      .EXT_host_image_copy = false,
#else
      .EXT_host_image_copy = true,
#endif''')
print('Negotiated cache ownership, atom-isolated allocation/import and native bridge prepared; Vulkan HOST_COHERENT remains unset',flush=True)
