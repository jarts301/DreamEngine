// Draws the scene into a float color target, lit by one light and darkened where the shadow map
// says something is between the surface and the light.
//
// Bindings as in ADR 0013 and 0027: the uniform block is binding 0 / register b0, push constants
// are register b1, and the shadow map is slot 2: binding 3 / registers t2 and s2.

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

[[vk::binding(3, 0)]]
Sampler2DShadow shadow_map : register(t2) : register(s2);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
    float4 light_position : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    const float4 world = mul(draw.model, float4(input.position, 1.0));
    VertexOutput output;
    output.position = mul(frame.camera_view_projection, world);
    output.color = input.color;
    output.light_position = mul(frame.light_view_projection, world);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    // Clip space has +Y up and depth from 0 to 1, and a texture's first row is its top.
    const float3 light = input.light_position.xyz / input.light_position.w;
    const float2 uv = float2(light.x * 0.5 + 0.5, 0.5 - light.y * 0.5);
    const float lit = shadow_map.SampleCmp(uv, light.z - 0.002);
    return float4(input.color * (0.3 + 0.7 * lit), 1.0);
}
