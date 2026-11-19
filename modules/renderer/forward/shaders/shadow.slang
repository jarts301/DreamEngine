// Draws shadow casters into the shadow map, as the sun sees them: depth only (ADR 0029).
//
// The frame block and push constants are forward.slang's, with the same bindings.

struct FrameUniforms {
    float4x4 view_projection;
    float4 camera_position;
    float4 direction_to_light;
    float4 light_color_and_ambient;
    float4x4 light_view_projection;
    float4x4 cascades[4];
    float4 cascade_far;
    float4 cascade_normal_offset;
    float4 shadow_settings;
    float4 camera_forward;
};

struct DrawConstants {
    float4x4 model;
    float4 normal_x;
    float4 normal_y;
    float4 normal_z;
    float4 base_color;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

[[vk::push_constant]]
ConstantBuffer<DrawConstants> draw : register(b1);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
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
