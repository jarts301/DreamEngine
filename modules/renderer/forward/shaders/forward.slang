// The renderer's standard opaque surface, in glTF's metallic-roughness model (ADR 0033): one
// directional light, which the shadow map can hide, and even ambient light from every side. A
// material has a base color, metallic and roughness, a normal map, ambient occlusion and emitted
// light, each a factor times a texture. Output is linear, into the HDR target or the window.
//
// Every shader uses the same bindings, for both graphics APIs (ADR 0013): the uniform block is
// binding 0 / register b0, the base color texture is binding 1 / registers t0 and s0. The shadow
// map is texture slot 2: binding 3 / registers t2 and s2 (ADR 0027). The material's other textures
// are slots 3 to 6. Vertex inputs are declared in location order, the model's first, then each
// instance's (ADR 0032).

struct FrameUniforms {
    float4x4 view_projection;
    float4 camera_position;
    float4 direction_to_light;
    float4 light_color_and_ambient;
    // The cascade the shadow pass draws, which this shader doesn't read (ADR 0029).
    float4x4 light_view_projection;
    // From world space to each cascade's part of the shadow map, in its clip space: four columns
    // per cascade. Not an array of matrices, which Adreno drivers read wrongly from uniform blocks.
    float4 cascade_columns[16];
    // How far from the camera each cascade reaches, along its view.
    float4 cascade_far;
    // How far to move a surface along its normal before looking it up, per cascade, in world units.
    float4 cascade_normal_offset;
    // x: how many cascades, 0 without shadows. y and w: one texel of the shadow map, across and
    // down, in texture coordinates. z: a depth bias.
    float4 shadow_settings;
    float4 camera_forward;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

[[vk::binding(1, 0)]]
Sampler2D base_color_texture;

[[vk::binding(3, 0)]]
Sampler2DShadow shadow_map : register(t2) : register(s2);

// Linear data. A missing one reads as white, except the normal map, which reads as flat.
[[vk::binding(4, 0)]]
Sampler2D normal_texture : register(t3) : register(s3);
[[vk::binding(5, 0)]]
Sampler2D metallic_roughness_texture : register(t4) : register(s4);
[[vk::binding(6, 0)]]
Sampler2D occlusion_texture : register(t5) : register(s5);
// sRGB colors, like the base color.
[[vk::binding(7, 0)]]
Sampler2D emissive_texture : register(t6) : register(s6);

static const float k_pi = 3.14159265;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    // xyz along the texture's +U, w which way the bitangent points. Zero without tangents.
    [[vk::location(3)]] float4 tangent : TANGENT;
    // Each instance's: the columns of its model matrix and of its normals' matrix, and its
    // material's factors.
    [[vk::location(4)]] float4 model_x : MODEL0;
    [[vk::location(5)]] float4 model_y : MODEL1;
    [[vk::location(6)]] float4 model_z : MODEL2;
    [[vk::location(7)]] float4 model_w : MODEL3;
    [[vk::location(8)]] float4 normal_x : NORMALS0;
    [[vk::location(9)]] float4 normal_y : NORMALS1;
    [[vk::location(10)]] float4 normal_z : NORMALS2;
    [[vk::location(11)]] float4 base_color : COLOR0;
    // Metallic, roughness, normal scale, occlusion strength.
    [[vk::location(12)]] float4 material : MATERIAL0;
    [[vk::location(13)]] float4 emissive : EMISSIVE0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world_position : POSITION;
    float3 world_normal : NORMAL;
    float4 world_tangent : TANGENT;
    float2 uv : TEXCOORD0;
    nointerpolation float4 base_color : COLOR0;
    nointerpolation float4 material : MATERIAL0;
    nointerpolation float4 emissive : EMISSIVE0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    const float3 p = input.position;
    const float4 world = input.model_x * p.x + input.model_y * p.y + input.model_z * p.z +
                         input.model_w;

