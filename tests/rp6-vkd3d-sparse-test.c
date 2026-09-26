/*
 * Sparse residency smoke test for Turnip on Adreno a7xx-gen2 (RP6/A740).
 * Verifies the actual sparse-residency image2D machinery works on the device:
 *   A) create SPARSE_BINDING|SPARSE_RESIDENCY image, bind one 64K tile,
 *      fill it, copy back; bound tile must read back exactly and unbound
 *      regions must read as 0 (PRR / NonResidentStrict semantics).
 *   B) alias the same 64K memory into an R32_UINT sparse image (informative;
 *      storage tiling differs per format, so a clean comparison needs a
 *      pipeline-based view-cast test).
 *   C) fallback probe for a MUTABLE sparse image with format list
 *      {RGBA8, R32_UINT}. On A740 the driver accepts it and base-format
 *      readback stays clean - but that is NOT evidence that the guarded case
 *      is sound: this list has no swaps, so the image is not force-lined and
 *      never reaches the hole. The force-linear case is S below, and there
 *      the data is broken on stock.
 *   D) view-cast probe: on the C image
 *      (base RGBA8, list {RGBA8, R32_UINT}) create RGBA8 and R32_UINT views,
 *      fill through one format, read back through the other with a compute
 *      shader. Stock result: d1/d3 (R32-INT) exact -> the tiled non-UBWC
 *      layout is format-identical, cast works; d2 (8888-INT) TRANSFORMED,
 *      but G/H/K/M/N/O/P proved that transform to be the integer-sampler
 *      decode anomaly, not cast/layout corruption.
 *   E) MUTABLE sparse with swap list {RGBA8, BGRA8}: on gen2 this trips
 *      format_list_has_swaps() -> force_linear_tile (tu_image.cc), i.e. the
 *      linear+sparse path guarded by the assert next to it. Probes whether
 *      the resulting image still functions (documents the latent hole).
 *   F) MUTABLE sparse WITHOUT a format list - the exact vkd3d-proton pattern
 *      named in MR !32671. NULL list -> format_list_has_swaps()=true -> same
 *      force_linear path, probed the same way.
 *   G/H/K/D0/D3 control probes isolating the trigger of D's d2 transform:
 *      G)  non-sparse non-mutable TILED 8888: R2D fill2 + TP-8888 shader read
 *          (expect exact = baseline "normal tiled 8888 is raw");
 *      H)  non-sparse MUTABLE {RGBA8, R32_UINT} (D's image class minus sparse):
 *          d1' = R2D fill1 + TP-R32 read, d2' = R2D fill2 + TP-8888 read;
 *      K)  SPARSE non-mutable 8888 (probe A's image class, shader-read):
 *          R2D fill2 into the bound granule + TP-8888 read;
 *      D0) TP-8888 read of the C/D image BEFORE any fill (still holds the
 *          0x37 tile bound in C) - what the 8888 decode does to constant data;
 *      D3) TP-R32 read of the C/D image AFTER d2 with no rewrite - ground
 *          truth that memory still holds fill2's raw words.
 * Trigger matrix (stock 26.2.2): every INT 8888 TP read of a UNORM image is
 * TRANSFORMED (G/H-d2'/K/M/D-d2, incl. constant data D0), every FLOAT 8888
 * TP read is EXACT (N/O/P), R32-INT reads EXACT (H-d1'/D-d1/D3), R2D 8888
 * EXACT (G2), and a REAL R8G8B8A8_UINT image + usampler (probe R) is EXACT
 * -> the "transform" is the invalid UNORM-image + usampler combination
 * (format-class mismatch -> the spec-defined "poison" texel value, a test
 * artifact - no driver bug); the D3D12 sampling paths (float, real UINT)
 * are clean, so the FL 12_0 gate-lift (P3) is safe for them.
 *
 * Build (the device has no compiler; use a container):
 *   podman run --rm -v "$(pwd)":/work:z -w /work fedora:44 bash -c \
 *     "dnf -y install gcc vulkan-headers vulkan-loader-devel && \
 *      gcc -O2 -o rp6-vkd3d-sparse-test rp6-vkd3d-sparse-test.c -lvulkan"
 * Three compute shaders are embedded as SPIR-V (glslc 2026.1, from
 * rp6-vkd3d-sparse-cast_{rgba,r32,f32}.glsl next to this file): D uses
 * {rgba,r32}, the G/H/K/M block also uses {f32}. If a shader changes,
 * recompile (glslc -fshader-stage=compute) and re-embed the words.
 * Run on the device against the packaged Turnip:
 *   ./rp6-vkd3d-sparse-test
 *
 * Observed on stock Mesa 26.2.2 / A740 (20-21.09.2026). Every result below is
 * from 26.2.2; the branch image is Mesa 26.2.3. A PASS,
 * B INCONCLUSIVE, C PASS, D FAIL (d1 bit-exact, d2 transformed), E/F create
 * OK (linear+sparse hole: query/create mismatch; data corruption of this
 * class is documented by the S probe below).
 * G-P: every INT 8888 TP read (tiled/linear, sparse/non-sparse, mutable,
 * incl. constant data D0) is TRANSFORMED, every FLOAT 8888 TP read (N/O/P,
 * incl. D's sparse+mutable class) and every R32 int read and R2D is EXACT.
 * Q (BGRA8 int): also TRANSFORMED, identically on stock and patched - the
 * same UNORM-image + usampler poison, format-agnostic (both R8G8B8A8 and
 * B8G8R8A8 hit). R (real R8G8B8A8_UINT + usampler): EXACT on stock and
 * patched -> real 8-bit int sampling works; the "transform" is the
 * format-class mismatch (spec "poison"), a test artifact, no Mesa bug
 * (branch rp6/bug-a740-tp-8888-decode closed without a patch).
 * S (BGRA uniform-swap sparse+mutable): stock create OK (the hole) but the
 * data is broken - the TP read loses 8 texels ((0, y), y=4..11, read PRR
 * zero) and the transfer readback of the filled granule mismatches
 * 12288/16384 words; 0004: create rejected (FEATURE_NOT_PRESENT, the P1
 * force-linear check - the list has swaps); 0005: create OK, transfer
 * 0/16384, both views EXACT (tiled+sparse). S2 (mixed swap {BGRA8,RGBA8},
 * NULL list): stock create OK (the hole); 0004/0005: query
 * FORMAT_NOT_SUPPORTED + create FEATURE_NOT_PRESENT.
 *   R) the VALID integer path: a REAL R8G8B8A8_UINT image sampled with the
 *      same usampler. EXACT on stock and patched -> real 8-bit int sampling
 *      WORKS; the G/H/K/M/Q "transform" is the invalid UNORM-image + usampler
 *      combo (format-class mismatch -> the spec-defined "poison" texel value,
 *      a test artifact, no driver bug).
 *   S) P4 (0005) probe: BGRA8 sparse+mutable with the UNIFORM-SWAP list
 *      {BGRA8_UNORM, BGRA8_UINT} - the upstream TODO case (tu_image.cc).
 *      stock: create silently linear+sparse (the E/F hole). 0004 (P1+P3):
 *      create rejected (FEATURE_NOT_PRESENT). 0005 (P4): create OK,
 *      tiled+sparse; the UINT (usampler) and UNORM (sampler) views must read
 *      back exact.
 *   S2) control: the mixed-swap list {BGRA8, RGBA8} and the NULL list stay
 *      rejected by query and create on 0004/0005 (stock: silent
 *      linear+sparse - the hole).
 *   T) P6 (0006) probe: NON-sparse MUTABLE {BGRA8_UNORM, BGRA8_UINT}
 *      (the D3D12 B8G8R8A8_TYPELESS class). Pre-0006: incompatible list
 *      (B8G8R8A8_UINT = UNKNOWN_COMPAT) + swaps -> LINEAR, memreq 0x100000.
 *      0006: B8G8R8A8 family unifies on the INT compat type -> list
 *      compatible -> UBWC+tiled (memreq 0x102000), both views still EXACT.
 * Verdict: the INT-8888 "transform" is the invalid UNORM-image + usampler
 * combination - a format-class mismatch whose result is the spec-defined
 * "poison" texel value (a test artifact, no driver/HW bug). Real integer
 * sampling (probe R: R8G8B8A8_UINT + usampler) is EXACT. The D3D12 sampling
 * paths (float, real UINT) are clean, so the FL 12_0 gate-lift (P3) is safe
 * for them. The E/F hole (P1) is a real query/create mismatch. All three Mesa
 * patches are implemented and verified on the device, not pending:
 *   P1+P3 = 0004  gate lift + hole closure      (probe S: create rejected)
 *   P4    = 0005  uniform-swap sparse lists tiled, the upstream TODO
 *                                             (probe S: create OK, both views
 *                                              EXACT)
 *   P6    = 0006  B8G8R8A8 family unified on the INT compat type
 *                                             (probe T: UBWC+tiled kept)
 * S2 pins the boundary: mixed-swap and NULL lists stay rejected on 0004/0005.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t cast_rgba_spv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000044, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00060010, 0x00000004,
    0x00000011, 0x00000020, 0x00000020, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004,
    0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365,
    0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572,
    0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00007675,
    0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044,
    0x00030005, 0x00000014, 0x00000063, 0x00030005, 0x00000018, 0x00676d69, 0x00030005, 0x0000001f,
    0x0074754f, 0x00050006, 0x0000001f, 0x00000000, 0x61746164, 0x00000000, 0x00040005, 0x00000021,
    0x6274756f, 0x00000000, 0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00040047, 0x00000018,
    0x00000021, 0x00000000, 0x00040047, 0x00000018, 0x00000022, 0x00000000, 0x00040047, 0x0000001e,
    0x00000006, 0x00000004, 0x00030047, 0x0000001f, 0x00000003, 0x00050048, 0x0000001f, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000021, 0x00000021, 0x00000001, 0x00040047, 0x00000021,
    0x00000022, 0x00000000, 0x00040047, 0x00000043, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015,
    0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020,
    0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040017,
    0x0000000e, 0x0000000a, 0x00000002, 0x00040017, 0x00000012, 0x0000000a, 0x00000004, 0x00040020,
    0x00000013, 0x00000007, 0x00000012, 0x00090019, 0x00000015, 0x0000000a, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x00000016, 0x00000015, 0x00040020,
    0x00000017, 0x00000000, 0x00000016, 0x0004003b, 0x00000017, 0x00000018, 0x00000000, 0x0004002b,
    0x00000006, 0x0000001b, 0x00000000, 0x0003001d, 0x0000001e, 0x0000000a, 0x0003001e, 0x0000001f,
    0x0000001e, 0x00040020, 0x00000020, 0x00000002, 0x0000001f, 0x0004003b, 0x00000020, 0x00000021,
    0x00000002, 0x0004002b, 0x0000000a, 0x00000022, 0x00000001, 0x00040020, 0x00000023, 0x00000007,
    0x00000006, 0x0004002b, 0x00000006, 0x00000026, 0x00000200, 0x0004002b, 0x0000000a, 0x00000028,
    0x00000000, 0x00040020, 0x0000002c, 0x00000007, 0x0000000a, 0x0004002b, 0x00000006, 0x00000031,
    0x00000008, 0x0004002b, 0x0000000a, 0x00000034, 0x00000002, 0x0004002b, 0x00000006, 0x00000037,
    0x00000010, 0x0004002b, 0x0000000a, 0x0000003a, 0x00000003, 0x0004002b, 0x00000006, 0x0000003d,
    0x00000018, 0x00040020, 0x00000040, 0x00000002, 0x0000000a, 0x0004002b, 0x0000000a, 0x00000042,
    0x00000020, 0x0006002c, 0x0000000b, 0x00000043, 0x00000042, 0x00000042, 0x00000022, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
    0x00000009, 0x00000007, 0x0004003b, 0x00000013, 0x00000014, 0x00000007, 0x0004003d, 0x0000000b,
    0x0000000f, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000010, 0x0000000f, 0x0000000f, 0x00000000,
    0x00000001, 0x0004007c, 0x00000007, 0x00000011, 0x00000010, 0x0003003e, 0x00000009, 0x00000011,
    0x0004003d, 0x00000016, 0x00000019, 0x00000018, 0x0004003d, 0x00000007, 0x0000001a, 0x00000009,
    0x00040064, 0x00000015, 0x0000001c, 0x00000019, 0x0007005f, 0x00000012, 0x0000001d, 0x0000001c,
    0x0000001a, 0x00000002, 0x0000001b, 0x0003003e, 0x00000014, 0x0000001d, 0x00050041, 0x00000023,
    0x00000024, 0x00000009, 0x00000022, 0x0004003d, 0x00000006, 0x00000025, 0x00000024, 0x00050084,
    0x00000006, 0x00000027, 0x00000025, 0x00000026, 0x00050041, 0x00000023, 0x00000029, 0x00000009,
    0x00000028, 0x0004003d, 0x00000006, 0x0000002a, 0x00000029, 0x00050080, 0x00000006, 0x0000002b,
    0x00000027, 0x0000002a, 0x00050041, 0x0000002c, 0x0000002d, 0x00000014, 0x00000028, 0x0004003d,
    0x0000000a, 0x0000002e, 0x0000002d, 0x00050041, 0x0000002c, 0x0000002f, 0x00000014, 0x00000022,
    0x0004003d, 0x0000000a, 0x00000030, 0x0000002f, 0x000500c4, 0x0000000a, 0x00000032, 0x00000030,
    0x00000031, 0x000500c5, 0x0000000a, 0x00000033, 0x0000002e, 0x00000032, 0x00050041, 0x0000002c,
    0x00000035, 0x00000014, 0x00000034, 0x0004003d, 0x0000000a, 0x00000036, 0x00000035, 0x000500c4,
    0x0000000a, 0x00000038, 0x00000036, 0x00000037, 0x000500c5, 0x0000000a, 0x00000039, 0x00000033,
    0x00000038, 0x00050041, 0x0000002c, 0x0000003b, 0x00000014, 0x0000003a, 0x0004003d, 0x0000000a,
    0x0000003c, 0x0000003b, 0x000500c4, 0x0000000a, 0x0000003e, 0x0000003c, 0x0000003d, 0x000500c5,
    0x0000000a, 0x0000003f, 0x00000039, 0x0000003e, 0x00060041, 0x00000040, 0x00000041, 0x00000021,
    0x0000001b, 0x0000002b, 0x0003003e, 0x00000041, 0x0000003f, 0x000100fd, 0x00010038
};

static const uint32_t cast_f32_spv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000053, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00060010, 0x00000004,
    0x00000011, 0x00000020, 0x00000020, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004,
    0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365,
    0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572,
    0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00007675,
    0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044,
    0x00030005, 0x00000015, 0x00000063, 0x00030005, 0x00000019, 0x00676d69, 0x00030005, 0x00000020,
    0x0074754f, 0x00050006, 0x00000020, 0x00000000, 0x61746164, 0x00000000, 0x00040005, 0x00000022,
    0x6274756f, 0x00000000, 0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00040047, 0x00000019,
    0x00000021, 0x00000000, 0x00040047, 0x00000019, 0x00000022, 0x00000000, 0x00040047, 0x0000001f,
    0x00000006, 0x00000004, 0x00030047, 0x00000020, 0x00000003, 0x00050048, 0x00000020, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000022, 0x00000021, 0x00000001, 0x00040047, 0x00000022,
    0x00000022, 0x00000000, 0x00040047, 0x00000052, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015,
    0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020,
    0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040017,
    0x0000000e, 0x0000000a, 0x00000002, 0x00030016, 0x00000012, 0x00000020, 0x00040017, 0x00000013,
    0x00000012, 0x00000004, 0x00040020, 0x00000014, 0x00000007, 0x00000013, 0x00090019, 0x00000016,
    0x00000012, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b,
    0x00000017, 0x00000016, 0x00040020, 0x00000018, 0x00000000, 0x00000017, 0x0004003b, 0x00000018,
    0x00000019, 0x00000000, 0x0004002b, 0x00000006, 0x0000001c, 0x00000000, 0x0003001d, 0x0000001f,
    0x0000000a, 0x0003001e, 0x00000020, 0x0000001f, 0x00040020, 0x00000021, 0x00000002, 0x00000020,
    0x0004003b, 0x00000021, 0x00000022, 0x00000002, 0x0004002b, 0x0000000a, 0x00000023, 0x00000001,
    0x00040020, 0x00000024, 0x00000007, 0x00000006, 0x0004002b, 0x00000006, 0x00000027, 0x00000200,
    0x0004002b, 0x0000000a, 0x00000029, 0x00000000, 0x00040020, 0x0000002d, 0x00000007, 0x00000012,
    0x0004002b, 0x00000012, 0x00000030, 0x437f0000, 0x0004002b, 0x00000012, 0x00000032, 0x3f000000,
    0x0004002b, 0x00000006, 0x0000003a, 0x00000008, 0x0004002b, 0x0000000a, 0x0000003d, 0x00000002,
    0x0004002b, 0x00000006, 0x00000043, 0x00000010, 0x0004002b, 0x0000000a, 0x00000046, 0x00000003,
    0x0004002b, 0x00000006, 0x0000004c, 0x00000018, 0x00040020, 0x0000004f, 0x00000002, 0x0000000a,
    0x0004002b, 0x0000000a, 0x00000051, 0x00000020, 0x0006002c, 0x0000000b, 0x00000052, 0x00000051,
    0x00000051, 0x00000023, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000014, 0x00000015,
    0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000010,
    0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x0004007c, 0x00000007, 0x00000011, 0x00000010,
    0x0003003e, 0x00000009, 0x00000011, 0x0004003d, 0x00000017, 0x0000001a, 0x00000019, 0x0004003d,
    0x00000007, 0x0000001b, 0x00000009, 0x00040064, 0x00000016, 0x0000001d, 0x0000001a, 0x0007005f,
    0x00000013, 0x0000001e, 0x0000001d, 0x0000001b, 0x00000002, 0x0000001c, 0x0003003e, 0x00000015,
    0x0000001e, 0x00050041, 0x00000024, 0x00000025, 0x00000009, 0x00000023, 0x0004003d, 0x00000006,
    0x00000026, 0x00000025, 0x00050084, 0x00000006, 0x00000028, 0x00000026, 0x00000027, 0x00050041,
    0x00000024, 0x0000002a, 0x00000009, 0x00000029, 0x0004003d, 0x00000006, 0x0000002b, 0x0000002a,
    0x00050080, 0x00000006, 0x0000002c, 0x00000028, 0x0000002b, 0x00050041, 0x0000002d, 0x0000002e,
    0x00000015, 0x00000029, 0x0004003d, 0x00000012, 0x0000002f, 0x0000002e, 0x00050085, 0x00000012,
    0x00000031, 0x0000002f, 0x00000030, 0x00050081, 0x00000012, 0x00000033, 0x00000031, 0x00000032,
    0x0004006d, 0x0000000a, 0x00000034, 0x00000033, 0x00050041, 0x0000002d, 0x00000035, 0x00000015,
    0x00000023, 0x0004003d, 0x00000012, 0x00000036, 0x00000035, 0x00050085, 0x00000012, 0x00000037,
    0x00000036, 0x00000030, 0x00050081, 0x00000012, 0x00000038, 0x00000037, 0x00000032, 0x0004006d,
    0x0000000a, 0x00000039, 0x00000038, 0x000500c4, 0x0000000a, 0x0000003b, 0x00000039, 0x0000003a,
    0x000500c5, 0x0000000a, 0x0000003c, 0x00000034, 0x0000003b, 0x00050041, 0x0000002d, 0x0000003e,
    0x00000015, 0x0000003d, 0x0004003d, 0x00000012, 0x0000003f, 0x0000003e, 0x00050085, 0x00000012,
    0x00000040, 0x0000003f, 0x00000030, 0x00050081, 0x00000012, 0x00000041, 0x00000040, 0x00000032,
    0x0004006d, 0x0000000a, 0x00000042, 0x00000041, 0x000500c4, 0x0000000a, 0x00000044, 0x00000042,
    0x00000043, 0x000500c5, 0x0000000a, 0x00000045, 0x0000003c, 0x00000044, 0x00050041, 0x0000002d,
    0x00000047, 0x00000015, 0x00000046, 0x0004003d, 0x00000012, 0x00000048, 0x00000047, 0x00050085,
    0x00000012, 0x00000049, 0x00000048, 0x00000030, 0x00050081, 0x00000012, 0x0000004a, 0x00000049,
    0x00000032, 0x0004006d, 0x0000000a, 0x0000004b, 0x0000004a, 0x000500c4, 0x0000000a, 0x0000004d,
    0x0000004b, 0x0000004c, 0x000500c5, 0x0000000a, 0x0000004e, 0x00000045, 0x0000004d, 0x00060041,
    0x0000004f, 0x00000050, 0x00000022, 0x0000001c, 0x0000002c, 0x0003003e, 0x00000050, 0x0000004e,
    0x000100fd, 0x00010038
};

static const uint32_t cast_r32_spv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000033, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00060010, 0x00000004,
    0x00000011, 0x00000020, 0x00000020, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004,
    0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365,
    0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572,
    0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00007675,
    0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044,
    0x00030005, 0x00000014, 0x00000063, 0x00030005, 0x00000018, 0x00676d69, 0x00030005, 0x0000001f,
    0x0074754f, 0x00050006, 0x0000001f, 0x00000000, 0x61746164, 0x00000000, 0x00040005, 0x00000021,
    0x6274756f, 0x00000000, 0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00040047, 0x00000018,
    0x00000021, 0x00000000, 0x00040047, 0x00000018, 0x00000022, 0x00000000, 0x00040047, 0x0000001e,
    0x00000006, 0x00000004, 0x00030047, 0x0000001f, 0x00000003, 0x00050048, 0x0000001f, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000021, 0x00000021, 0x00000001, 0x00040047, 0x00000021,
    0x00000022, 0x00000000, 0x00040047, 0x00000032, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015,
    0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020,
    0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040017,
    0x0000000e, 0x0000000a, 0x00000002, 0x00040017, 0x00000012, 0x0000000a, 0x00000004, 0x00040020,
    0x00000013, 0x00000007, 0x00000012, 0x00090019, 0x00000015, 0x0000000a, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x00000016, 0x00000015, 0x00040020,
    0x00000017, 0x00000000, 0x00000016, 0x0004003b, 0x00000017, 0x00000018, 0x00000000, 0x0004002b,
    0x00000006, 0x0000001b, 0x00000000, 0x0003001d, 0x0000001e, 0x0000000a, 0x0003001e, 0x0000001f,
    0x0000001e, 0x00040020, 0x00000020, 0x00000002, 0x0000001f, 0x0004003b, 0x00000020, 0x00000021,
    0x00000002, 0x0004002b, 0x0000000a, 0x00000022, 0x00000001, 0x00040020, 0x00000023, 0x00000007,
    0x00000006, 0x0004002b, 0x00000006, 0x00000026, 0x00000200, 0x0004002b, 0x0000000a, 0x00000028,
    0x00000000, 0x00040020, 0x0000002c, 0x00000007, 0x0000000a, 0x00040020, 0x0000002f, 0x00000002,
    0x0000000a, 0x0004002b, 0x0000000a, 0x00000031, 0x00000020, 0x0006002c, 0x0000000b, 0x00000032,
    0x00000031, 0x00000031, 0x00000022, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000013,
    0x00000014, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d, 0x0007004f, 0x0000000e,
    0x00000010, 0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x0004007c, 0x00000007, 0x00000011,
    0x00000010, 0x0003003e, 0x00000009, 0x00000011, 0x0004003d, 0x00000016, 0x00000019, 0x00000018,
    0x0004003d, 0x00000007, 0x0000001a, 0x00000009, 0x00040064, 0x00000015, 0x0000001c, 0x00000019,
    0x0007005f, 0x00000012, 0x0000001d, 0x0000001c, 0x0000001a, 0x00000002, 0x0000001b, 0x0003003e,
    0x00000014, 0x0000001d, 0x00050041, 0x00000023, 0x00000024, 0x00000009, 0x00000022, 0x0004003d,
    0x00000006, 0x00000025, 0x00000024, 0x00050084, 0x00000006, 0x00000027, 0x00000025, 0x00000026,
    0x00050041, 0x00000023, 0x00000029, 0x00000009, 0x00000028, 0x0004003d, 0x00000006, 0x0000002a,
    0x00000029, 0x00050080, 0x00000006, 0x0000002b, 0x00000027, 0x0000002a, 0x00050041, 0x0000002c,
    0x0000002d, 0x00000014, 0x00000028, 0x0004003d, 0x0000000a, 0x0000002e, 0x0000002d, 0x00060041,
    0x0000002f, 0x00000030, 0x00000021, 0x0000001b, 0x0000002b, 0x0003003e, 0x00000030, 0x0000002e,
    0x000100fd, 0x00010038
};

#define W 512
#define H 512
#define TILE 0x10000

static void chk(VkResult r, const char *what) {
    if (r != VK_SUCCESS) { printf("[ERROR] %s -> %d\n", what, r); exit(1); }
}

static const char *vkerr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    default: return "OTHER";
    }
}

static VkResult alloc_mem(VkDevice dev, VkDeviceSize size, uint32_t type, VkDeviceMemory *out) {
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, type };
    return vkAllocateMemory(dev, &mai, NULL, out);
}

/* Compute pipeline from embedded SPIR-V (entry "main", local size 32x32). */
static VkPipeline make_compute_pipe(VkDevice dev, VkPipelineLayout pl, const uint32_t *code, uint32_t code_size) {
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, code_size, code };
    VkShaderModule mod;
    if (vkCreateShaderModule(dev, &smci, NULL, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo ss = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                           VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
    VkComputePipelineCreateInfo cpc = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, ss, pl, NULL, 0 };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpc, NULL, &pipe);
    vkDestroyShaderModule(dev, mod, NULL);
    return r == VK_SUCCESS ? pipe : VK_NULL_HANDLE;
}

