from pathlib import Path
w=Path(__file__).resolve().parent
p=w/'metal2vulkan-pan200-source/crates/metal2vulkan-runtime/src/render.rs'
s=p.read_text()
def edit(old,new):
    global s
    assert s.count(old)==1,(s.count(old),old[:90]);s=s.replace(old,new)
edit('''        if graphics.vertex_capture.is_some()
            && (self.replaying''','''        let replay_samples = sample_positions::replayed(
            &device, self.layout.samples, &self.layout.sample_positions,
        );
        if replay_samples && (!graphics.sample_replay_safe
            || self.replaying || self.rerouting || self.rate_map.is_some()
            || self.layout.fetch_inputs || graphics.fragment_writes_memory
            || (graphics.vertex_writes_memory && graphics.vertex_capture.is_none())
            || graphics.mesh_records.is_some() || graphics.tessellation.is_some()
            || graphics.emulated_tessellation.is_some() || graphics.alpha_to_coverage
            || !graphics.storage_attachments.is_empty() || !graphics.input_attachments.is_empty()
            || graphics.fragment_imageblock.is_some() || graphics.shader_tests.is_some()
            || graphics.shadow_stencil.is_some() || graphics.snapshot_reads
            || !matches!(topology, metal2vulkan::vk::PrimitiveTopology::TRIANGLE_LIST
                | metal2vulkan::vk::PrimitiveTopology::TRIANGLE_STRIP
                | metal2vulkan::vk::PrimitiveTopology::TRIANGLE_FAN))
        {
            return Err(Error::NotImplemented(RuntimeGap::SamplePositionReplayDraw));
        }
        if graphics.vertex_capture.is_some()
            && (self.replaying''')
edit('''            sample_positions: &self.layout.sample_positions,
        };''','''            sample_positions: &self.layout.sample_positions,
            replay_sample: replay_samples.then_some(0),
        };''')
edit('''        let token = shape_token.filter(|_| commands.specialization.is_none());''','''        let token = shape_token.filter(|_| commands.specialization.is_none() && !replay_samples);''')
edit('''        if reused_state && reused_pipeline {''','''        // Prepare every pipeline and shifted viewport before recording anything. Each pipeline
        // enables one *hardware* sample, so color, stencil and depth retain their sample indices.
        // Capture remains outside this loop: a Metal vertex function is never re-executed here.
        let mut sample_recordings = Vec::new();
        if replay_samples {
            let offsets = sample_positions::offsets(self.layout.samples, &self.layout.sample_positions)
                .ok_or_else(|| internal("a replayed grid without its standard positions"))?;
            // SAFETY: this physical device belongs to the live instance.
            let limits = unsafe { device.shared().instance.raw.get_physical_device_properties(
                device.shared().physical) }.limits;
            for (sample, [dx, dy]) in offsets.into_iter().enumerate() {
                let mut key = draw_key();
                key.replay_sample = Some(sample as u32);
                let (made, _) = variant.graphics_pipeline_now(
                    &device, &key, &compatible, (&pipeline, &formats),
                )?;
                let states = (0..draw_states.len()).map(|a| {
                    draw_states.of(a).iter().map(|state| {
                        let mut shifted = state.clone();
                        for viewport in shifted.viewports.iter_mut() {
                            viewport.x += dx;
                            viewport.y += dy;
                            let [lo, hi] = limits.viewport_bounds_range;
                            if !viewport.x.is_finite() || !viewport.y.is_finite()
                                || viewport.x < lo || viewport.x + viewport.width > hi
                                || viewport.y.min(viewport.y + viewport.height) < lo
                                || viewport.y.max(viewport.y + viewport.height) > hi {
                                return Err(Error::NotImplemented(RuntimeGap::SamplePositionReplayDraw));
                            }
                        }
                        Ok(shifted)
                    }).collect::<Result<Vec<_>, Error>>()
                }).collect::<Result<Vec<_>, Error>>()?;
                sample_recordings.push((made, states));
            }
        }
        if reused_state && reused_pipeline {''')
# DrawStates has either one family broadcast or one per amplification; use the amplification
# count instead of relying on a representation-specific len method.
s=s.replace('(0..draw_states.len()).map(|a| {','(0..pushes_len_for_samples(&commands)).map(|a| {') if False else s
# pushes is constructed later, but commands.amplifications already carries its eventual length.
s=s.replace('(0..draw_states.len()).map(|a| {','(0..commands.amplifications.len().max(1)).map(|a| {')
edit('''        if let Some(steps) = capture_steps.as_ref().filter(|_| records) {''','''        if records {
            created.extend(sample_recordings.iter().skip(1).map(|(made, _)| made.created_in));
        }
        if let Some(steps) = capture_steps.as_ref().filter(|_| records) {''')
edit('''            for &phase in phases {
                let mut first = true;''','''            for &phase in phases {
              for sample in 0..sample_recordings.len().max(1) {
                let sample_recording = sample_recordings.get(sample);
                let made_now = sample_recording.map(|(made, _)| made.raw).or(made_now);
                let mut first = true;''')
edit('''                    for state in draw_states.of(a) {''','''                    let states = sample_recording.map(|(_, states)| &states[a][..])
                        .unwrap_or_else(|| draw_states.of(a));
                    for state in states {''')
edit('''            states_recorded.count(&device.shared().counts);''','''              }
            states_recorded.count(&device.shared().counts);''')
edit('''                if !self.layout.sample_positions.is_empty() && self.current.positioned.is_none() {''','''                if !replay_samples && !self.layout.sample_positions.is_empty()
                    && self.current.positioned.is_none() {''')
edit('''        let made = match draw_pipeline {
            DrawPipeline::Made(made) => Some(made),''','''        if !sample_recordings.is_empty() {
            device.degrade(Degradation::ShiftedSampleRasterization, || (
                format!("a {}-sample draw with custom positions", self.layout.samples),
                "one masked hardware draw per sample; viewport translation changes actual coverage and depth/stencil, not only a shader position LUT".to_owned(),
            ));
            for (made, _) in sample_recordings { self.core.retain(made); }
        }
        let made = match draw_pipeline {
            DrawPipeline::Made(made) => Some(made),''')
edit('''            if let Some(steps) = capture_steps {
                self.core.retain(Arc::clone(&steps.pipeline));''','''            if let Some(steps) = capture_steps {
                device.degrade(Degradation::VertexComputeCapture, || (
                    "a vertex SIMD draw".to_owned(),
                    "compute capture, producer-to-vertex barrier, and one pull draw; tail invocations do not execute the vertex body".to_owned(),
                ));
                self.core.retain(Arc::clone(&steps.pipeline));''')
p.write_text(s)
print('pan200 masked sample recordings integrated')
