// The environment drawn behind the scene (ADR 0040), as its sharpest level, with one triangle
// covering the window, at the far end of depth: drawn after opaque surfaces, it is only shaded
// where none is, which matters on phones. Output is linear, into the HDR target or the window.

#include "environment.slang"

struct SkyConstants {
    // From clip space to world space: the inverse of the camera's view-projection, as columns.
    float4 clip_to_world[4];
    // The camera's position.
    float4 eye;
    // x: the environment's intensity. z and w: its rotation's cosine and sine.
    float4 environment;
};

[[vk::push_constant]]
ConstantBuffer<SkyConstants> constants : register(b1);

struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 clip : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.99999, 1.0);
    output.clip = input.position;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    const float4 clip = float4(input.clip, 1.0, 1.0);
    const float4 world = constants.clip_to_world[0] * clip.x + constants.clip_to_world[1] * clip.y +
                         constants.clip_to_world[2] * clip.z + constants.clip_to_world[3] * clip.w;
    const float3 direction = world.xyz / world.w - constants.eye.xyz;
    const float3 light =
        environment_light(direction, constants.environment.zw, 0.0) * constants.environment.x;
    return float4(light, 1.0);
}