/* One fill+read round trip: copy the [0,gran) region of `fill` (W*H words,
 * stored as little-endian bytes) into img, dispatch `pipe`/`ds` over the same
 * region (the descriptor's image view determines the format the words are read
 * back in), wait, and copy the readback words out. With fill==NULL the fill
 * step is skipped (pure read; img must already hold the expected content) and
 * a layout barrier to SHADER_READ_ONLY_OPTIMAL is issued. Leaves img in
 * SHADER_READ_ONLY_OPTIMAL. Returns 0 on success. */
static int cast_roundtrip(VkDevice dev, VkQueue q, VkCommandPool pool,
                          VkImage img, VkImageLayout from_layout, VkAccessFlags from_access,
                          VkExtent3D gran, const uint32_t *fill,
                          VkPipeline pipe, VkDescriptorSet ds,
                          VkDeviceMemory rbm, uint32_t *rb_out)
{
    VkBuffer fb = VK_NULL_HANDLE;
    VkDeviceMemory fbm = VK_NULL_HANDLE;
    if (fill) {
        VkBufferCreateInfo fci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)W * H * 4,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if (vkCreateBuffer(dev, &fci, NULL, &fb) != VK_SUCCESS)
            return -1;
        VkMemoryRequirements fr;
        vkGetBufferMemoryRequirements(dev, fb, &fr);
        if (alloc_mem(dev, fr.size, 0, &fbm) != VK_SUCCESS)
            return -1;
        if (vkBindBufferMemory(dev, fb, fbm, 0) != VK_SUCCESS)
            return -1;
        uint32_t *fw;
        if (vkMapMemory(dev, fbm, 0, fr.size, 0, (void **)&fw) != VK_SUCCESS)
            return -1;
        memcpy(fw, fill, (size_t)W * H * 4);
        vkUnmapMemory(dev, fbm);
    }

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cb, &bbi);

    VkImageSubresourceRange full = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (fill) {
        VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, from_access,
                                     VK_ACCESS_TRANSFER_WRITE_BIT, from_layout,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, img, full };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
        /* fill buffer is W-wide rows; copy only the [0,gran) corner */
        VkBufferImageCopy bic = { 0, W, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                  { (uint32_t)gran.width, (uint32_t)gran.height, 1 } };
        vkCmdCopyBufferToImage(cb, fb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        imb = (VkImageMemoryBarrier){ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
                                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, img, full };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &imb);
    } else {
        VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, from_access,
                                     VK_ACCESS_SHADER_READ_BIT, from_layout,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, img, full };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, VK_NULL_HANDLE, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, (gran.width + 31) / 32, (gran.height + 31) / 32, 1);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    vkQueueSubmit(q, 1, &si, NULL);
    vkQueueWaitIdle(q);
    vkFreeCommandBuffers(dev, pool, 1, &cb);

    void *rwm;
    if (vkMapMemory(dev, rbm, 0, (VkDeviceSize)W * H * 4, 0, &rwm) != VK_SUCCESS)
        return -1;
    memcpy(rb_out, rwm, (size_t)W * H * 4);
    vkUnmapMemory(dev, rbm);
    if (fb) {
        vkDestroyBuffer(dev, fb, NULL);
        vkFreeMemory(dev, fbm, NULL);
    }
    return 0;
}

