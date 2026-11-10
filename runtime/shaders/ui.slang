// Interface triangles from ui::DrawList. Positions are in pixels, and colors are sRGB, so they are
// converted to linear like everything else the renderer outputs.

struct UiConstants {
    // Pixels to clip space: position * scale + offset.
    float2 scale;
    float2 offset;
    // 1 if colors are premultiplied by alpha.
    float premultiplied;
};

[[vk::binding(1, 0)]]
Sampler2D ui_texture;

[[vk::push_constant]]
ConstantBuffer<UiConstants> constants;

struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

float3 to_linear(float3 srgb) {
    return select(srgb <= 0.04045, srgb / 12.92, pow((srgb + 0.055) / 1.055, 2.4));
}

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position * constants.scale + constants.offset, 0.0, 1.0);
    output.uv = input.uv;

    float4 color = input.color;
    if (constants.premultiplied > 0.5) {
        // Undoes the premultiplication, which was done in sRGB, then redoes it in linear.
        const float alpha = max(color.a, 1.0 / 255.0);
        color.rgb = to_linear(color.rgb / alpha) * color.a;
    } else {
        color.rgb = to_linear(color.rgb);
    }
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return ui_texture.Sample(input.uv) * input.color;
}
