// shadow.slang for materials whose alpha cuts holes in them (ADR 0034): the shadow map leaves out
// pixels whose base color alpha is below the material's cutoff, so leaves and fences cast the
// shadow of their shapes. Each instance's data is forward.slang's, of which this reads the model
// matrix, the alpha cutoff in the normals' first column, and the base color.

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

[[vk::binding(1, 0)]]
Sampler2D base_color_texture;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 model_x : MODEL0;
    [[vk::location(3)]] float4 model_y : MODEL1;
    [[vk::location(4)]] float4 model_z : MODEL2;
    [[vk::location(5)]] float4 model_w : MODEL3;
    // Only w, the alpha cutoff, is read.
    [[vk::location(6)]] float4 normal_x : NORMALS0;
    [[vk::location(7)]] float4 base_color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float alpha : COLOR0;
    nointerpolation float alpha_cutoff : CUTOFF0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    const float3 p = input.position;
    const float4 world = input.model_x * p.x + input.model_y * p.y + input.model_z * p.z +
                         input.model_w;
    VertexOutput output;
    output.position = mul(frame.light_view_projection, world);
    output.uv = input.uv;
    output.alpha = input.base_color.a;
    output.alpha_cutoff = input.normal_x.w;
    return output;
}

// Depth only, with no color to write: the discard is all it does.
[shader("fragment")]
void fragment_main(VertexOutput input) {
    if (base_color_texture.Sample(input.uv).a * input.alpha < input.alpha_cutoff) {
        discard;
    }
}