/* Compare the [0,w)x[0,h) word region of `got` against `want`; print up to 8
 * mismatches. Returns the mismatch count. */
static int compare_words(const uint32_t *got, const uint32_t *want, uint32_t w, uint32_t h, const char *tag)
{
    int bad = 0;
    for (uint32_t y = 0; y < h && bad < 8; y++)
        for (uint32_t x = 0; x < w; x++)
            if (got[y * W + x] != want[y * W + x]) {
                printf("  %s y%u x%u: read 0x%08x want 0x%08x\n", tag, y, x,
                       got[y * W + x], want[y * W + x]);
                bad++;
                break;
            }
    return bad;
}

/* Behavior probe for an image that may have been force-lined by the
 * mutable-format logic (E/F): bind one granule, read the whole image back
 * via transfer, classify the pixels. */
static void probe_linear_sparse(VkDevice dev, VkQueue q, VkCommandPool pool, VkImage img, const char *tag)
{
    uint32_t sc = 0;
    vkGetImageSparseMemoryRequirements(dev, img, &sc, NULL);
    printf("  %s: sparse req count=%u\n", tag, sc);
    if (!sc)
        return;
    VkSparseImageMemoryRequirements smreq[16];
    if (sc > 16)
        sc = 16;
    vkGetImageSparseMemoryRequirements(dev, img, &sc, smreq);
    VkExtent3D g = smreq[0].formatProperties.imageGranularity;
    printf("  %s: gran=%ux%ux%u\n", tag, g.width, g.height, g.depth);
    if (!g.width || !g.height)
        return;

    VkDeviceSize gran_bytes = (VkDeviceSize)g.width * g.height * 4;
    VkDeviceSize img_bytes = (VkDeviceSize)W * H * 4;
    VkDeviceSize memsize = gran_bytes < img_bytes ? gran_bytes : img_bytes;
    uint32_t bw = g.width < W ? g.width : W;
    uint32_t bh = g.height < H ? g.height : H;

    VkDeviceMemory m;
    if (alloc_mem(dev, memsize, 0, &m) != VK_SUCCESS) {
        printf("  %s: memory alloc failed\n", tag);
        return;
    }
    unsigned char *pm;
    chk(vkMapMemory(dev, m, 0, memsize, 0, (void **)&pm), "map E/F tile");
    memset(pm, 0x37, (size_t)memsize);
    vkUnmapMemory(dev, m);

    VkSparseImageMemoryBind b = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 },
                                  (VkExtent3D){ bw, bh, 1 }, m, 0, 0 };
    VkSparseImageMemoryBindInfo bi = { img, 1, &b };
    VkBindSparseInfo bsi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL,
                             1, &bi, 0, NULL };
    chk(vkQueueBindSparse(q, 1, &bsi, NULL), "vkQueueBindSparse(E/F)");
    vkQueueWaitIdle(q);

    VkBufferCreateInfo stci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, img_bytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer st;
    chk(vkCreateBuffer(dev, &stci, NULL, &st), "create staging(E/F)");
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, st, &r);
    VkDeviceMemory sm;
    chk(alloc_mem(dev, r.size, 0, &sm), "alloc staging(E/F)");
    chk(vkBindBufferMemory(dev, st, sm, 0), "bind staging(E/F)");

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cb, &bbi);
    VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                 VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, img,
                                 (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &imb);
    VkBufferImageCopy ic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st, 1, &ic);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    vkQueueSubmit(q, 1, &si, NULL);
    vkQueueWaitIdle(q);
    vkFreeCommandBuffers(dev, pool, 1, &cb);

    unsigned char *sp;
    chk(vkMapMemory(dev, sm, 0, r.size, 0, (void **)&sp), "map readback(E/F)");
    unsigned n37 = 0, nz = 0, noth = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const unsigned char *px = sp + (size_t)(y * W + x) * 4;
            int all_zero = 1, all_37 = 1;
            for (int i = 0; i < 4; i++) {
                if (px[i])
                    all_zero = 0;
                if (px[i] != 0x37)
                    all_37 = 0;
            }
            if (all_37)
                n37++;
            else if (all_zero)
                nz++;
            else
                noth++;
        }
    vkUnmapMemory(dev, sm);
    if (n37 == (unsigned)W * H)
        printf("  %s readback: fully 0x37 - linear+sparse functional end-to-end\n", tag);
    else if (!noth)
        printf("  %s readback: 0x37 x%u, zero x%u - bind partially effective\n", tag, n37, nz);
    else
        printf("  %s readback: 0x37 x%u, zero x%u, GARBAGE x%u - linear+sparse broken, gate justified\n",
               tag, n37, nz, noth);
}

int main(void) {
    VkInstance inst;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "sparse_test", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    chk(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    VkPhysicalDevice *devs = malloc(nd * sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &nd, devs);
    VkPhysicalDevice pd = NULL;
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[i], &pp);
        if (pp.vendorID == 0x5143) {
            pd = devs[i];
            printf("device: %s (chip_id 0x%x)\n", pp.deviceName, pp.deviceID);
            printf("sparseProperties: std2DBlock=%u std2DMultisample=%u std3DBlock=%u "
                   "alignedMip=%u nonResidentStrict=%u\n",
                   pp.sparseProperties.residencyStandard2DBlockShape,
                   pp.sparseProperties.residencyStandard2DMultisampleBlockShape,
                   pp.sparseProperties.residencyStandard3DBlockShape,
                   pp.sparseProperties.residencyAlignedMipSize,
                   pp.sparseProperties.residencyNonResidentStrict);
            VkPhysicalDeviceFeatures pf;
            vkGetPhysicalDeviceFeatures(devs[i], &pf);
            printf("features: sparseBinding=%u sparseResidencyBuffer=%u sparseResidencyImage2D=%u "
                   "sparseResidencyAliased=%u shaderResourceResidency=%u shaderResourceMinLod=%u\n",
                   pf.sparseBinding, pf.sparseResidencyBuffer, pf.sparseResidencyImage2D,
                   pf.sparseResidencyAliased, pf.shaderResourceResidency, pf.shaderResourceMinLod);
        }
    }
    chk(pd ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED, "find turnip");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &dq, 0, NULL, 0, NULL, NULL };
    VkDevice dev;
    chk(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* sparse residency image: bind 1 tile, fill pattern, copy back, verify */
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){ W, H, 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    chk(vkCreateImage(dev, &ii, NULL, &img), "vkCreateImage(sparse)");

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    printf("image memreq: size=0x%llx align=0x%llx typeBits=0x%x\n",
           (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);

    /* memory type 0 = DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (unified) */
    VkPhysicalDeviceMemoryProperties memprops;
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);

    uint32_t sc = 0;
    vkGetImageSparseMemoryRequirements(dev, img, &sc, NULL);
    VkSparseImageMemoryRequirements sm[8];
    vkGetImageSparseMemoryRequirements(dev, img, &sc, sm);
    for (uint32_t i = 0; i < sc; i++)
        printf("sparse req[%u]: gran=%ux%ux%u flags=0x%x mipTailLod=%u size=0x%llx off=0x%llx\n",
               i, sm[i].formatProperties.imageGranularity.width,
               sm[i].formatProperties.imageGranularity.height,
               sm[i].formatProperties.imageGranularity.depth,
               sm[i].formatProperties.flags, sm[i].imageMipTailFirstLod,
               (unsigned long long)sm[i].imageMipTailSize,
               (unsigned long long)sm[i].imageMipTailOffset);
    VkExtent3D gran = sm[0].formatProperties.imageGranularity;

    VkDeviceMemory mem;
    chk(alloc_mem(dev, TILE, 0, &mem), "alloc tile");
    unsigned char *p = NULL;
    chk(vkMapMemory(dev, mem, 0, TILE, 0, (void **)&p), "map");
    memset(p, 0x37, TILE);
    vkUnmapMemory(dev, mem);
    printf("filled bound granule (%u x %u px) with 0x37\n", gran.width, gran.height);

    VkSparseImageMemoryBind bind = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 }, gran, mem, 0, 0 };
    VkSparseImageMemoryBindInfo bind_info = { img, 1, &bind };
    VkBindSparseInfo bsi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL, 1, &bind_info, 0, NULL };
    chk(vkQueueBindSparse(q, 1, &bsi, NULL), "vkQueueBindSparse");
    vkQueueWaitIdle(q);
    printf("vkQueueBindSparse: SUCCESS\n");

    /* staging buffer */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer st;
    chk(vkCreateBuffer(dev, &bci, NULL, &st), "create staging");
    VkMemoryRequirements smr;
    vkGetBufferMemoryRequirements(dev, st, &smr);
    VkDeviceMemory stm;
    chk(alloc_mem(dev, smr.size, 0, &stm), "alloc staging");
    chk(vkBindBufferMemory(dev, st, stm, 0), "bind staging");
    unsigned char *sp = NULL;
    chk(vkMapMemory(dev, stm, 0, smr.size, 0, (void **)&sp), "map staging");
    memset(sp, 0xAA, (size_t)smr.size);
    vkUnmapMemory(dev, stm);

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    VkCommandPool pool;
    chk(vkCreateCommandPool(dev, &cpci, NULL, &pool), "create pool");
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    chk(vkAllocateCommandBuffers(dev, &cai, &cb), "alloc cb");
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    chk(vkBeginCommandBuffer(cb, &bbi), "begin cb");
    VkImageMemoryBarrier imr = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                 VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, img,
                                 (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &imr);
    VkBufferImageCopy ic = { 0, 0, 0,
                       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st, 1, &ic);
    chk(vkEndCommandBuffer(cb), "end cb");
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    chk(vkQueueSubmit(q, 1, &si, NULL), "submit");
    chk(vkQueueWaitIdle(q), "wait");

    chk(vkMapMemory(dev, stm, 0, smr.size, 0, (void **)&sp), "map readback");
    const unsigned char *data = sp;
    int errs = 0;
    for (int y = 0; y < H && errs < 12; y++) {
        const unsigned char *row = data + (size_t)y * W * 4;
        if (y < (int)gran.height) {
            for (int x = 0; x < (int)gran.width; x++)
                if (row[x * 4] != 0x37) { printf("  row %d: bound pixel %d wrong (0x%02x)\n", y, x, row[x * 4]); errs++; break; }
            if (errs < 12)
                for (int x = (int)gran.width; x < W; x++)
                    if (row[x * 4] != 0x00) { printf("  row %d: unbound-after-tile %d = 0x%02x\n", y, x, row[x * 4]); errs++; break; }
        } else {
            for (int x = 0; x < W; x++)
                if (row[x * 4] != 0x00) { printf("  row %d: unbound x%d = 0x%02x\n", y, x, row[x * 4]); errs++; break; }
        }
    }
    vkUnmapMemory(dev, stm);
    if (!errs)
        printf("RESULT A: PASS - bound tile readback ok, unbound reads 0 (PRR)\n");
    else
        printf("RESULT A: FAIL (%d problems)\n", errs);

    /* B: format-cast via alias - bind the same 64K memory into an R32_UINT
     * sparse image and read it back. If A740 mis-handles format casts on
     * UBWC/sparse blocks we'd see garbage; expect exact bit reinterpretation. */
    printf("\n=== B. alias/cast: same memory as R32_UINT ===");
    VkImage img2 = VK_NULL_HANDLE;
    VkImageCreateInfo ii2 = ii;
    ii2.format = VK_FORMAT_R32_UINT;
    ii2.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT
                | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkResult rc2 = vkCreateImage(dev, &ii2, NULL, &img2);
    if (rc2 != VK_SUCCESS) {
        /* B's class is MUTABLE with no format list - on a P1-patched
         * driver this is exactly the linear+sparse hole that got closed,
         * so the create is (correctly) rejected; stock 26.2.2 created a
         * silently linear+sparse image here (NDEBUG). */
        printf("B: create no-list mutable sparse R32_UINT: %s - SKIPPED\n", vkerr(rc2));
        goto skip_b;
    }

    uint32_t sc2 = 0;
    vkGetImageSparseMemoryRequirements(dev, img2, &sc2, NULL);
    VkSparseImageMemoryRequirements sm2r2[8];
    vkGetImageSparseMemoryRequirements(dev, img2, &sc2, sm2r2);
    for (uint32_t i = 0; i < sc2; i++)
        printf("sparse req[%u] (R32_UINT): gran=%ux%ux%u flags=0x%x\n", i,
               sm2r2[i].formatProperties.imageGranularity.width,
               sm2r2[i].formatProperties.imageGranularity.height,
               sm2r2[i].formatProperties.imageGranularity.depth,
               sm2r2[i].formatProperties.flags);
    VkExtent3D gran2 = sm2r2[0].formatProperties.imageGranularity;
    VkDeviceSize bytes2 = (VkDeviceSize)gran2.width * gran2.height * 4;
    printf("R32_UINT granule %ux%u = 0x%llx bytes\n", gran2.width, gran2.height,
           (unsigned long long)bytes2);
    if (bytes2 > TILE) { printf("granule exceeds 64K tile, aborting B\n"); goto skip_b; }
    if (bytes2 < TILE)
        printf("B granule < 64K: binding full 64K memory (same content, zeros above)\n");

    VkSparseImageMemoryBind bind2 = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 },
        gran2, mem, 0, 0 };   /* mem still holds the 0x37373737 tile */
    VkSparseImageMemoryBindInfo bind_info2 = { img2, 1, &bind2 };
    VkBindSparseInfo bsi2 = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL,
                              1, &bind_info2, 0, NULL };
    chk(vkQueueBindSparse(q, 1, &bsi2, NULL), "vkQueueBindSparse(R32_UINT)");
    vkQueueWaitIdle(q);

    VkBuffer st2;
    chk(vkCreateBuffer(dev, &bci, NULL, &st2), "create staging2");
    VkMemoryRequirements smr2;
    vkGetBufferMemoryRequirements(dev, st2, &smr2);
    VkDeviceMemory stm2;
    chk(alloc_mem(dev, smr2.size, 0, &stm2), "alloc staging2");
    chk(vkBindBufferMemory(dev, st2, stm2, 0), "bind staging2");
    unsigned char *sp2 = NULL;
    chk(vkMapMemory(dev, stm2, 0, smr2.size, 0, (void **)&sp2), "map staging2");
    memset(sp2, 0x55, (size_t)smr2.size);
    vkUnmapMemory(dev, stm2);

    VkCommandBuffer cb2;
    chk(vkAllocateCommandBuffers(dev, &cai, &cb2), "alloc cb2");
    VkCommandBufferBeginInfo bbi2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    chk(vkBeginCommandBuffer(cb2, &bbi2), "begin cb2");
    VkImageMemoryBarrier imr2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                  VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, img2,
                                  (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &imr2);
    VkBufferImageCopy ic2 = { 0, 0, 0,
                        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb2, img2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st2, 1, &ic2);
    chk(vkEndCommandBuffer(cb2), "end cb2");
    VkSubmitInfo si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb2, 0, NULL };
    chk(vkQueueSubmit(q, 1, &si2, NULL), "submit2");
    chk(vkQueueWaitIdle(q), "wait2");

    chk(vkMapMemory(dev, stm2, 0, smr2.size, 0, (void **)&sp2), "map readback2");
    errs = 0;
    for (int y = 0; y < (int)gran2.height && !errs; y++) {
        const uint32_t *row = (const uint32_t *)(sp2 + (size_t)y * W * 4);
        for (int x = 0; x < (int)gran2.width; x++) {
            if (row[x] != 0x37373737u) { printf("  row %d px %d = 0x%08x\n", y, x, row[x]); errs++; break; }
        }
    }
    for (int y = 0; y < (int)gran2.height && !errs; y++) {
        const uint32_t *row = (const uint32_t *)(sp2 + (size_t)y * W * 4);
        for (int x = (int)gran2.width; x < W; x++) {
            if (row[x] != 0) { printf("  row %d x%d (unbound) = 0x%08x\n", y, x, row[x]); errs++; break; }
        }
    }
    vkUnmapMemory(dev, stm2);
    if (!errs)
        printf("\nRESULT B: PASS - R32_UINT alias of the same 64K tile reads back exactly 0x37373737, rest 0 (cast-clean)\n");
    else
        printf("\nRESULT B: INCONCLUSIVE - same memory bound into two differently-formatted images, "
               "%d mismatch; storage tiling differs per format. True D3D tiled-resources format-cast "
               "is a view-cast on ONE image and needs a pipeline test.\n", errs);

