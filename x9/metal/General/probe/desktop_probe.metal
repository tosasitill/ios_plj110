#include <metal_stdlib>
using namespace metal;
kernel void x9_cache_roundtrip(device const uint *input [[buffer(0)]],
                               device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    output[i] = input[i] * 3u + 7u;
}
vertex float4 x9_fullscreen_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1.0,-1.0),float2(3.0,-1.0),float2(-1.0,3.0)};
    return float4(positions[id],0.0,1.0);
}
fragment float4 x9_magenta_fragment() { return float4(1.0,0.0,1.0,1.0); }
