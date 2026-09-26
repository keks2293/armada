#version 450
/* Read an R32_UINT image view and store the raw word: data[y*512+x] = word.
 * This is the other half of the view-cast pair: if the R32_UINT view of the
 * same memory returns exactly the words stored as RGBA8 bytes, the tiling
 * layout is format-identical (non-swap, non-UBWC).
 * Embedded SPIR-V: cast_r32_spv[] in rp6-vkd3d-sparse-test.c
 * (regenerate: glslc -o cast_r32.spv rp6-vkd3d-sparse-cast_r32.glsl)
 */
layout(local_size_x = 32, local_size_y = 32) in;
layout(set = 0, binding = 0) uniform usampler2D img;
layout(set = 0, binding = 1) buffer Out { uint data[]; } outb;
void main() {
    ivec2 uv = ivec2(gl_GlobalInvocationID.xy);
    uvec4 c = texelFetch(img, uv, 0);
    /* R32_UINT holds its 32-bit value in the first component */
    outb.data[uv.y * 512 + uv.x] = c.x;
}