    VertexOutput output;
    output.position = mul(frame.view_projection, world);
    output.world_position = world.xyz;
    output.world_normal = input.normal_x.xyz * input.normal.x + input.normal_y.xyz * input.normal.y +
                          input.normal_z.xyz * input.normal.z;
    // Tangents move with the surface, so they take the model matrix itself. A mirroring matrix
    // turns the bitangent around too.
    const float3 tangent = input.model_x.xyz * input.tangent.x + input.model_y.xyz * input.tangent.y +
                           input.model_z.xyz * input.tangent.z;
    const float mirrored =
        dot(cross(input.model_x.xyz, input.model_y.xyz), input.model_z.xyz) < 0.0 ? -1.0 : 1.0;
    output.world_tangent = float4(tangent, input.tangent.w * mirrored);
    output.uv = input.uv;
    output.base_color = input.base_color;
    output.material = input.material;
    output.emissive = input.emissive;
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
    const int column = cascade * 4;
    const float4 clip = frame.cascade_columns[column] * moved.x +
                        frame.cascade_columns[column + 1] * moved.y +
                        frame.cascade_columns[column + 2] * moved.z + frame.cascade_columns[column + 3];
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

// The surface's normal: the normal map's, turned from the tangent space at this point into the
// world. Without a tangent there, the normal map can't be placed, and the mesh's normal is kept.
float3 surface_normal(VertexOutput input, float3 geometric) {
    const float4 tangent = input.world_tangent;
    if (dot(tangent.xyz, tangent.xyz) < 1e-12) {
        return geometric;
    }
    // Made square to the normal again, since both were blended across the triangle.
    const float3 t = normalize(tangent.xyz - geometric * dot(geometric, tangent.xyz));
    const float3 b = cross(geometric, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    float3 mapped = normal_texture.Sample(input.uv).xyz * 2.0 - 1.0;
    mapped.xy *= input.material.z;
    return normalize(t * mapped.x + b * mapped.y + geometric * mapped.z);
}

// GGX, the spread of the surface's microscopic facets, with alpha = roughness squared.
float distribution(float n_dot_h, float alpha) {
    const float alpha2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / (k_pi * d * d);
}

// How much of the facets the light and the eye both see, with Smith's height-correlated GGX term,
// already divided by 4 n.l n.v.
float visibility(float n_dot_v, float n_dot_l, float alpha) {
    const float alpha2 = alpha * alpha;
    const float from_view = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    const float from_light = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(from_view + from_light, 1e-5);
}

float3 fresnel(float3 f0, float cosine) {
    const float f = pow(1.0 - cosine, 5.0);
    return f0 + (1.0 - f0) * f;
}

// How much ambient light a surface reflects as specular, averaged over every direction it comes
// from: Karis's fit for mobile, standing in for a lookup table.
float3 ambient_specular(float3 f0, float roughness, float n_dot_v) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const float2 scale_bias = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scale_bias.x + scale_bias.y;
}

// Light's units: a white matte surface facing the sun reflects `light_color * intensity`, so the
// usual 1/pi of the diffuse term and the pi of the light's irradiance cancel out.
[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    const float4 base_color = base_color_texture.Sample(input.uv) * input.base_color;
    const float4 metallic_roughness = metallic_roughness_texture.Sample(input.uv);
    const float metallic = saturate(input.material.x * metallic_roughness.b);
    // Kept above zero, where the highlight would be a single point.
    const float roughness = clamp(input.material.y * metallic_roughness.g, 0.045, 1.0);
    const float alpha = roughness * roughness;
    const float occlusion = 1.0 + input.material.w * (occlusion_texture.Sample(input.uv).r - 1.0);
    const float3 emissive = emissive_texture.Sample(input.uv).rgb * input.emissive.rgb;

    const float3 geometric = normalize(input.world_normal);
    const float3 normal = surface_normal(input, geometric);
    const float3 to_light = frame.direction_to_light.xyz;
    const float3 to_camera = normalize(frame.camera_position.xyz - input.world_position);
    const float n_dot_v = max(dot(normal, to_camera), 1e-4);

    // Dielectrics reflect 4% straight on; metals reflect their color and have no diffuse.
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), base_color.rgb, metallic);
    const float3 diffuse_color = base_color.rgb * (1.0 - metallic);

    const float3 light = frame.light_color_and_ambient.rgb;
    const float ambient = frame.light_color_and_ambient.a;

    float3 color = float3(0.0, 0.0, 0.0);
    const float n_dot_l = saturate(dot(normal, to_light));
    // Faces turned from the light stay dark, however their normal map tilts them.
    if (n_dot_l > 0.0 && dot(geometric, to_light) > 0.0) {
        const float3 halfway = normalize(to_light + to_camera);
        const float n_dot_h = saturate(dot(normal, halfway));
        const float3 f = fresnel(f0, saturate(dot(to_light, halfway)));
        const float3 specular = distribution(n_dot_h, alpha) *
                                visibility(n_dot_v, n_dot_l, alpha) * f * k_pi;
        const float3 diffuse = diffuse_color * (1.0 - f);
        // The shadow lookup moves along the mesh's normal, not the mapped one.
        color = (diffuse + specular) * light * (n_dot_l * sunlight(input.world_position, geometric));
    }

    // Ambient light, the same from every side, which occlusion darkens.
    color += (diffuse_color + ambient_specular(f0, roughness, n_dot_v)) * (ambient * occlusion);
    color += emissive;
    return float4(color, base_color.a);
}
