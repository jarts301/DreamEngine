// Debug lines drawn over the scene in a flat color. Output is linear, like forward.slang.

// The start of the renderer's frame block; see forward.slang.
struct FrameUniforms {
    float4x4 view_projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(frame.view_projection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return input.color;
}
