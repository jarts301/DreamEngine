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
    // The cascade the shadow pass draws, which this shader doesn't read (ADR 0029).
    float4x4 light_view_projection;
    // From world space to each cascade's part of the shadow map, in its clip space.
    float4x4 cascades[4];
    // How far from the camera each cascade reaches, along its view.
    float4 cascade_far;
    // How far to move a surface along its normal before looking it up, per cascade, in world units.
    float4 cascade_normal_offset;
    // x: how many cascades, 0 without shadows. y and w: one texel of the shadow map, across and
    // down, in texture coordinates. z: a depth bias.
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

// How much of the sun reaches a point: 1 lit, 0 in shadow, and between at the edges, from nine
// filtered comparisons around it in the nearest cascade that covers it. Past the last cascade, and
// without shadows, everything is lit.
float sunlight(float3 world_position, float3 normal) {
    const int count = int(frame.shadow_settings.x + 0.5);
    if (count == 0) {
        return 1.0;
    }
    const float depth = dot(world_position - frame.camera_position.xyz, frame.camera_forward.xyz);
    int cascade = -1;
    for (int index = 3; index >= 0; --index) {
        if (index < count && depth < frame.cascade_far[index]) {
            cascade = index;
        }
    }
    if (cascade < 0) {
        return 1.0;
    }

    // Moved a little along the normal, which keeps lit surfaces from shadowing themselves.
    const float3 moved = world_position + normal * frame.cascade_normal_offset[cascade];
    const float4 clip = mul(frame.cascades[cascade], float4(moved, 1.0));
    // Clip space has +Y up and depth from 0 to 1, and a texture's first row is its top.
    const float3 position = clip.xyz / clip.w;
    const float2 uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
    if (position.z > 1.0) {
        return 1.0;
    }

    // The cascades lie side by side in the map, two across and then two down. The samples stay
    // inside this one's part.
    const float columns = count > 1 ? 2.0 : 1.0;
    const float rows = count > 2 ? 2.0 : 1.0;
    const float2 cell = float2(float(cascade % 2), float(cascade / 2));
    const float2 texel = frame.shadow_settings.yw;
    const float2 low = cell / float2(columns, rows) + texel * 1.5;
    const float2 high = (cell + 1.0) / float2(columns, rows) - texel * 1.5;

    const float reference = position.z - frame.shadow_settings.z;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float2 at = clamp(uv + float2(x, y) * texel, low, high);
            lit += shadow_map.SampleCmpLevelZero(at, reference);
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

    const float sun = sunlight(input.world_position, normal);
    const float diffuse = saturate(dot(normal, to_light)) * sun;
    const float specular =
        diffuse > 0.0 ? pow(saturate(dot(normal, halfway)), 32.0) * 0.15 * sun : 0.0;

    return float4(albedo.rgb * (light * diffuse + ambient) + light * specular, albedo.a);
}
