// Copies the scene's color target to the window, over a triangle that covers it.
//
// The scene's color is slot 1: binding 2 / registers t1 and s1 (ADR 0027).

[[vk::binding(2, 0)]]
Sampler2D scene_color : register(t1) : register(s1);

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

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return float4(scene_color.Sample(input.uv).rgb, 1.0);
}
