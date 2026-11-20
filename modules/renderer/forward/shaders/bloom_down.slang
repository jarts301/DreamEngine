// Bloom, first half (ADR 0040): halves the HDR scene, then each half again, with Jimenez's 13-tap
// filter, which keeps the glow from flickering as things move. The first pass keeps only light above
// the threshold, with a soft knee, and averages each group of taps by its brightness, so a single
// very bright pixel can't make a flashing square.
//
// The source is texture slot 1, clamped: binding 2 / registers t1 and s1.

struct BloomConstants {
    // xy: one texel of the source, in texture coordinates. z: the threshold. w: 1 for the first
    // pass.
    float4 settings;
};

[[vk::push_constant]]
ConstantBuffer<BloomConstants> constants : register(b1);

[[vk::binding(2, 0)]]
Sampler2D source : register(t1) : register(s1);

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

float3 tap(float2 uv, float2 offset) {
    // Very bright texels, such as the sun's highlight, are capped so they don't take over.
    return min(source.SampleLevel(uv + offset * constants.settings.xy, 0.0).rgb, 64.0);
}

float brightness(float3 color) {
    return max(color.r, max(color.g, color.b));
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    const float2 uv = input.uv;
    const float3 a = tap(uv, float2(-2.0, -2.0));
    const float3 b = tap(uv, float2(0.0, -2.0));
    const float3 c = tap(uv, float2(2.0, -2.0));
    const float3 d = tap(uv, float2(-2.0, 0.0));
    const float3 e = tap(uv, float2(0.0, 0.0));
    const float3 f = tap(uv, float2(2.0, 0.0));
    const float3 g = tap(uv, float2(-2.0, 2.0));
    const float3 h = tap(uv, float2(0.0, 2.0));
    const float3 i = tap(uv, float2(2.0, 2.0));
    const float3 j = tap(uv, float2(-1.0, -1.0));
    const float3 k = tap(uv, float2(1.0, -1.0));
    const float3 l = tap(uv, float2(-1.0, 1.0));
    const float3 m = tap(uv, float2(1.0, 1.0));

    // Five overlapping groups of four taps, the middle one counting for half.
    float3 groups[5] = {(j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25,
                        (d + e + g + h) * 0.25, (e + f + h + i) * 0.25};
    float weights[5] = {0.5, 0.125, 0.125, 0.125, 0.125};
    const bool first = constants.settings.w > 0.5;
    float3 color = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int index = 0; index < 5; ++index) {
        // On the first pass, brighter groups count for less, so one very bright texel can't flash.
        const float weight = first ? weights[index] / (1.0 + brightness(groups[index]))
                                   : weights[index];
        color += groups[index] * weight;
        total += weight;
    }
    color /= total;
    if (first) {
        // Only what is above the threshold glows, easing in over a knee of half of it.
        const float threshold = constants.settings.z;
        const float knee = max(threshold * 0.5, 1e-4);
        const float bright = brightness(color);
        float soft = clamp(bright - threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee);
        color *= max(soft, bright - threshold) / max(bright, 1e-4);
    }
    return float4(color, 1.0);
}
