#version 450
/* Read an 8-bit 4-channel image view (RGBA8 or BGRA8) and pack the bytes into
 * a little-endian word: data[y*512+x] = b0 | b1<<8 | b2<<16 | b3<<24, where
 * b0..b3 are the raw memory bytes of the texel in the VIEW format. For an
 * RGBA8 view of an image whose memory holds R,G,B,A this yields R|G<<8|B<<16|A<<24;
 * for a BGRA8 view of the same memory the channels come out swapped.
 * Embedded SPIR-V: cast_rgba_spv[] in rp6-vkd3d-sparse-test.c
 * (regenerate: glslc -o cast_rgba.spv rp6-vkd3d-sparse-cast_rgba.glsl)
 */
layout(local_size_x = 32, local_size_y = 32) in;
layout(set = 0, binding = 0) uniform usampler2D img;
layout(set = 0, binding = 1) buffer Out { uint data[]; } outb;
void main() {
    ivec2 uv = ivec2(gl_GlobalInvocationID.xy);
    uvec4 c = texelFetch(img, uv, 0);
    outb.data[uv.y * 512 + uv.x] = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
}
