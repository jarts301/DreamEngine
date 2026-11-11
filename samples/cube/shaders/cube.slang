struct Constants {
    float4x4 model_view_projection;
};

// Push constants are register b1 on DirectX; see runtime/shaders/forward.slang.
[[vk::push_constant]]
ConstantBuffer<Constants> constants : register(b1);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(constants.model_view_projection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
