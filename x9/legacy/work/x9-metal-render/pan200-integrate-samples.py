from pathlib import Path
w=Path(__file__).resolve().parent
s=w/'metal2vulkan-pan200-source/crates'
c=s/'metal2vulkan/src'; r=s/'metal2vulkan-runtime/src'
def edit(path, old, new, count=1):
    text=path.read_text()
    assert text.count(old)==count,(str(path),text.count(old),old[:90])
    path.write_text(text.replace(old,new))
edit(c/'pipeline/graphics.rs','    pub vertex_capture: Option<crate::pipeline::interface::VertexCapture>,','''    pub vertex_capture: Option<crate::pipeline::interface::VertexCapture>,
    /// Whether the fragment interface can run with viewport-shifted, isolated sample draws.
    /// Cross-sample coverage, window coordinates and memory/attachment side effects are excluded.
    /// This is an execution contract, not a claim of full programmable-sample-position support.
    #[cfg_attr(feature = "serde", serde(default))]
    pub sample_replay_safe: bool,''')
edit(c/'pipeline/graphics.rs','            vertex_capture: None,','            vertex_capture: None,\n            sample_replay_safe: false,')
edit(c/'pipeline/link/graphics.rs','        vertex_capture: None,','''        vertex_capture: None,
        sample_replay_safe: fs.is_none_or(|fs| {
            !fs.writes_memory && !fs.io.varyings_at_pixel
                && fs.io.builtins.iter().all(|b| matches!(b,
                    StageBuiltin::FrontFacing | StageBuiltin::SampleId | StageBuiltin::PrimitiveId
                    | StageBuiltin::Layer | StageBuiltin::ViewportIndex | StageBuiltin::ViewIndex
                    | StageBuiltin::FragDepth(_) | StageBuiltin::SampleMaskOut))
        }),''')
edit(c/'cache/codec.rs','early_fragment_tests, mesh_records, vertex_capture,','early_fragment_tests, mesh_records, vertex_capture, sample_replay_safe,')
for constant in ['SEMANTICS_VERSION','ARTIFACT_FORMAT_VERSION','RUNTIME_VERSION']:
    import re
    p=c/'cache/mod.rs'; text=p.read_text(); text,n=re.subn(r'(pub const '+constant+r': u32 = )\d+;',r'\g<1>2000001;',text); assert n==1; p.write_text(text)
edit(r/'pipeline/graphics.rs','    pub(crate) sample_positions: Vec<[u8; 2]>,','''    pub(crate) sample_positions: Vec<[u8; 2]>,
    /// Isolate this standard-grid sample for viewport-shifted coverage/depth emulation.
    pub(crate) replay_sample: Option<u32>,''')
edit(r/'pipeline/graphics.rs',"    pub(crate) sample_positions: &'a [[u8; 2]],","    pub(crate) sample_positions: &'a [[u8; 2]],\n    pub(crate) replay_sample: Option<u32>,")
edit(r/'pipeline/graphics.rs','            sample_positions,\n        } = key;','            sample_positions,\n            replay_sample,\n        } = key;')
edit(r/'pipeline/graphics.rs','            && self.sample_positions == &sample_positions[..]','            && self.sample_positions == &sample_positions[..]\n            && self.replay_sample == *replay_sample')
edit(r/'pipeline/graphics.rs','            sample_positions: self.sample_positions.to_vec(),','            sample_positions: self.sample_positions.to_vec(),\n            replay_sample: self.replay_sample,')
edit(r/'pipeline/graphics.rs','                &key.sample_positions,','                &key.sample_positions,\n                key.replay_sample,')
edit(r/'pipeline/graphics.rs','            sample_shading: graphics.sample_shading,','            sample_shading: graphics.sample_shading || key.replay_sample.is_some(),')
edit(r/'pipeline/graphics.rs','''            sample_mask: graphics.sample_mask,
            sample_locations: key
                .sample_positions
                .iter()''','''            sample_mask: match key.replay_sample {
                Some(sample) => Some(graphics.sample_mask.unwrap_or(u32::MAX) & (1u32 << sample)),
                None => graphics.sample_mask,
            },
            sample_locations: key
                .sample_positions
                .iter()
                .filter(|_| key.replay_sample.is_none())''')
