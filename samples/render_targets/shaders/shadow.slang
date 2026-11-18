// Draws shadow casters into the shadow map, as the light sees them: depth only.
//
// Bindings as in ADR 0013 and 0027: the uniform block is binding 0 / register b0, and push
// constants are register b1.

struct FrameUniforms {
    float4x4 camera_view_projection;
    float4x4 light_view_projection;
};

struct DrawConstants {
    float4x4 model;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

[[vk::push_constant]]
ConstantBuffer<DrawConstants> draw : register(b1);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
};

[shader("vertex")]
float4 vertex_main(VertexInput input) : SV_Position {
    return mul(frame.light_view_projection, mul(draw.model, float4(input.position, 1.0)));
}

// Never used: the shadow pipeline has no fragment shader. Every shader file has both stages.
[shader("fragment")]
float4 fragment_main() : SV_Target {
    return float4(0.0, 0.0, 0.0, 1.0);
}
