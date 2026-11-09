// FSR 1 look, second pass: RCAS (robust contrast adaptive sharpening) of the picture fs_easu (blit.wgsl) drew at the window's size, into
// the window. Ported from the 32-bit FsrRcasF of AMD's ffx_fsr1.h (FidelityFX-FSR 1.0.2, MIT license, Copyright (c) 2021 Advanced
// Micro Devices, Inc.; the notice is in docs/apple/fsr1-license.txt), without the optional denoise. AMD's approximate reciprocals are
// exact ones here, with the divisions kept away from zero where the original would make an infinity of a black or white ring.

struct Rcas {
    width: u32,
    height: u32,
    sharpness: f32,  // FsrRcasCon's con.x: 2^-stops, 1.0 is the most
    unused: u32,
}

@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var<uniform> rcas: Rcas;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

// The original limits the negative lobe here: at the edge of unnatural sharpening.
const RCAS_LIMIT: f32 = 0.25 - 1.0 / 16.0;

fn load(p: vec2<i32>) -> vec3<f32> {
    let q = clamp(p, vec2<i32>(0), vec2<i32>(i32(rcas.width) - 1, i32(rcas.height) - 1));
    return textureLoad(source, q, 0).rgb;
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    let sp = vec2<i32>(floor(in.uv * vec2<f32>(f32(rcas.width), f32(rcas.height))));
    //    b
    //  d e f
    //    h
    let b = load(sp + vec2<i32>(0, -1));
    let d = load(sp + vec2<i32>(-1, 0));
    let e = load(sp);
    let f = load(sp + vec2<i32>(1, 0));
    let h = load(sp + vec2<i32>(0, 1));
    // min and max of the ring
    let mn4 = min(min(b, d), min(f, h));
    let mx4 = max(max(b, d), max(f, h));
    // limiters: how far the lobe may go before a channel would leave 0..1
    let hit_min = min(mn4, e) / max(4.0 * mx4, vec3<f32>(1.0e-6));
    let hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3<f32>(-1.0e-6));
    let lobe3 = max(-hit_min, hit_max);
    let lobe = max(-RCAS_LIMIT, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * rcas.sharpness;
    let pix = (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);
    return vec4<f32>(clamp(pix, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
