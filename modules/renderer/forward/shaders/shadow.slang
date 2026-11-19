// Draws shadow casters into the shadow map, as the sun sees them: depth only (ADR 0029).
//
// The frame block is forward.slang's, with the same binding, and each instance's model matrix is
// read as forward.slang reads it (ADR 0032).

struct FrameUniforms {
    float4x4 view_projection;
    float4 camera_position;
    float4 direction_to_light;
    float4 light_color_and_ambient;
    float4x4 light_view_projection;
    float4 cascade_columns[16];
    float4 cascade_far;
    float4 cascade_normal_offset;
    float4 shadow_settings;
    float4 camera_forward;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    // The columns of each instance's model matrix.
    [[vk::location(1)]] float4 model_x : MODEL0;
    [[vk::location(2)]] float4 model_y : MODEL1;
    [[vk::location(3)]] float4 model_z : MODEL2;
    [[vk::location(4)]] float4 model_w : MODEL3;
};

[shader("vertex")]
float4 vertex_main(VertexInput input) : SV_Position {
    const float3 p = input.position;
    const float4 world = input.model_x * p.x + input.model_y * p.y + input.model_z * p.z +
                         input.model_w;
    return mul(frame.light_view_projection, world);
}

// Never used: the shadow pipeline has no fragment shader. Every shader file has both stages.
[shader("fragment")]
float4 fragment_main() : SV_Target {
    return float4(0.0, 0.0, 0.0, 1.0);
}
