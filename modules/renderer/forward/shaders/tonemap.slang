// Turns the renderer's HDR scene into the window's colors (ADR 0028): exposure, then Khronos PBR
// Neutral, which keeps colors as the materials have them up to about 0.8 and rolls brighter ones off
// toward white. Output is linear, and the window's sRGB format encodes it.
//
// The scene is texture slot 1: binding 2 / registers t1 and s1, and the settings are the push
// constants, register b1 (ADR 0013, 0027). With bloom (ADR 0040), its largest image, a quarter of
// the scene's size, is slot 3: binding 4 / registers t3 and s3, added here, which saves a pass over
// the whole scene.

struct ToneMapConstants {
    // x: exposure, a factor on the scene's colors. y: 1 for PBR Neutral, 0 to only clamp. z: how
    // much of the bloom to add, 0 without it.
    float4 settings;
    // xy: one texel of the bloom image, in texture coordinates.
    float4 bloom_texel;
};

[[vk::push_constant]]
ConstantBuffer<ToneMapConstants> constants : register(b1);

[[vk::binding(2, 0)]]
Sampler2D scene : register(t1) : register(s1);

[[vk::binding(4, 0)]]
Sampler2D bloom : register(t3) : register(s3);

// Slot 3's sampler repeats, so taps are kept inside the image, where the edges stay clean.
float3 bloom_tap(float2 uv, float2 offset) {
    const float2 texel = constants.bloom_texel.xy;
    const float2 at = clamp(uv + offset * texel, texel * 0.5, 1.0 - texel * 0.5);
    return bloom.SampleLevel(at, 0.0).rgb;
}

struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.5, 1.0);
    output.uv = input.uv;
    return output;
}

// The Khronos PBR Neutral tone mapper, as its specification gives it.
float3 pbr_neutral(float3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) {
        return color;
    }
    const float d = 1.0 - start_compression;
    const float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    float3 color = scene.Sample(input.uv).rgb;
    if (constants.settings.z > 0.0) {
        // Already blurred as it was added up, so one filtered read is smooth enough, and reading
        // more at every pixel of the window costs phones too much.
        color += bloom_tap(input.uv, float2(0.0, 0.0)) * constants.settings.z;
    }
    color *= constants.settings.x;
    const float3 mapped = constants.settings.y > 0.5 ? pbr_neutral(color) : color;
    return float4(saturate(mapped), 1.0);
}
