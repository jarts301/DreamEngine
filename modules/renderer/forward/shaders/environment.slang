// Reading the scene's environment (ADR 0040), shared by forward.slang and sky.slang: an HDR image
// of the surroundings in equirectangular form, longitude across and latitude down, with +Y up.
// Level 0 is sharp; each next level is the image as a rougher surface reflects it, up to fully
// rough at the second to last; the last level is what a matte surface receives from each side.
// Texture slot 7: binding 9 / registers t7 and s7.

[[vk::binding(9, 0)]]
Sampler2D environment_map : register(t7) : register(s7);

static const float k_environment_pi = 3.14159265;

// Where a direction falls in the image, turned by the environment's rotation around +Y, given as
// its cosine and sine.
float2 environment_uv(float3 direction, float2 turn) {
    const float3 d = normalize(direction);
    const float x = d.x * turn.x - d.z * turn.y;
    const float z = d.x * turn.y + d.z * turn.x;
    const float u = atan2(x, -z) / (2.0 * k_environment_pi) + 0.5;
    const float v = acos(clamp(d.y, -1.0, 1.0)) / k_environment_pi;
    return float2(u, v);
}

float3 environment_light(float3 direction, float2 turn, float level) {
    return environment_map.SampleLevel(environment_uv(direction, turn), level).rgb;
}