skip_b:
    vkQueueWaitIdle(q);

    /* C. fallback probe: what happens when an app asks for the exact case the
     * ubwc_all_formats_compatible gate exists to protect on gen2 - a MUTABLE
     * sparse image whose format list permits cast (RGBA8 <-> R32_UINT)? */
    printf("\n=== C. MUTABLE sparse probe (RGBA8 + R32_UINT fmt list) ===");
    VkPhysicalDeviceImageFormatInfo2 fif = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
    fif.type = VK_IMAGE_TYPE_2D;
    fif.format = VK_FORMAT_R8G8B8A8_UNORM;
    fif.tiling = VK_IMAGE_TILING_OPTIMAL;
    fif.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    fif.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    VkImageFormatProperties2 ifp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    VkResult rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &fif, &ifp);
    printf("getImageFormatProperties2(SPARSE_RESIDENCY,RGBA8): %s\n", vkerr(rq));

    VkFormat view_fmts[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT };
    VkImageFormatListCreateInfo ifl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, view_fmts };
    VkImageCreateInfo ii3 = ii;
    ii3.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii3.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT
                | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ii3.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   /* D fills it via CopyBufferToImage */
    ii3.pNext = &ifl;
    VkImage img3;
    VkExtent3D gran3 = { 1, 1, 1 };
    VkResult rc3 = vkCreateImage(dev, &ii3, NULL, &img3);
    printf("vkCreateImage(MUTABLE+SPARSE_RESIDENCY,{RGBA8,R32_UINT}): %s\n", vkerr(rc3));
    if (rc3 == VK_SUCCESS) {
        VkMemoryRequirements mr3;
        vkGetImageMemoryRequirements(dev, img3, &mr3);
        printf("  memreq: size=0x%llx align=0x%llx\n", (unsigned long long)mr3.size,
               (unsigned long long)mr3.alignment);
        VkSparseImageMemoryRequirements sm3[8];
        uint32_t sc3 = 0;
        vkGetImageSparseMemoryRequirements(dev, img3, &sc3, NULL);
        vkGetImageSparseMemoryRequirements(dev, img3, &sc3, sm3);
        for (uint32_t i = 0; i < sc3; i++)
            printf("  sparse[%u]: gran=%ux%ux%u mipTailLod=%u off=0x%llx\n", i,
                   sm3[i].formatProperties.imageGranularity.width,
                   sm3[i].formatProperties.imageGranularity.height,
                   sm3[i].formatProperties.imageGranularity.depth,
                   sm3[i].imageMipTailFirstLod, (unsigned long long)sm3[i].imageMipTailOffset);
        gran3 = sm3[0].formatProperties.imageGranularity;
        VkSparseImageMemoryBind b3 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 },
                                       gran3, mem, 0, 0 };   /* mem: 0x37 tile */
        VkSparseImageMemoryBindInfo bi3 = { img3, 1, &b3 };
        VkBindSparseInfo bsi3 = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL,
                                  1, &bi3, 0, NULL };
        chk(vkQueueBindSparse(q, 1, &bsi3, NULL), "vkQueueBindSparse(C)");
        vkQueueWaitIdle(q);

        VkBuffer st3;
        chk(vkCreateBuffer(dev, &bci, NULL, &st3), "create staging C");
        VkMemoryRequirements smr3;
        vkGetBufferMemoryRequirements(dev, st3, &smr3);
        VkDeviceMemory stm3;
        chk(alloc_mem(dev, smr3.size, 0, &stm3), "alloc staging C");
        chk(vkBindBufferMemory(dev, st3, stm3, 0), "bind staging C");
        unsigned char *sp3 = NULL;
        chk(vkMapMemory(dev, stm3, 0, smr3.size, 0, (void **)&sp3), "map staging C");
        memset(sp3, 0xAA, (size_t)smr3.size);
        vkUnmapMemory(dev, stm3);

        VkCommandBuffer cb3;
        chk(vkAllocateCommandBuffers(dev, &cai, &cb3), "alloc cb3");
        VkCommandBufferBeginInfo bbi3 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        chk(vkBeginCommandBuffer(cb3, &bbi3), "begin cb3");
        VkImageMemoryBarrier imr3 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                      VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, img3,
                                      (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb3, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &imr3);
        VkBufferImageCopy ic3 = { 0, 0, 0,
                            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyImageToBuffer(cb3, img3, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st3, 1, &ic3);
        chk(vkEndCommandBuffer(cb3), "end cb3");
        VkSubmitInfo si3 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb3, 0, NULL };
        chk(vkQueueSubmit(q, 1, &si3, NULL), "submit3");
        vkQueueWaitIdle(q);

        chk(vkMapMemory(dev, stm3, 0, smr3.size, 0, (void **)&sp3), "map readback C");
        errs = 0;
        for (int y = 0; y < (int)gran3.height && !errs; y++) {
            const unsigned char *row = sp3 + (size_t)y * W * 4;
            for (int x = 0; x < (int)gran3.width; x++)
                if (row[x * 4] != 0x37) { printf("  row %d px %d = 0x%02x\n", y, x, row[x * 4]); errs++; break; }
        }
        if (!errs)
            for (int y = 0; y < (int)gran3.height && !errs; y++) {
                const unsigned char *row = sp3 + (size_t)y * W * 4;
                for (int x = (int)gran3.width; x < W; x++)
                    if (row[x * 4] != 0x00) { printf("  row %d x%d = 0x%02x\n", y, x, row[x * 4]); errs++; break; }
            }
        if (!errs)
            for (int y = (int)gran3.height; y < H && !errs; y++) {
                const unsigned char *row = sp3 + (size_t)y * W * 4;
                if (row[0] != 0x00) { printf("  unbound row %d = 0x%02x\n", y, row[0]); errs++; break; }
            }
        vkUnmapMemory(dev, stm3);
        if (!errs)
            printf("C probe: PASS - MUTABLE sparse create OK + base-format RGBA8 readback clean, unbound 0\n");
        else
            printf("C probe: FAIL/INCONCLUSIVE (%d problems)\n", errs);
    }

    /* D. view-cast probe - decisive for the ubwc_all_formats_compatible gate:
     * on the C image (base RGBA8, list {RGBA8, R32_UINT}) create an RGBA8 and
     * an R32_UINT view, fill through one format, read back through the other
     * with a compute shader. If the tiled non-UBWC layout is format-identical
     * the cast must be bit-exact in both directions. */
    int d2_bad = 0;   /* D's d2 mismatch count, exported for the trigger matrix */
    printf("\n=== D. view-cast probe (RGBA8 <-> R32_UINT, one image) ===");
    if (rc3 == VK_SUCCESS) {
        VkDescriptorSetLayoutBinding dsb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        };
        VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  NULL, 0, 2, dsb };
        VkDescriptorSetLayout dsl;
        chk(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl), "create dsl");
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
        VkPipelineLayout pl;
        chk(vkCreatePipelineLayout(dev, &plci, NULL, &pl), "create pipeline layout");

        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .anisotropyEnable = VK_FALSE,
            .compareEnable = VK_FALSE,
            .unnormalizedCoordinates = VK_FALSE,
        };
        VkSampler sam;
        chk(vkCreateSampler(dev, &sci, NULL, &sam), "create sampler");

        VkImageViewCreateInfo ivc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img3,
                                      VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                      (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY,
                                                           VK_COMPONENT_SWIZZLE_IDENTITY,
                                                           VK_COMPONENT_SWIZZLE_IDENTITY,
                                                           VK_COMPONENT_SWIZZLE_IDENTITY },
                                      (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView view_rgba;
        chk(vkCreateImageView(dev, &ivc, NULL, &view_rgba), "create view RGBA8");
        ivc.format = VK_FORMAT_R32_UINT;
        VkImageView view_r32;
        chk(vkCreateImageView(dev, &ivc, NULL, &view_r32), "create view R32_UINT");

        VkPipeline pipe_rgba = make_compute_pipe(dev, pl, cast_rgba_spv, (uint32_t)sizeof(cast_rgba_spv));
        VkPipeline pipe_r32 = make_compute_pipe(dev, pl, cast_r32_spv, (uint32_t)sizeof(cast_r32_spv));
        chk(pipe_rgba != VK_NULL_HANDLE && pipe_r32 != VK_NULL_HANDLE ?
                VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED, "create cast pipelines");

        VkBufferCreateInfo rbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)W * H * 4,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer rb;
        chk(vkCreateBuffer(dev, &rbci, NULL, &rb), "create readback");
        VkMemoryRequirements rbr;
        vkGetBufferMemoryRequirements(dev, rb, &rbr);
        VkDeviceMemory rbm;
        chk(alloc_mem(dev, rbr.size, 0, &rbm), "alloc readback");
        chk(vkBindBufferMemory(dev, rb, rbm, 0), "bind readback");

        VkDescriptorPoolSize dpsz[2] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
        };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 2, dpsz };
        VkDescriptorPool dp;
        chk(vkCreateDescriptorPool(dev, &dpci, NULL, &dp), "create dpool");
        VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 2,
                                             (VkDescriptorSetLayout[]){ dsl, dsl } };
        VkDescriptorSet dsets[2];
        chk(vkAllocateDescriptorSets(dev, &dsai, dsets), "alloc dsets");
        VkDescriptorImageInfo dii0 = { sam, view_rgba, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo dii1 = { sam, view_r32, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo bfi = { rb, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr[4] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[0], .dstBinding = 0,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii0 },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[0], .dstBinding = 1,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bfi },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[1], .dstBinding = 0,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii1 },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[1], .dstBinding = 1,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bfi },
        };
        vkUpdateDescriptorSets(dev, 4, wr, 0, NULL);

        uint32_t *fill1 = malloc((size_t)W * H * 4);
        uint32_t *fill2 = malloc((size_t)W * H * 4);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                /* bytes (b0..b3) = (0x5A, x^y, x, y) / (0xA5, x+y, y, x) */
                fill1[y * W + x] = ((uint32_t)y << 24) | ((uint32_t)x << 16)
                                  | ((((uint32_t)(x ^ y)) << 8) & 0xFF00u) | 0x5A;
                fill2[y * W + x] = ((uint32_t)x << 24) | ((uint32_t)y << 16)
                                  | ((((uint32_t)(x + y)) << 8) & 0xFF00u) | 0xA5;
            }
        uint32_t *rbw = malloc((size_t)W * H * 4);

        /* D0: baseline - decode the untouched 0x37 tile through the 8888 view
         * before any fill (img3 is in TRANSFER_SRC_OPTIMAL after C). */
        chk(cast_roundtrip(dev, q, pool, img3, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_TRANSFER_READ_BIT, gran3, NULL, pipe_rgba, dsets[0],
                           rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
            "D0 read");
        {
            int bad0 = 0;
            for (int y = 0; y < (int)gran3.height && bad0 < 8; y++)
                for (int x = 0; x < (int)gran3.width; x++)
                    if (rbw[y * W + x] != 0x37373737u) {
                        printf("  d0 y%d x%d: read 0x%08x (want 0x37373737)\n", y, x,
                               rbw[y * W + x]);
                        bad0++;
                        break;
                    }
            if (bad0)
                printf("  d0: 8888 TP decode TRANSFORMS constant 0x37 data on this image\n");
            else
                printf("  d0: constant 0x37 reads exact through 8888 view (decode is data-dependent)\n");
        }

        /* direction 1: written as RGBA8, read back through the R32_UINT view */
        chk(cast_roundtrip(dev, q, pool, img3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT, gran3, fill1, pipe_r32, dsets[1],
                           rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
            "cast roundtrip 1");
        int bad1 = 0;
        for (int y = 0; y < (int)gran3.height && bad1 < 8; y++)
            for (int x = 0; x < (int)gran3.width; x++)
                if (rbw[y * W + x] != fill1[y * W + x]) {
                    printf("  d1 y%d x%d: read 0x%08x want 0x%08x\n", y, x,
                           rbw[y * W + x], fill1[y * W + x]);
                    bad1++;
                    break;
                }

        /* direction 2: written as R32_UINT, read back through the RGBA8 view */
        chk(cast_roundtrip(dev, q, pool, img3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT, gran3, fill2, pipe_rgba, dsets[0],
                           rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
            "cast roundtrip 2");
        int bad2 = 0;
        for (int y = 0; y < (int)gran3.height && bad2 < 8; y++)
            for (int x = 0; x < (int)gran3.width; x++)
                if (rbw[y * W + x] != fill2[y * W + x]) {
                    printf("  d2 y%d x%d: read 0x%08x want 0x%08x\n", y, x,
                           rbw[y * W + x], fill2[y * W + x]);
                    bad2++;
                    break;
                }

        d2_bad = bad2;

        /* D3: ground truth after d2 - R32 view, no rewrite: if d2's transform
         * is read-side, memory must still hold fill2's raw words. */
        chk(cast_roundtrip(dev, q, pool, img3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT, gran3, NULL, pipe_r32, dsets[1],
                           rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
            "D3 read");
        int bad3 = 0;
        for (int y = 0; y < (int)gran3.height && bad3 < 8; y++)
            for (int x = 0; x < (int)gran3.width; x++)
                if (rbw[y * W + x] != fill2[y * W + x]) {
                    printf("  d3 y%d x%d: read 0x%08x want 0x%08x\n", y, x,
                           rbw[y * W + x], fill2[y * W + x]);
                    bad3++;
                    break;
                }
        if (bad3)
            printf("  d3: FAIL - R32 view after d2 does NOT match fill2 (memory changed, R32 decode off)\n");
        else
            printf("  d3: PASS - R32 view exact after d2; d2's transform lives in the 8888 read path only\n");

        if (!bad1 && !bad2)
            printf("D probe: PASS - view-cast RGBA8<->R32_UINT bit-exact in both directions; "
                   "tiled non-UBWC layout is format-identical, cast hazard not reproduced\n");
        else
            printf("D probe: FAIL (int) - d1/d3 (R32-INT) exact, d2 (8888-INT) transformed\n"
                   "                   -> int-8888 decode, not cast/layout corruption\n");
        free(fill1);
        free(fill2);
        free(rbw);
    } else {
        printf("D probe: SKIPPED (C image not created)\n");
    }

    /* E. swap-format list: on gen2 {RGBA8, BGRA8} trips format_list_has_swaps()
     * -> force_linear_tile (tu_image.cc), i.e. the linear+sparse path guarded
     * by the assert next to it. Stock release builds have no assert: probe
     * what the resulting image actually does. */
    printf("\n=== E. MUTABLE sparse, swap list {RGBA8, BGRA8} ===");
    VkFormat swap_fmts[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    VkImageFormatListCreateInfo ifl_s = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, swap_fmts };
    VkImageCreateInfo ii4 = ii3;
    ii4.pNext = &ifl_s;
    VkImage img4 = VK_NULL_HANDLE;
    VkResult rc4 = vkCreateImage(dev, &ii4, NULL, &img4);
    printf("vkCreateImage(MUTABLE+SPARSE_RESIDENCY,{RGBA8,BGRA8}): %s\n", vkerr(rc4));
    if (rc4 == VK_SUCCESS)
        probe_linear_sparse(dev, q, pool, img4, "E");

    /* F. no format list - the exact vkd3d-proton pattern named in MR !32671;
     * a NULL list makes format_list_has_swaps() return true, same force_linear
     * path as E. */
    printf("\n=== F. MUTABLE sparse, NO format list (vkd3d pattern) ===");
    VkImageCreateInfo ii5 = ii3;
    ii5.pNext = NULL;
    VkImage img5 = VK_NULL_HANDLE;
    VkResult rc5 = vkCreateImage(dev, &ii5, NULL, &img5);
    printf("vkCreateImage(MUTABLE+SPARSE_RESIDENCY, no list): %s\n", vkerr(rc5));
    if (rc5 == VK_SUCCESS)
        probe_linear_sparse(dev, q, pool, img5, "F");

    /* G0. Format-query side of the E/F hole (P1): the query must refuse
     * SPARSE_RESIDENCY for the same mutable lists that force linear tiling.
     * Stock 26.2.2: both SUCCESS (the hole). P1-patched: both
     * FORMAT_NOT_SUPPORTED. The non-mutable control must stay supported. */
    {
        VkPhysicalDeviceImageFormatInfo2 ifi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &ifl_s,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        };
        VkImageFormatProperties2 ifp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, NULL };
        VkResult rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
        printf("G0 query(SPARSE_RESIDENCY+MUTABLE {RGBA8,BGRA8}): %s\n", vkerr(rq));
        ifi.pNext = NULL;
        rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
        printf("G0 query(SPARSE_RESIDENCY+MUTABLE, no list):      %s\n", vkerr(rq));
        ifi.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
        printf("G0 query(SPARSE_RESIDENCY, non-mutable, control): %s\n", vkerr(rq));
    }

    /* G/H/K/M/N/O/P: controls isolating the 8888 read anomaly and splitting
     * it into INT-sampler vs FLOAT-sampler paths (header matrix). Own
     * dsl/pipelines/readback/pool; O/P additionally reuse K's bound granule
     * and img3 (D's class). d2_bad carries D's d2 mismatch count into the
     * matrix. */
    printf("\n=== G/H/K. trigger controls (tiled vs sparse vs mutable) ===");
    {
        VkDescriptorSetLayoutBinding dsb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        };
        VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  NULL, 0, 2, dsb };
        VkDescriptorSetLayout dsl;
        chk(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl), "create dsl(ghk)");
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
        VkPipelineLayout pl;
        chk(vkCreatePipelineLayout(dev, &plci, NULL, &pl), "create pl(ghk)");

        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .anisotropyEnable = VK_FALSE,
            .compareEnable = VK_FALSE,
            .unnormalizedCoordinates = VK_FALSE,
        };
        VkSampler sam;
        chk(vkCreateSampler(dev, &sci, NULL, &sam), "create sampler(ghk)");
        VkPipeline pipe_rgba = make_compute_pipe(dev, pl, cast_rgba_spv, (uint32_t)sizeof(cast_rgba_spv));
        VkPipeline pipe_r32 = make_compute_pipe(dev, pl, cast_r32_spv, (uint32_t)sizeof(cast_r32_spv));
        VkPipeline pipe_f32 = make_compute_pipe(dev, pl, cast_f32_spv, (uint32_t)sizeof(cast_f32_spv));
        chk(pipe_rgba != VK_NULL_HANDLE && pipe_r32 != VK_NULL_HANDLE &&
                pipe_f32 != VK_NULL_HANDLE ?
                VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED, "create pipes(ghk)");

        VkBufferCreateInfo rbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)W * H * 4,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer rb;
        chk(vkCreateBuffer(dev, &rbci, NULL, &rb), "create readback(ghk)");
        VkMemoryRequirements rbr;
        vkGetBufferMemoryRequirements(dev, rb, &rbr);
        VkDeviceMemory rbm;
        chk(alloc_mem(dev, rbr.size, 0, &rbm), "alloc readback(ghk)");
        chk(vkBindBufferMemory(dev, rb, rbm, 0), "bind readback(ghk)");

        VkDescriptorPoolSize dpsz[2] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 9 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 },
        };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 9, 2, dpsz };
        VkDescriptorPool dp;
        chk(vkCreateDescriptorPool(dev, &dpci, NULL, &dp), "create dpool(ghk)");

        uint32_t *fill1 = malloc((size_t)W * H * 4);
        uint32_t *fill2 = malloc((size_t)W * H * 4);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                /* same patterns as D: fill1 bytes (b0..b3) = (0x5A, x^y, x, y),
                 * fill2 = (0xA5, x+y, y, x) */
                fill1[y * W + x] = ((uint32_t)y << 24) | ((uint32_t)x << 16)
                                  | ((((uint32_t)(x ^ y)) << 8) & 0xFF00u) | 0x5A;
                fill2[y * W + x] = ((uint32_t)x << 24) | ((uint32_t)y << 16)
                                  | ((((uint32_t)(x + y)) << 8) & 0xFF00u) | 0xA5;
            }
        uint32_t *rbw = malloc((size_t)W * H * 4);

        /* G: non-sparse non-mutable TILED RGBA8 (baseline) */
        VkImageCreateInfo iig = ii3;
        iig.flags = 0;
        iig.pNext = NULL;
        VkImage img_g = VK_NULL_HANDLE;
        VkResult rcg = vkCreateImage(dev, &iig, NULL, &img_g);
        printf("G: vkCreateImage(OPTIMAL RGBA8, non-sparse): %s\n", vkerr(rcg));
        VkDeviceMemory mem_g = VK_NULL_HANDLE;
        if (rcg == VK_SUCCESS) {
            VkMemoryRequirements mgr;
            vkGetImageMemoryRequirements(dev, img_g, &mgr);
            printf("G: memreq size=0x%llx (0x100000 = plain tiled 512x512 8888; larger => UBWC metadata)\n",
                   (unsigned long long)mgr.size);
            chk(alloc_mem(dev, mgr.size, 0, &mem_g), "alloc G mem");
            chk(vkBindImageMemory(dev, img_g, mem_g, 0), "bind G mem");
        }

        /* H: non-sparse MUTABLE {RGBA8, R32_UINT} - D's class minus sparse */
        VkFormat h_fmts[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT };
        VkImageFormatListCreateInfo ifl_h = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, h_fmts };
        VkImageCreateInfo iih = iig;
        iih.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        iih.pNext = &ifl_h;
        VkImage img_h = VK_NULL_HANDLE;
        VkResult rch = vkCreateImage(dev, &iih, NULL, &img_h);
        printf("H: vkCreateImage(OPTIMAL MUTABLE {RGBA8,R32_UINT}, non-sparse): %s\n", vkerr(rch));
        VkDeviceMemory mem_h = VK_NULL_HANDLE;
        if (rch == VK_SUCCESS) {
            VkMemoryRequirements mhr;
            vkGetImageMemoryRequirements(dev, img_h, &mhr);
            chk(alloc_mem(dev, mhr.size, 0, &mem_h), "alloc H mem");
            chk(vkBindImageMemory(dev, img_h, mem_h, 0), "bind H mem");
        }

        /* K: SPARSE non-mutable RGBA8 - probe A's image class, shader-read */
        VkImageCreateInfo iik = ii3;
        iik.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        iik.pNext = NULL;
        VkImage img_k = VK_NULL_HANDLE;
        VkResult rck = vkCreateImage(dev, &iik, NULL, &img_k);
        printf("K: vkCreateImage(SPARSE non-mutable RGBA8): %s\n", vkerr(rck));
        VkExtent3D gran_k = { 0, 0, 0 };
        if (rck == VK_SUCCESS) {
            uint32_t sck = 0;
            vkGetImageSparseMemoryRequirements(dev, img_k, &sck, NULL);
            VkSparseImageMemoryRequirements smk[8];
            vkGetImageSparseMemoryRequirements(dev, img_k, &sck, smk);
            if (sck)
                gran_k = smk[0].formatProperties.imageGranularity;
            printf("K: gran=%ux%ux%u\n", gran_k.width, gran_k.height, gran_k.depth);
            if (gran_k.width) {
                VkDeviceMemory mem_k;
                chk(alloc_mem(dev, TILE, 0, &mem_k), "alloc K tile");
                unsigned char *pk = NULL;
                chk(vkMapMemory(dev, mem_k, 0, TILE, 0, (void **)&pk), "map K tile");
                memset(pk, 0x37, TILE);
                vkUnmapMemory(dev, mem_k);
                VkSparseImageMemoryBind bk = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 },
                                               gran_k, mem_k, 0, 0 };
                VkSparseImageMemoryBindInfo bki = { img_k, 1, &bk };
                VkBindSparseInfo bski = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL,
                                          1, &bki, 0, NULL };
                chk(vkQueueBindSparse(q, 1, &bski, NULL), "vkQueueBindSparse(K)");
                vkQueueWaitIdle(q);
            }
        }

        /* M: LINEAR-tiling 8888 - is the issue TILED-specific or general? */
        VkImageCreateInfo iim = iig;
        iim.tiling = VK_IMAGE_TILING_LINEAR;
        VkImage img_m = VK_NULL_HANDLE;
        VkResult rcm = vkCreateImage(dev, &iim, NULL, &img_m);
        printf("M: vkCreateImage(LINEAR RGBA8): %s\n", vkerr(rcm));
        VkDeviceMemory mem_m = VK_NULL_HANDLE;
        if (rcm == VK_SUCCESS) {
            VkMemoryRequirements mmr;
            vkGetImageMemoryRequirements(dev, img_m, &mmr);
            printf("M: memreq size=0x%llx (LINEAR 512x512 8888 = 0x100000)\n", (unsigned long long)mmr.size);
            chk(alloc_mem(dev, mmr.size, 0, &mem_m), "alloc M mem");
            chk(vkBindImageMemory(dev, img_m, mem_m, 0), "bind M mem");
        }

        /* Q: B8G8R8A8 (BGRA, the WXYZ-swap game format) TILED non-sparse.
         * Same fill2 bytes as G, read through the int sampler. A correct BGRA
         * int decode returns the channels (B,G,R,A)=(b0,b1,b2,b3) which pack to
         * exactly the fill2 word, so exact == fill2. If the 8-bit int decode
         * bug is format-agnostic, Q is TRANSFORMED like G. */
        VkImageCreateInfo iiq = iig;
        iiq.format = VK_FORMAT_B8G8R8A8_UNORM;
        VkImage img_q = VK_NULL_HANDLE;
        VkResult rcq = vkCreateImage(dev, &iiq, NULL, &img_q);
        printf("Q: vkCreateImage(OPTIMAL BGRA8, non-sparse): %s\n", vkerr(rcq));
        VkDeviceMemory mem_q = VK_NULL_HANDLE;
        if (rcq == VK_SUCCESS) {
            VkMemoryRequirements mqr;
            vkGetImageMemoryRequirements(dev, img_q, &mqr);
            chk(alloc_mem(dev, mqr.size, 0, &mem_q), "alloc Q mem");
            chk(vkBindImageMemory(dev, img_q, mem_q, 0), "bind Q mem");
        }

        if (rcg == VK_SUCCESS && rch == VK_SUCCESS && rck == VK_SUCCESS && rcm == VK_SUCCESS && gran_k.width) {
            VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img_g,
                                         VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                         (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY,
                                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                                              VK_COMPONENT_SWIZZLE_IDENTITY },
                                         (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            VkImageView view_g, view_h_rgba, view_h_r32, view_k;
            chk(vkCreateImageView(dev, &iv, NULL, &view_g), "view G");
            iv.image = img_h;
            chk(vkCreateImageView(dev, &iv, NULL, &view_h_rgba), "view H RGBA8");
            iv.format = VK_FORMAT_R32_UINT;
            chk(vkCreateImageView(dev, &iv, NULL, &view_h_r32), "view H R32_UINT");
            iv.image = img_k;
            iv.format = VK_FORMAT_R8G8B8A8_UNORM;
            chk(vkCreateImageView(dev, &iv, NULL, &view_k), "view K");
            iv.image = img_m;
            VkImageView view_m;
            chk(vkCreateImageView(dev, &iv, NULL, &view_m), "view M");
            /* P's view: 8888 view of the C/D image (img3, D's class). Only
             * exists when D ran; dii[7] falls back to view_k until then so
             * the update never sees an invalid handle. */
            VkImageView view_p = VK_NULL_HANDLE;
            if (rc3 == VK_SUCCESS) {
                iv.image = img3;
                chk(vkCreateImageView(dev, &iv, NULL, &view_p), "view P (img3 8888)");
            }
            VkImageView view_q = VK_NULL_HANDLE;
            if (rcq == VK_SUCCESS) {
                iv.image = img_q;
                iv.format = VK_FORMAT_B8G8R8A8_UNORM;
                chk(vkCreateImageView(dev, &iv, NULL, &view_q), "view Q (BGRA8)");
            }

            VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 9,
                                                 (VkDescriptorSetLayout[]){ dsl, dsl, dsl, dsl, dsl, dsl, dsl, dsl, dsl } };
            VkDescriptorSet dsets[9];
            chk(vkAllocateDescriptorSets(dev, &dsai, dsets), "alloc dsets(ghk)");
            VkDescriptorImageInfo dii[9] = {
                { sam, view_g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sam, view_h_rgba, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sam, view_h_r32, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sam, view_k, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sam, view_m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { sam, view_g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, /* N: float read of G */
                { sam, view_k, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, /* O: float read of K */
                { sam, view_k, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, /* P: float read of img3 */
                { sam, view_g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, /* Q: int read of BGRA */
            };
            dii[7].imageView = view_p != VK_NULL_HANDLE ? view_p : view_k;
            dii[8].imageView = view_q != VK_NULL_HANDLE ? view_q : view_g;
            VkDescriptorBufferInfo bfi = { rb, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet wr[18];
            for (int i = 0; i < 9; i++) {
                wr[i * 2] = (VkWriteDescriptorSet){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = dsets[i], .dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii[i] };
                wr[i * 2 + 1] = (VkWriteDescriptorSet){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = dsets[i], .dstBinding = 1, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bfi };
            }
            vkUpdateDescriptorSets(dev, 2 * 9, wr, 0, NULL);

            VkExtent3D full = { W, H, 1 };

            /* G: R2D fill2 + TP-8888 read, whole image */
            chk(cast_roundtrip(dev, q, pool, img_g, VK_IMAGE_LAYOUT_UNDEFINED, 0, full, fill2,
                               pipe_rgba, dsets[0], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "G roundtrip");
            int badg = compare_words(rbw, fill2, W, H, "G");
            if (badg)
                printf("G probe: FAIL - non-sparse tiled 8888 INT shader read TRANSFORMED\n"
                       "          (int-sampler decode; tiling/sparse state irrelevant)\n");
            else
                printf("G probe: PASS - non-sparse tiled 8888 read exact (baseline raw)\n");

            /* G2: R2D readback (CopyImageToBuffer) of G right after the fill -
             * does the R2D agree with itself, or is the R2D 8888 write off too? */
            {
                VkBuffer stg;
                VkDeviceMemory stgm;
                unsigned char *stgp = NULL;
                VkBufferCreateInfo stgci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                             (VkDeviceSize)W * H * 4,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
                chk(vkCreateBuffer(dev, &stgci, NULL, &stg), "create staging(G2)");
                VkMemoryRequirements stgr;
                vkGetBufferMemoryRequirements(dev, stg, &stgr);
                chk(alloc_mem(dev, stgr.size, 0, &stgm), "alloc staging(G2)");
                chk(vkBindBufferMemory(dev, stg, stgm, 0), "bind staging(G2)");
                VkCommandBuffer c;
                VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                                   pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
                vkAllocateCommandBuffers(dev, &ca, &c);
                VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
                vkBeginCommandBuffer(c, &bb);
                VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
                                            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, img_g,
                                            (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, NULL, 0, NULL, 1, &ib);
                VkBufferImageCopy ic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                         { 0, 0, 0 }, { W, H, 1 } };
                vkCmdCopyImageToBuffer(c, img_g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg, 1, &ic);
                vkEndCommandBuffer(c);
                VkSubmitInfo s = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &c, 0, NULL };
                vkQueueSubmit(q, 1, &s, NULL);
                vkQueueWaitIdle(q);
                vkFreeCommandBuffers(dev, pool, 1, &c);
                chk(vkMapMemory(dev, stgm, 0, stgr.size, 0, (void **)&stgp), "map staging(G2)");
                int badg2 = 0;
                for (int y = 0; y < H && badg2 < 8; y++)
                    for (int x = 0; x < W; x++)
                        if (((const uint32_t *)stgp)[y * W + x] != fill2[y * W + x]) {
                            printf("  G2 y%d x%d: R2D readback 0x%08x want 0x%08x\n", y, x,
                                   ((const uint32_t *)stgp)[y * W + x], fill2[y * W + x]);
                            badg2++;
                            break;
                        }
                if (badg2)
                    printf("G2 (R2D readback of G): MISMATCH - R2D 8888 write+read do not round-trip\n");
                else
                    printf("G2 (R2D readback of G): EXACT - R2D 8888 self-consistent (raw); TP 8888 view is the odd one out\n");
                vkUnmapMemory(dev, stgm);
                vkDestroyBuffer(dev, stg, NULL);
                vkFreeMemory(dev, stgm, NULL);
            }

            /* H d1': R2D fill1 + TP-R32 read, whole image */
            chk(cast_roundtrip(dev, q, pool, img_h, VK_IMAGE_LAYOUT_UNDEFINED, 0, full, fill1,
                               pipe_r32, dsets[2], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "H d1' roundtrip");
            int badh1 = compare_words(rbw, fill1, W, H, "H d1'");

            /* H d2': R2D fill2 + TP-8888 read, whole image */
            chk(cast_roundtrip(dev, q, pool, img_h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_SHADER_READ_BIT, full, fill2,
                               pipe_rgba, dsets[1], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "H d2' roundtrip");
            int badh2 = compare_words(rbw, fill2, W, H, "H d2'");
            if (!badh1 && !badh2)
                printf("H probe: PASS - non-sparse mutable-tiled cast exact in both directions\n");
            else if (badh2 && !badh1)
                printf("H probe: FAIL (int) - d2' (8888-INT) TRANSFORMED without sparse while\n"
                       "          d1' (R32-INT) exact -> int-8888 decode; sparse/mutable/tiling irrelevant\n");
            else
                printf("H probe: unexpected combination (d1' %s / d2' %s)\n",
                       badh1 ? "mismatch" : "exact", badh2 ? "mismatch" : "exact");

            /* K: R2D fill2 into the bound granule + TP-8888 read */
            chk(cast_roundtrip(dev, q, pool, img_k, VK_IMAGE_LAYOUT_UNDEFINED, 0, gran_k, fill2,
                               pipe_rgba, dsets[3], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "K roundtrip");
            int badk = compare_words(rbw, fill2, gran_k.width, gran_k.height, "K");
            if (badk)
                printf("K probe: FAIL (int) - SPARSE non-mutable 8888-INT read TRANSFORMED\n"
                       "          (same int-8888 pattern; sparse irrelevant)\n");
            else
                printf("K probe: PASS - sparse non-mutable 8888 read exact\n");

            /* M: R2D fill2 + TP-8888 read on the LINEAR image */
            chk(cast_roundtrip(dev, q, pool, img_m, VK_IMAGE_LAYOUT_UNDEFINED, 0, full, fill2,
                               pipe_rgba, dsets[4], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "M roundtrip");
            int badm = compare_words(rbw, fill2, W, H, "M");
            if (badm)
                printf("M probe: FAIL (int) - LINEAR 8888-INT read TRANSFORMED too\n"
                       "          (tiling irrelevant; int-sampler decode)\n");
            else
                printf("M probe: PASS - LINEAR 8888 read exact (issue is TILED-8888 specific)\n");

            /* Q: R2D fill2 + TP-8888 int read on the BGRA image (WXYZ swap) */
            int badq = 0;
            if (rcq == VK_SUCCESS) {
                chk(cast_roundtrip(dev, q, pool, img_q, VK_IMAGE_LAYOUT_UNDEFINED, 0, full, fill2,
                                   pipe_rgba, dsets[8], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                    "Q roundtrip");
                badq = compare_words(rbw, fill2, W, H, "Q");
                if (badq)
                    printf("Q probe: FAIL (int) - BGRA8 INT read TRANSFORMED too\n"
                           "          (int-8888 decode bug is format-agnostic: RGBA + BGRA both hit)\n");
                else
                    printf("Q probe: PASS - BGRA8 INT read exact (int bug specific to the RGBA order)\n");
            } else {
                printf("Q probe: SKIPPED (BGRA image not created)\n");
            }

            /* N: FLOAT-sampler TP read of G (the game-equivalent path: D3D12
             * texture2D is float sampling). Same image/view as G, which the
             * int probe above read TRANSFORMED. If N is exact, the anomaly is
             * in the integer-sampling path only; float-sampling games are
             * unaffected (img_g is in TRANSFER_SRC_OPTIMAL after G2). */
            chk(cast_roundtrip(dev, q, pool, img_g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, full, NULL,
                               pipe_f32, dsets[5], rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "N roundtrip");
            int badn = compare_words(rbw, fill2, W, H, "N");
            if (badn)
                printf("N probe: FAIL - FLOAT 8888 read TRANSFORMED too (game path affected)\n");
            else
                printf("N probe: PASS - float 8888 read exact; int-sampler path is the odd one out\n");

            /* O: FLOAT-sampler TP read of K (sparse non-mutable 8888).
             * K's bound granule still holds the fill2 written above; if
             * float is exact here, the anomaly does not reach the sparse
             * float path either (probe A's exact image class). */
            chk(cast_roundtrip(dev, q, pool, img_k, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_SHADER_READ_BIT, gran_k, NULL, pipe_f32, dsets[6],
                               rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                "O roundtrip");
            int bado = compare_words(rbw, fill2, gran_k.width, gran_k.height, "O");
            if (bado)
                printf("O probe: FAIL - SPARSE FLOAT 8888 read TRANSFORMED (sparse float path broken)\n");
            else
                printf("O probe: PASS - sparse non-mutable 8888 FLOAT read exact\n");

            /* P: FLOAT-sampler TP read of img3 - D's exact class (sparse +
             * mutable {RGBA8, R32_UINT}, the vkd3d TiledResources pattern).
             * gran3 holds d2's fill2; D3 proved the memory intact via R32.
             * If P is exact, the data is clean and only int-sampler 8888
             * reads lie -> the 12_0 gate-lift is safe for float sampling. */
            int badp = 0;
            if (rc3 == VK_SUCCESS) {
                chk(cast_roundtrip(dev, q, pool, img3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_ACCESS_SHADER_READ_BIT, gran3, NULL, pipe_f32, dsets[7],
                                   rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                    "P roundtrip");
                badp = compare_words(rbw, fill2, gran3.width, gran3.height, "P");
                if (badp)
                    printf("P probe: FAIL - SPARSE+MUTABLE (D class) FLOAT 8888 TRANSFORMED\n");
                else
                    printf("P probe: PASS - D class (vkd3d cast pattern) FLOAT read exact\n");
            } else {
                printf("P probe: SKIPPED (C/D image not created)\n");
            }

            /* R: the VALID integer path - a REAL R8G8B8A8_UINT image sampled
             * with the same usampler (pipe_rgba). Decisive for driver-vs-HW:
             *  - R exact       -> 8-bit int sampling works; the G/H/K/M/Q
             *    "transform" is the invalid UNORM-image + usampler combination
             *    (format-class mismatch -> undefined behavior, a test artifact).
             *  - R transformed -> a real 8-bit int sampling bug (driver/HW). */
            int badr = -1;
            {
                VkImageCreateInfo iir = iig;
                iir.format = VK_FORMAT_R8G8B8A8_UINT;
                VkImage img_r = VK_NULL_HANDLE;
                VkResult rcr = vkCreateImage(dev, &iir, NULL, &img_r);
                printf("R: vkCreateImage(OPTIMAL R8G8B8A8_UINT, non-sparse): %s\n", vkerr(rcr));
                if (rcr == VK_SUCCESS) {
                    VkMemoryRequirements mrr;
                    vkGetImageMemoryRequirements(dev, img_r, &mrr);
                    VkDeviceMemory mem_r;
                    chk(alloc_mem(dev, mrr.size, 0, &mem_r), "alloc R mem");
                    chk(vkBindImageMemory(dev, img_r, mem_r, 0), "bind R mem");
                    VkImageViewCreateInfo ivr = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img_r,
                                                  VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UINT,
                                                  (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                         VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                         VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                         VK_COMPONENT_SWIZZLE_IDENTITY },
                                                  (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                    VkImageView view_r;
                    chk(vkCreateImageView(dev, &ivr, NULL, &view_r), "view R");
                    VkDescriptorPoolSize dpszr[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 },
                                                      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
                    VkDescriptorPoolCreateInfo dpcr = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, dpszr };
                    VkDescriptorPool dpr;
                    chk(vkCreateDescriptorPool(dev, &dpcr, NULL, &dpr), "create dpool(R)");
                    VkDescriptorSetAllocateInfo dsar = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpr, 1,
                                                         (VkDescriptorSetLayout[]){ dsl } };
                    VkDescriptorSet dsr;
                    chk(vkAllocateDescriptorSets(dev, &dsar, &dsr), "alloc dset(R)");
                    VkDescriptorImageInfo diir = { sam, view_r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    VkDescriptorBufferInfo bfir = { rb, 0, VK_WHOLE_SIZE };
                    VkWriteDescriptorSet wrr[2] = {
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsr, .dstBinding = 0,
                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &diir },
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsr, .dstBinding = 1,
                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bfir },
                    };
                    vkUpdateDescriptorSets(dev, 2, wrr, 0, NULL);
                    chk(cast_roundtrip(dev, q, pool, img_r, VK_IMAGE_LAYOUT_UNDEFINED, 0, full, fill2,
                                       pipe_rgba, dsr, rbm, rbw) >= 0 ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                        "R roundtrip");
                    badr = compare_words(rbw, fill2, W, H, "R");
                    if (badr)
                        printf("R probe: FAIL - REAL R8G8B8A8_UINT int read TRANSFORMED -> real 8-bit int sampling bug (driver/HW)\n");
                    else
                        printf("R probe: PASS - REAL R8G8B8A8_UINT int read exact -> 8-bit int sampling works;\n"
                               "          G/H/K/M/Q 'transform' is the invalid UNORM+usampler combo (UB / test artifact)\n");
                    vkDestroyImageView(dev, view_r, NULL);
                    /* leak dsr/dpr: one-shot test, device teardown reclaims */
                } else {
                    badr = -1;
                    printf("R probe: SKIPPED (R8G8B8A8_UINT image not supported)\n");
                }
            }

            printf("\n=== trigger matrix (D-d2: %s) ===\n",
                   rc3 == VK_SUCCESS ? (d2_bad ? "transformed" : "exact") : "skipped");
            printf("  G  tiled, non-sparse, non-mutable 8888, INT:  %s\n", badg ? "TRANSFORMED" : "exact");
            printf("  H  tiled, non-sparse, mutable {RGBA8,R32}:    d1'(R32) %s / d2'(8888) %s\n",
                   badh1 ? "TRANSFORMED" : "exact", badh2 ? "TRANSFORMED" : "exact");
            printf("  K  tiled, sparse, non-mutable 8888, INT:      %s\n", badk ? "TRANSFORMED" : "exact");
            printf("  M  linear 8888, INT (tiling control):         %s\n", badm ? "TRANSFORMED" : "exact");
            printf("  Q  BGRA8 (WXYZ swap), INT:                    %s\n", badq ? "TRANSFORMED" : "exact");
            printf("  R  REAL R8G8B8A8_UINT, INT (valid path):     %s\n",
                   badr < 0 ? "skipped" : (badr ? "TRANSFORMED" : "exact"));
            printf("  N  float sampler on G (game path):            %s\n", badn ? "TRANSFORMED" : "exact");
            printf("  O  float sampler on K (sparse):               %s\n", bado ? "TRANSFORMED" : "exact");
            printf("  P  float on D class (sparse+mutable):         %s\n",
                   rc3 == VK_SUCCESS ? (badp ? "TRANSFORMED" : "exact") : "skipped");
            int r_ok = (badr == 0);   /* real R8G8B8A8_UINT int path exact */
            int r_bad = (badr > 0);   /* real R8G8B8A8_UINT int path transformed */
            if (!d2_bad && !badg && !badh1 && !badh2 && !badk && !badm && !badn && !bado && !badp && !r_bad)
                printf("  => no 8888 read anomaly reproduced (everything exact)\n");
            else if (badn || bado || (rc3 == VK_SUCCESS && badp))
                printf("  => FLOAT path also TRANSFORMED - decode-level 8888 bug; contradicts working games\n");
            else if (r_ok && (badg || badh2 || badk || badm || badq))
                printf("  => ROOT CAUSE: the 'transform' is the INVALID combo UNORM-image + usampler\n"
                       "     (format-class mismatch -> undefined behavior, a test artifact).\n"
                       "     R (REAL R8G8B8A8_UINT + usampler) is EXACT -> real 8-bit int sampling\n"
                       "     WORKS on A740. No driver/HW bug, no Mesa fix. D3D12 int path (real\n"
                       "     UINT) is clean; 12_0 gate-lift safe.\n");
            else if (r_bad)
                printf("  => REAL 8-bit int sampling bug: R (real R8G8B8A8_UINT + usampler)\n"
                       "     TRANSFORMED -> A740 TP 8-bit int decode defect (file upstream Mesa issue)\n");
            else
                printf("  => INT-sampler 8888 anomaly; R inconclusive (skipped) - re-run to distinguish\n");
        } else {
            printf("G/H/K/M/N/O/P: INCOMPLETE (G=%s H=%s K=%s M=%s granK=%u) - matrix unavailable\n",
                   vkerr(rcg), vkerr(rch), vkerr(rck), vkerr(rcm), gran_k.width);
        }

        /* S. P4 (0005) probe: BGRA8 sparse+mutable with a UNIFORM-SWAP list
         * {BGRA8_UNORM, BGRA8_UINT} - the upstream TODO case (tu_image.cc).
         * Expected per build:
         *  - stock 26.2.2:  create silently yields linear+sparse (NDEBUG),
         *    the E/F class - the hole;
         *  - 0004 (P1+P3):  create rejected (FEATURE_NOT_PRESENT), honest;
         *  - 0005 (P4):     create OK, tiled+sparse, both views exact.
         * The fill goes in as BGRA bytes (b0..b3) = (B,G,R,A); the view
         * readback packs components as R|G<<8|B<<16|A<<24, so `want` is
         * fill2 with b0<->b2 swapped. */
        {
            VkFormat bg_list[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UINT };
            VkImageFormatListCreateInfo ifl_bg = {
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, bg_list };

            VkPhysicalDeviceImageFormatInfo2 ifi = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                .pNext = &ifl_bg,
                .format = VK_FORMAT_B8G8R8A8_UNORM,
                .type = VK_IMAGE_TYPE_2D,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                         VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                         VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            };
            VkImageFormatProperties2 ifp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, NULL };
            VkResult rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
            printf("S  query(SPARSE+MUTABLE {BGRA8_UNORM,BGRA8_UINT}): %s\n", vkerr(rq));

            VkImageCreateInfo iis = iig;
            iis.format = VK_FORMAT_B8G8R8A8_UNORM;
            iis.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                        VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            iis.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            iis.pNext = &ifl_bg;
            VkImage img_s = VK_NULL_HANDLE;
            VkResult rcs = vkCreateImage(dev, &iis, NULL, &img_s);
            printf("S  vkCreateImage(MUTABLE+SPARSE {BGRA8_UNORM,BGRA8_UINT}): %s\n", vkerr(rcs));
            int bad_u = -1, bad_n = -1;
            if (rcs == VK_SUCCESS) {
                uint32_t scs = 0;
                vkGetImageSparseMemoryRequirements(dev, img_s, &scs, NULL);
                VkSparseImageMemoryRequirements sms[4];
                if (scs > 4)
                    scs = 4;
                vkGetImageSparseMemoryRequirements(dev, img_s, &scs, sms);
                VkExtent3D grans = { 64, 64, 1 };
                if (scs)
                    grans = sms[0].formatProperties.imageGranularity;
                printf("S  sparse gran=%ux%ux%u\n", grans.width, grans.height, grans.depth);

                if ((VkDeviceSize)grans.width * grans.height * 4 > TILE) {
                    printf("S  SKIPPED data check (granule exceeds %uK tile)\n", TILE / 1024);
                } else {
                    VkDeviceMemory smem;
                    chk(alloc_mem(dev, TILE, 0, &smem), "alloc S tile");
                    unsigned char *spm = NULL;
                    chk(vkMapMemory(dev, smem, 0, TILE, 0, (void **)&spm), "map S tile");
                    memset(spm, 0x5E, TILE);
                    vkUnmapMemory(dev, smem);
                    VkSparseImageMemoryBind bs = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 },
                                                   { 0, 0, 0 }, grans, smem, 0, 0 };
                    VkSparseImageMemoryBindInfo bis = { img_s, 1, &bs };
                    VkBindSparseInfo bsis = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL,
                                              0, NULL, 0, NULL, 1, &bis, 0, NULL };
                    chk(vkQueueBindSparse(q, 1, &bsis, NULL), "vkQueueBindSparse(S)");
                    vkQueueWaitIdle(q);

                    VkImageViewCreateInfo ivs = {
                        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img_s,
                        VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UINT,
                        (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY },
                        (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                    VkImageView view_su;
                    chk(vkCreateImageView(dev, &ivs, NULL, &view_su), "view S UINT");
                    ivs.format = VK_FORMAT_B8G8R8A8_UNORM;
                    VkImageView view_sn;
                    chk(vkCreateImageView(dev, &ivs, NULL, &view_sn), "view S UNORM");

                    VkDescriptorPoolSize dpsz_s[2] = {
                        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
                    VkDescriptorPoolCreateInfo dpc_s = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 2, dpsz_s };
                    VkDescriptorPool dp_s;
                    chk(vkCreateDescriptorPool(dev, &dpc_s, NULL, &dp_s), "create dpool(S)");
                    VkDescriptorSetAllocateInfo dsa_s = {
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp_s, 2,
                        (VkDescriptorSetLayout[]){ dsl, dsl } };
                    VkDescriptorSet dss[2];
                    chk(vkAllocateDescriptorSets(dev, &dsa_s, dss), "alloc dsets(S)");
                    VkDescriptorImageInfo dii_s[2] = {
                        { sam, view_su, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                        { sam, view_sn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
                    VkDescriptorBufferInfo bfi_s = { rb, 0, VK_WHOLE_SIZE };
                    VkWriteDescriptorSet wrs[4] = {
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dss[0],
                          .dstBinding = 0, .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          .pImageInfo = &dii_s[0] },
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dss[0],
                          .dstBinding = 1, .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = &bfi_s },
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dss[1],
                          .dstBinding = 0, .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          .pImageInfo = &dii_s[1] },
                        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dss[1],
                          .dstBinding = 1, .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = &bfi_s } };
                    vkUpdateDescriptorSets(dev, 4, wrs, 0, NULL);

                    /* fill2 bytes (b0..b3) = (0xA5, x+y, y, x) = (B, G, R, A)
                     * for the BGRA8 base; the view readback packs
                     * R|G<<8|B<<16|A<<24, so `want` is fill2 with b0<->b2
                     * swapped. */
                    uint32_t *want = malloc((size_t)W * H * 4);
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++)
                            want[y * W + x] = (uint32_t)(y & 0xFF) |
                                              ((((uint32_t)(x + y)) & 0xFF) << 8) |
                                              (0xA5u << 16) |
                                              ((uint32_t)(x & 0xFF) << 24);

                    /* s-uint: UINT view + usampler (same format class, no poison) */
                    chk(cast_roundtrip(dev, q, pool, img_s, VK_IMAGE_LAYOUT_UNDEFINED, 0,
                                       grans, fill2, pipe_rgba, dss[0], rbm, rbw) >= 0 ?
                            VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                        "S roundtrip (uint)");
                    bad_u = compare_words(rbw, want, grans.width, grans.height, "s-uint");
                    /* s-unorm: UNORM view + sampler (the D3D12 float path) */
                    chk(cast_roundtrip(dev, q, pool, img_s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_ACCESS_SHADER_READ_BIT, grans, fill2, pipe_f32, dss[1],
                                       rbm, rbw) >= 0 ?
                            VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                        "S roundtrip (unorm)");
                    bad_n = compare_words(rbw, want, grans.width, grans.height, "s-unorm");
                    /* S-tr: discriminator - read the same region back via
                     * transfer (CopyImageToBuffer): raw bytes come out in the
                     * base format's byte order (BGRA), so compare against the
                     * original fill2 words. Exact => the fill+binding are
                     * fine and the zero texels above are a TP-sampling defect
                     * of the linear+sparse image (the hole); same zeros =>
                     * the fill never reached those texels. */
                    {
                        VkBufferCreateInfo tci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                                   (VkDeviceSize)grans.width * grans.height * 4,
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
                        VkBuffer tb;
                        chk(vkCreateBuffer(dev, &tci, NULL, &tb), "create S-tr buf");
                        VkMemoryRequirements tr;
                        vkGetBufferMemoryRequirements(dev, tb, &tr);
                        VkDeviceMemory tbm;
                        chk(alloc_mem(dev, tr.size, 0, &tbm), "alloc S-tr mem");
                        chk(vkBindBufferMemory(dev, tb, tbm, 0), "bind S-tr mem");

                        VkCommandBuffer tcb;
                        VkCommandBufferAllocateInfo tcai = {
                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
                        chk(vkAllocateCommandBuffers(dev, &tcai, &tcb), "alloc S-tr cb");
                        VkCommandBufferBeginInfo tbbi = {
                            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
                        chk(vkBeginCommandBuffer(tcb, &tbbi), "begin S-tr cb");
                        VkBufferImageCopy tic = { 0, grans.width, 0,
                                                  { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                                  { 0, 0, 0 }, { grans.width, grans.height, 1 } };
                        vkCmdCopyImageToBuffer(tcb, img_s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               tb, 1, &tic);
                        chk(vkEndCommandBuffer(tcb), "end S-tr cb");
                        VkSubmitInfo tsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL,
                                             1, &tcb, 0, NULL };
                        chk(vkQueueSubmit(q, 1, &tsi, NULL), "submit S-tr");
                        vkQueueWaitIdle(q);
                        vkFreeCommandBuffers(dev, pool, 1, &tcb);

                        unsigned char *tp;
                        chk(vkMapMemory(dev, tbm, 0, tr.size, 0, (void **)&tp), "map S-tr");
                        int badtr = 0, ftx = -1, fty = -1;
                        uint32_t ftw = 0, fww = 0;
                        for (int y = 0; y < (int)grans.height; y++)
                            for (int x = 0; x < (int)grans.width; x++) {
                                uint32_t w;
                                memcpy(&w, tp + (size_t)(y * grans.width + x) * 4, 4);
                                if (w != fill2[y * W + x]) {
                                    badtr++;
                                    if (ftx < 0) {
                                        ftx = x; fty = y; ftw = w; fww = fill2[y * W + x];
                                    }
                                }
                            }
                        if (badtr)
                            printf("S-tr first mismatch at (%d, %d): got 0x%08x want 0x%08x\n",
                                   ftx, fty, ftw, fww);
                        printf("S-tr transfer readback: %d/%u mismatches (raw bytes vs fill2)%s\n",
                               badtr, (unsigned)(grans.width * grans.height),
                               badtr ? " - fill/binding defect" : " - fill+bind exact");
                        vkUnmapMemory(dev, tbm);
                    }
                    free(want);
                    if (!bad_u && !bad_n)
                        printf("S probe: PASS - uniform-swap BGRA+UINT sparse+mutable: "
                               "create OK, UINT view exact, UNORM view exact (tiled+sparse)\n");
                    else
                        printf("S probe: FAIL - s-uint=%d s-unorm=%d mismatches (TP read). "
                               "On stock this is the linear+sparse hole (create succeeds "
                               "only because feature/query lie); on 0005/P4 it would mean "
                               "the tiled+sparse uniform-swap path is broken\n",
                               bad_u, bad_n);
                }
            } else {
                printf("S probe: create rejected - expected on a 0004-only build "
                       "(honest hole closure); 0005/P4 makes this tiled+sparse\n");
            }
        }

        /* S2. controls: mixed-swap and NULL lists must STAY rejected (query and
         * create) on 0004/0005; stock 26.2.2 silently creates linear+sparse. */
        {
            VkFormat mixed_list[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
            VkImageFormatListCreateInfo ifl_mx = {
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, mixed_list };
            VkPhysicalDeviceImageFormatInfo2 ifi = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                .pNext = &ifl_mx,
                .format = VK_FORMAT_B8G8R8A8_UNORM,
                .type = VK_IMAGE_TYPE_2D,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                         VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                         VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            };
            VkImageFormatProperties2 ifp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, NULL };
            VkResult rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
            printf("S2 query(SPARSE+MUTABLE {BGRA8,RGBA8}, mixed swap): %s\n", vkerr(rq));
            ifi.pNext = NULL;
            rq = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
            printf("S2 query(SPARSE+MUTABLE, no list):                  %s\n", vkerr(rq));

            VkImageCreateInfo iim = iig;
            iim.format = VK_FORMAT_B8G8R8A8_UNORM;
            iim.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                        VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            iim.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            iim.pNext = &ifl_mx;
            VkImage img_mx = VK_NULL_HANDLE;
            VkResult rcm = vkCreateImage(dev, &iim, NULL, &img_mx);
            printf("S2 vkCreateImage(MUTABLE+SPARSE {BGRA8,RGBA8}): %s%s\n", vkerr(rcm),
                   rcm == VK_SUCCESS ? " (stock: silent linear+sparse - the hole)" : "");
            iim.pNext = NULL;
            VkImage img_nl = VK_NULL_HANDLE;
            VkResult rcn = vkCreateImage(dev, &iim, NULL, &img_nl);
            printf("S2 vkCreateImage(MUTABLE+SPARSE, no list):       %s%s\n", vkerr(rcn),
                   rcn == VK_SUCCESS ? " (stock: silent linear+sparse - the hole)" : "");
        }

        /* T. P6 (0006) probe: NON-sparse MUTABLE {BGRA8_UNORM, BGRA8_UINT},
         * OPTIMAL tiling - the D3D12 B8G8R8A8_TYPELESS class (the dominant
         * D3D12 texture format). Expected per build:
         *  - stock/0004+0005: B8G8R8A8_UINT maps to UNKNOWN_COMPAT -> list
         *    not UBWC-compatible ("incompatible list + swap") -> UBWC off +
         *    format_list_has_swaps -> LINEAR, memreq 0x100000;
         *  - 0006: B8G8R8A8 UNORM/SRGB/SNORM/UINT/SINT unify on the INT
         *    compat type (ubwc_unorm_snorm_int_compatible) -> list compatible
         *    -> UBWC stays on, tiled, memreq 0x102000 - and both views must
         *    still read EXACT (same UBWC mode across the cast, like S).
         * want semantics: fill bytes (B,G,R,A); view readback packs
         * R|G<<8|B<<16|A<<24, so want = fill2 with B<->R swapped (as in S). */
        {
            VkFormat bg_list_t[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UINT };
            VkImageFormatListCreateInfo ifl_bt = {
                VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, bg_list_t };

            VkImageCreateInfo iit = iig;
            iit.format = VK_FORMAT_B8G8R8A8_UNORM;
            iit.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            iit.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            iit.pNext = &ifl_bt;
            VkImage img_t = VK_NULL_HANDLE;
            VkResult rct = vkCreateImage(dev, &iit, NULL, &img_t);
            printf("T  vkCreateImage(OPTIMAL MUTABLE {BGRA8_UNORM,BGRA8_UINT}): %s\n", vkerr(rct));
            if (rct == VK_SUCCESS) {
                VkMemoryRequirements treq;
                vkGetImageMemoryRequirements(dev, img_t, &treq);
                int t_ubwc = treq.size > 0x100000;
                printf("T  memreq size=0x%llx %s\n",
                       (unsigned long long)treq.size,
                       t_ubwc ? "(0x102000 => tiled + UBWC metadata, P6/0006)"
                              : "(=== 0x100000 => no UBWC metadata: linear or plain tiled)");
                VkDeviceMemory tmem;
                chk(alloc_mem(dev, treq.size, 0, &tmem), "alloc T mem");
                chk(vkBindImageMemory(dev, img_t, tmem, 0), "bind T mem");

                VkImageViewCreateInfo ivt = {
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img_t,
                    VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UINT,
                    (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY },
                    (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                VkImageView view_tu;
                chk(vkCreateImageView(dev, &ivt, NULL, &view_tu), "view T UINT");
                ivt.format = VK_FORMAT_B8G8R8A8_UNORM;
                VkImageView view_tn;
                chk(vkCreateImageView(dev, &ivt, NULL, &view_tn), "view T UNORM");

                VkDescriptorPoolSize dpsz_t[2] = {
                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
                VkDescriptorPoolCreateInfo dpc_t = {
                    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 2, dpsz_t };
                VkDescriptorPool dp_t;
                chk(vkCreateDescriptorPool(dev, &dpc_t, NULL, &dp_t), "create dpool(T)");
                VkDescriptorSetAllocateInfo dsa_t = {
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp_t, 2,
                    (VkDescriptorSetLayout[]){ dsl, dsl } };
                VkDescriptorSet dst[2];
                chk(vkAllocateDescriptorSets(dev, &dsa_t, dst), "alloc dsets(T)");
                VkDescriptorImageInfo dii_t[2] = {
                    { sam, view_tu, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                    { sam, view_tn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
                VkDescriptorBufferInfo bfi_t = { rb, 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet wrt[4] = {
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dst[0],
                      .dstBinding = 0, .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                      .pImageInfo = &dii_t[0] },
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dst[0],
                      .dstBinding = 1, .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      .pBufferInfo = &bfi_t },
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dst[1],
                      .dstBinding = 0, .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                      .pImageInfo = &dii_t[1] },
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dst[1],
                      .dstBinding = 1, .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      .pBufferInfo = &bfi_t } };
                vkUpdateDescriptorSets(dev, 4, wrt, 0, NULL);

                /* The view readback (TP) packs the sampled channels as
                 * R|G<<8|B<<16|A<<24 and the fill bytes (b0..b3) = (B,G,R,A),
                 * so want = fill2 with b0<->b2 swapped. Derive it from the
                 * fill word itself (not from x/y): the fill pattern stores
                 * y<<16 unmasked, so for y>=256 y's high bits land in byte 3
                 * (x's slot) - replicate that exactly (S used gran<=128 so it
                 * never hit the leak). */
                uint32_t *twant = malloc((size_t)W * H * 4);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        uint32_t f = fill2[y * W + x];
                        twant[y * W + x] = ((f & 0x00FF0000u) >> 16) |
                                           (f & 0x0000FF00u) |
                                           ((f & 0x000000FFu) << 16) |
                                           (f & 0xFF000000u);
                    }

                VkExtent3D full_ext = { W, H, 1 };
                int t_bad_u = -1, t_bad_n = -1, t_bad_tr = 0;
                chk(cast_roundtrip(dev, q, pool, img_t, VK_IMAGE_LAYOUT_UNDEFINED, 0,
                                   full_ext, fill2, pipe_rgba, dst[0], rbm, rbw) >= 0 ?
                        VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                    "T roundtrip (uint)");
                t_bad_u = compare_words(rbw, twant, W, H, "t-uint");
                chk(cast_roundtrip(dev, q, pool, img_t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_ACCESS_SHADER_READ_BIT, full_ext, fill2, pipe_f32, dst[1],
                                   rbm, rbw) >= 0 ?
                        VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY,
                    "T roundtrip (unorm)");
                t_bad_n = compare_words(rbw, twant, W, H, "t-unorm");

                /* T-tr: raw transfer readback vs fill2 - the UBWC verdict.
                 * Bytes must round-trip unchanged through compress-on-store /
                 * decompress-on-copy; this is independent of any TP-swap
                 * modelling and proves UBWC does not corrupt the cast. */
                {
                    VkBufferCreateInfo tci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                               (VkDeviceSize)W * H * 4,
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
                    VkBuffer tb;
                    chk(vkCreateBuffer(dev, &tci, NULL, &tb), "create T-tr buf");
                    VkMemoryRequirements trr;
                    vkGetBufferMemoryRequirements(dev, tb, &trr);
                    VkDeviceMemory tbm;
                    chk(alloc_mem(dev, trr.size, 0, &tbm), "alloc T-tr mem");
                    chk(vkBindBufferMemory(dev, tb, tbm, 0), "bind T-tr mem");

                    VkCommandBuffer tcb;
                    VkCommandBufferAllocateInfo tcai = {
                        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
                    chk(vkAllocateCommandBuffers(dev, &tcai, &tcb), "alloc T-tr cb");
                    VkCommandBufferBeginInfo tbbi = {
                        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
                    chk(vkBeginCommandBuffer(tcb, &tbbi), "begin T-tr cb");
                    VkBufferImageCopy tic = { 0, W, 0,
                                              { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                              { 0, 0, 0 }, { W, H, 1 } };
                    vkCmdCopyImageToBuffer(tcb, img_t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           tb, 1, &tic);
                    chk(vkEndCommandBuffer(tcb), "end T-tr cb");
                    VkSubmitInfo tsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL,
                                         1, &tcb, 0, NULL };
                    chk(vkQueueSubmit(q, 1, &tsi, NULL), "submit T-tr");
                    vkQueueWaitIdle(q);
                    vkFreeCommandBuffers(dev, pool, 1, &tcb);

                    unsigned char *tp;
                    chk(vkMapMemory(dev, tbm, 0, trr.size, 0, (void **)&tp), "map T-tr");
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            uint32_t w;
                            memcpy(&w, tp + (size_t)(y * W + x) * 4, 4);
                            if (w != fill2[y * W + x])
                                t_bad_tr++;
                        }
                    printf("T-tr transfer readback: %d/%u mismatches (raw bytes vs fill2)%s\n",
                           t_bad_tr, (unsigned)(W * H),
                           t_bad_tr ? " - UBWC roundtrip defect"
                                    : " - bytes round-trip exact through UBWC");
                    vkUnmapMemory(dev, tbm);
                }

                if (t_ubwc)
                    printf("T probe: %s - UBWC+tiled (memreq 0x%llx); UINT view read=%s, "
                           "UNORM view read=%s, raw roundtrip=%s\n",
                           (!t_bad_u && !t_bad_n && !t_bad_tr) ? "PASS" : "FAIL",
                           (unsigned long long)treq.size,
                           t_bad_u ? "MISMATCH" : "EXACT",
                           t_bad_n ? "MISMATCH" : "EXACT",
                           t_bad_tr ? "MISMATCH" : "EXACT");
                else
                    printf("T probe: baseline (no UBWC, %s); UINT view read=%s, "
                           "UNORM view read=%s, raw roundtrip=%s%s\n",
                           treq.size == 0x100000 ? "linear" : "tiled",
                           t_bad_u ? "MISMATCH" : "EXACT",
                           t_bad_n ? "MISMATCH" : "EXACT",
                           t_bad_tr ? "MISMATCH" : "EXACT",
                           t_bad_tr ? " (linear: no UBWC to round-trip through)" : "");
                free(twant);
            } else {
                printf("T probe: create rejected - unexpected for non-sparse mutable BGRA8\n");
            }
        }

        free(fill1);
        free(fill2);
        free(rbw);
    }

    vkQueueWaitIdle(q);
    printf("DONE\n");
    return 0;
}
