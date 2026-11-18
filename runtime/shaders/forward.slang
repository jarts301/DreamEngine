// The renderer's standard opaque surface: one directional light, which the shadow map can hide,
// ambient light and a base color texture. Output is linear, into the HDR target or the window.
//
// Every shader uses the same bindings, for both graphics APIs (ADR 0013): the uniform block is
// binding 0 / register b0, the texture is binding 1 / registers t0 and s0, and the push constants
// are register b1. The shadow map is texture slot 2: binding 3 / registers t2 and s2 (ADR 0027).
// Vertex inputs are declared in location order.

struct FrameUniforms {
    float4x4 view_projection;
    float4 camera_position;
    float4 direction_to_light;
    float4 light_color_and_ambient;
    // From world space to the shadow map's clip space (ADR 0029).
    float4x4 light_view_projection;
    // x: 1 with a shadow map. y: one texel of it, in texture coordinates. z: a depth bias. w: how
    // far to move a surface along its normal before looking it up, in world units.
    float4 shadow_settings;
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

[[vk::binding(3, 0)]]
Sampler2DShadow shadow_map : register(t2) : register(s2);

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
    float4 light_position : TEXCOORD1;
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
    // Moved a little along the normal, which keeps lit surfaces from shadowing themselves.
    const float3 offset = normalize(output.world_normal) * frame.shadow_settings.w;
    output.light_position = mul(frame.light_view_projection, float4(world.xyz + offset, 1.0));
    return output;
}

// How much of the sun reaches a point: 1 lit, 0 in shadow, and between at the edges, from nine
// filtered comparisons around it. Outside the shadow map, everything is lit.
float sunlight(float4 light_position) {
    if (frame.shadow_settings.x < 0.5) {
        return 1.0;
    }
    // Clip space has +Y up and depth from 0 to 1, and a texture's first row is its top.
    const float3 position = light_position.xyz / light_position.w;
    const float2 uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || position.z > 1.0) {
        return 1.0;
    }
    const float depth = position.z - frame.shadow_settings.z;
    const float texel = frame.shadow_settings.y;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += shadow_map.SampleCmpLevelZero(uv + float2(x, y) * texel, depth);
        }
    }
    return lit / 9.0;
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

    const float sun = sunlight(input.light_position);
    const float diffuse = saturate(dot(normal, to_light)) * sun;
    const float specular =
        diffuse > 0.0 ? pow(saturate(dot(normal, halfway)), 32.0) * 0.15 * sun : 0.0;

    return float4(albedo.rgb * (light * diffuse + ambient) + light * specular, albedo.a);
}
