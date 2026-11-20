// Particles (ADR 0038): one square per instance, turned to face the camera, textured or drawn as a
// soft round dot, unlit. Output is linear and premultiplied by alpha, for the premultiplied blend:
// an alpha particle covers what is behind it by its opacity, and an additive one writes an alpha of
// 0, so it only adds its light.

// The start of the renderer's frame block; see forward.slang.
struct FrameUniforms {
    float4x4 view_projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

[[vk::binding(1, 0)]]
Sampler2D particle_texture;

struct ParticleConstants {
    // The camera's right and up directions in world space, unit length.
    float4 right;
    float4 up;
    // x: 1 to read the texture, 0 to draw a soft dot.
    float4 settings;
};

[[vk::push_constant]]
ConstantBuffer<ParticleConstants> constants : register(b1);

struct VertexInput {
    // A corner of the square, from -0.5 to 0.5.
    [[vk::location(0)]] float2 corner : POSITION;
    // Per instance: the middle in world space and the size across, the linear color with opacity,
    // and in x and y the cosine and sine of its rotation, in z 1 if additive.
    [[vk::location(1)]] float4 position_size : TEXCOORD1;
    [[vk::location(2)]] float4 color : TEXCOORD2;
    [[vk::location(3)]] float4 turn : TEXCOORD3;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
    float additive : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    const float2 turned = float2(input.corner.x * input.turn.x - input.corner.y * input.turn.y,
                                 input.corner.x * input.turn.y + input.corner.y * input.turn.x);
    const float3 world = input.position_size.xyz +
                         (constants.right.xyz * turned.x + constants.up.xyz * turned.y) *
                             input.position_size.w;
    VertexOutput output;
    output.position = mul(frame.view_projection, float4(world, 1.0));
    output.uv = float2(input.corner.x + 0.5, 0.5 - input.corner.y);
    output.color = input.color;
    output.additive = input.turn.z;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    float4 shade;
    if (constants.settings.x > 0.5) {
        shade = particle_texture.Sample(input.uv);
    } else {
        // Brightest in the middle, fading smoothly to nothing at the edge.
        const float edge = saturate(1.0 - length(input.uv * 2.0 - 1.0));
        shade = float4(1.0, 1.0, 1.0, edge * edge * (3.0 - 2.0 * edge));
    }
    const float4 color = input.color * shade;
    const float alpha = saturate(color.a);
    return float4(color.rgb * alpha, input.additive > 0.5 ? 0.0 : alpha);
}
