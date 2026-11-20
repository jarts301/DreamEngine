// 2D sprites and tile maps (ADR 0039): one rectangle of a texture per instance, placed in the 2D
// world's XY plane, drawn over the scene into the window after tone mapping, so their colors come
// out as the artist painted them. Output is linear, like everything the renderer draws.

// The start of the renderer's frame block; see forward.slang. Holds the 2D camera here.
struct FrameUniforms {
    float4x4 view_projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame : register(b0);

[[vk::binding(1, 0)]]
Sampler2D sprite_texture;

struct SpriteConstants {
    // x: 1 for the Pixel filter. y and z: the texture's width and height in pixels.
    float4 settings;
};

[[vk::push_constant]]
ConstantBuffer<SpriteConstants> constants : register(b1);

struct VertexInput {
    // A corner of the rectangle, from -0.5 to 0.5.
    [[vk::location(0)]] float2 corner : POSITION;
    // Per instance: position and size in world units; texture rectangle (left, top, right,
    // bottom); linear color with opacity; pivot, and the rotation's cosine and sine.
    [[vk::location(1)]] float4 position_size : TEXCOORD1;
    [[vk::location(2)]] float4 uv_rect : TEXCOORD2;
    [[vk::location(3)]] float4 color : TEXCOORD3;
    [[vk::location(4)]] float4 pivot_turn : TEXCOORD4;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    // From the bottom left at 0 to the top right at 1.
    const float2 unit = input.corner + 0.5;
    const float2 local = (unit - input.pivot_turn.xy) * input.position_size.zw;
    const float2 turned = float2(local.x * input.pivot_turn.z - local.y * input.pivot_turn.w,
                                 local.x * input.pivot_turn.w + local.y * input.pivot_turn.z);
    VertexOutput output;
    output.position =
        mul(frame.view_projection, float4(input.position_size.xy + turned, 0.0, 1.0));
    output.uv = float2(lerp(input.uv_rect.x, input.uv_rect.z, unit.x),
                       lerp(input.uv_rect.w, input.uv_rect.y, unit.y));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    float4 texel;
    if (constants.settings.x > 0.5) {
        // The middle of the nearest texel, where filtering returns that texel alone.
        const float2 size = constants.settings.yz;
        const float2 middle = (floor(input.uv * size) + 0.5) / size;
        texel = sprite_texture.SampleLevel(middle, 0.0);
    } else {
        texel = sprite_texture.Sample(input.uv);
    }
    return texel * input.color;
}
