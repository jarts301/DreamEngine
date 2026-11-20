// Bloom, second half (ADR 0040): each smaller image, blurred with a 3 x 3 tent, is added to the
// next larger one, and the largest to the HDR scene, scaled by the bloom's intensity. It writes an
// alpha of 0, so the premultiplied blend adds it to what is there.
//
// The source is texture slot 1, clamped: binding 2 / registers t1 and s1.

struct BloomConstants {
    // xy: one texel of the source, in texture coordinates. z: how much of it to add.
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
    return source.SampleLevel(uv + offset * constants.settings.xy, 0.0).rgb;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    const float2 uv = input.uv;
    float3 color = tap(uv, float2(0.0, 0.0)) * 4.0;
    color += (tap(uv, float2(-1.0, 0.0)) + tap(uv, float2(1.0, 0.0)) + tap(uv, float2(0.0, -1.0)) +
              tap(uv, float2(0.0, 1.0))) * 2.0;
    color += tap(uv, float2(-1.0, -1.0)) + tap(uv, float2(1.0, -1.0)) +
             tap(uv, float2(-1.0, 1.0)) + tap(uv, float2(1.0, 1.0));
    return float4(color * (constants.settings.z / 16.0), 0.0);
}
