// The renderer's standard opaque surface: one directional light, ambient light and a base color
// texture. Output is linear, and the window's sRGB format encodes it.
//
// Every shader uses the same bindings, for both graphics APIs (ADR 0013): the uniform block is
// binding 0 / register b0, the texture is binding 1 / registers t0 and s0, and the push constants
// are register b1. Vertex inputs are declared in location order.

struct FrameUniforms {
    float4x4 view_projection;
    float4 camera_position;
    float4 direction_to_light;
    float4 light_color_and_ambient;
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

[[vk::binding(1, 0)]]
Sampler2D base_color_texture;

[[vk::push_constant]]
ConstantBuffer<DrawConstants> draw : register(b1);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world_position : POSITION;
    float3 world_normal : NORMAL;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    const float4 world = mul(draw.model, float4(input.position, 1.0));

    VertexOutput output;
    output.position = mul(frame.view_projection, world);
    output.world_position = world.xyz;
    output.world_normal = draw.normal_x.xyz * input.normal.x + draw.normal_y.xyz * input.normal.y +
                          draw.normal_z.xyz * input.normal.z;
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    const float4 albedo = base_color_texture.Sample(input.uv) * draw.base_color;

    const float3 normal = normalize(input.world_normal);
    const float3 to_light = frame.direction_to_light.xyz;
    const float3 to_camera = normalize(frame.camera_position.xyz - input.world_position);
    const float3 halfway = normalize(to_light + to_camera);

    const float3 light = frame.light_color_and_ambient.rgb;
    const float ambient = frame.light_color_and_ambient.a;

    const float diffuse = saturate(dot(normal, to_light));
    const float specular = diffuse > 0.0 ? pow(saturate(dot(normal, halfway)), 32.0) * 0.15 : 0.0;

    return float4(albedo.rgb * (light * diffuse + ambient) + light * specular, albedo.a);
}