edit(r/'pipeline/predicted.rs','            sample_positions: Vec::new(),','            sample_positions: Vec::new(),\n            replay_sample: None,')
edit(r/'pipeline/render/residual.rs','            sample_positions: key.sample_positions.clone(),','            sample_positions: key.sample_positions.clone(),\n            replay_sample: key.replay_sample,')
edit(r/'sample_positions.rs','''    let rules = PositionRules::of(device, samples);
    let _ = rules;
    grid("sample_positions",device,positions,samples,None).is_ok_and(|grid|!grid.is_empty())''','''    // Only native custom positions require depth images made for VK_EXT_sample_locations.
    // The shifted-sample path keeps ordinary depth/stencil storage in its original sample slots.
    let rules = PositionRules::of(device, samples);
    rules.located_at_samples && rules.variable
        && grid("sample_positions", device, positions, samples, None)
            .is_ok_and(|grid| !grid.is_empty())''')
edit(r/'sample_positions.rs','''        if standard_grid(samples).is_some_and(|standard|standard==checked.as_slice()) {''','''        if !rules.located_at_samples && replay_capable(device, samples) {
            if standard_grid(samples).is_some_and(|standard| standard == checked.as_slice()) {
                return Ok(Vec::new());
            }
            return Ok(checked);
        }
        if standard_grid(samples).is_some_and(|standard|standard==checked.as_slice()) {''')
edit(r/'sample_positions.rs',"fn standard_grid(samples:u32)->Option<&'static [Sixteenths]> {","pub(super) fn standard_grid(samples:u32)->Option<&'static [Sixteenths]> {")
insert='''/// The fallback relies on actual standard raster locations, not just the compiler's LUT.
/// It uses full sample-rate shading and isolates a hardware sample with pSampleMask.
pub(super) fn replay_capable(device: &Device, samples: u32) -> bool {
    use metal2vulkan::device::VkFeature;
    let shared = device.shared();
    // SAFETY: the physical device belongs to the live instance.
    let limits = unsafe { shared.instance.raw.get_physical_device_properties(shared.physical) }.limits;
    standard_grid(samples).is_some() && samples <= 32
        && limits.standard_sample_locations == ash::vk::TRUE
        && device.compile_device().profile().enabled().features.contains(&VkFeature::SampleRateShading)
}

/// Whether this validated grid needs the viewport-shifted rasterization path.
pub(super) fn replayed(device: &Device, samples: u32, grid: &[Sixteenths]) -> bool {
    !grid.is_empty() && !PositionRules::of(device, samples).located_at_samples
        && replay_capable(device, samples)
}

/// Translating geometry by native minus requested makes the fixed hardware sample evaluate
/// the requested point in the original primitive, including interpolated depth and depth bias.
pub(super) fn offsets(samples: u32, grid: &[Sixteenths]) -> Option<Vec<[f32; 2]>> {
    let native = standard_grid(samples)?;
    if grid.len() != native.len() { return None; }
    Some(native.iter().zip(grid).map(|(n, p)| [
        (f32::from(n[0]) - f32::from(p[0])) / 16.0,
        (f32::from(n[1]) - f32::from(p[1])) / 16.0,
    ]).collect())
}

'''
edit(r/'sample_positions.rs','/// Vulkan\'s standard sample locations, in sample-index order and sixteenths.',insert+'/// Vulkan\'s standard sample locations, in sample-index order and sixteenths.')
edit(r/'error.rs','    SamplePositionsAtBegin,','''    SamplePositionsAtBegin,
    /// A shifted-sample raster draw with cross-sample or screen-coordinate semantics not yet
    /// lowered, shader attachment tests, side effects, or a replay/rate-map/mesh combination.
    SamplePositionReplayDraw,''')
edit(r/'degradation.rs','        EmulatedMeshDraws = "emulated-mesh-draws", Performance,','''        ShiftedSampleRasterization = "shifted-sample-rasterization", Precision,
            "custom sample coverage and depth/stencil are rasterized as isolated samples with "
            "shifted viewports; fragment interpolation runs at sample rate rather than pixel or "
            "centroid rate; cross-sample and unlowered coordinate semantics are refused";
        /// A vertex function executes once in compute, then the raster draw reads its records.
        VertexComputeCapture = "vertex-compute-capture", Performance,
            "the vertex function runs in compute subgroups, with inactive tail lanes excluded, "
            "before a synchronized pull draw; subgroup grouping is implementation dependent";
        /// A mesh render pipeline's compute capture and synthesized raster draw.
        EmulatedMeshDraws = "emulated-mesh-draws", Performance,''')
print('pan200 sample pipeline contracts integrated')
