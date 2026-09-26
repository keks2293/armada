#version 450
/* Float-sampler mirror of the rgba probe: same image/view, but a FLOAT
 * texelFetch (sampler2D), scaled back to 8-bit. This is the game-equivalent
 * path (D3D12 texture2D / VK sampled float). If this is EXACT while the int
 * probe (cast_rgba, usampler2D) is TRANSFORMED, the 8888 decode anomaly is
 * in the integer-sampling path only - float-sampling games are unaffected.
 * Embedded SPIR-V: cast_f32_spv[] in rp6-vkd3d-sparse-test.c
 * (regenerate: glslc -fshader-stage=compute -o cast_f32.spv this-file)
 */
layout(local_size_x = 32, local_size_y = 32) in;
layout(set = 0, binding = 0) uniform sampler2D img;
layout(set = 0, binding = 1) buffer Out { uint data[]; } outb;
void main() {
    ivec2 uv = ivec2(gl_GlobalInvocationID.xy);
    vec4 c = texelFetch(img, uv, 0);
    outb.data[uv.y * 512 + uv.x] = uint(c.x * 255.0 + 0.5)
        | (uint(c.y * 255.0 + 0.5) << 8)
        | (uint(c.z * 255.0 + 0.5) << 16)
        | (uint(c.w * 255.0 + 0.5) << 24);
}
