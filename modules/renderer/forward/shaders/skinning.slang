// Skinning for the forward renderer's shaders (ADR 0036), included by the ones compiled with
// DREAM_SKINNED defined; it isn't a shader of its own.
//
// The per-draw block holds each joint's skinning matrix as three rows of a 3x4 matrix, from the
// part's vertices to the model's space as the skeleton is posed. A vertex names up to four joints,
// as bytes read from 0 to 1, with weights that add up to 1.

struct JointBlock {
    float4 rows[768]; // 256 joints.
};

[[vk::binding(8, 0)]]
ConstantBuffer<JointBlock> joint_block : register(b2);

// Moves a position, a normal and a tangent by the vertex's joints.
void skin(float4 joints, float4 weights, inout float3 position, inout float3 normal,
          inout float3 tangent) {
    float4 row_x = float4(0.0, 0.0, 0.0, 0.0);
    float4 row_y = float4(0.0, 0.0, 0.0, 0.0);
    float4 row_z = float4(0.0, 0.0, 0.0, 0.0);
    for (int index = 0; index < 4; ++index) {
        const int joint = int(joints[index] * 255.0 + 0.5) * 3;
        const float weight = weights[index];
        row_x += joint_block.rows[joint] * weight;
        row_y += joint_block.rows[joint + 1] * weight;
        row_z += joint_block.rows[joint + 2] * weight;
    }
    const float4 p = float4(position, 1.0);
    position = float3(dot(row_x, p), dot(row_y, p), dot(row_z, p));
    normal = float3(dot(row_x.xyz, normal), dot(row_y.xyz, normal), dot(row_z.xyz, normal));
    tangent = float3(dot(row_x.xyz, tangent), dot(row_y.xyz, tangent), dot(row_z.xyz, tangent));
}

// The same, for a position alone, as the shadow passes need.
float3 skin_position(float4 joints, float4 weights, float3 position) {
    float3 normal = float3(0.0, 0.0, 1.0);
    float3 tangent = float3(1.0, 0.0, 0.0);
    skin(joints, weights, position, normal, tangent);
    return position;
}
