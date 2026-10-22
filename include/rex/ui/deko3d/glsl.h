// rex::deko3d (from nfsmw-nx): the SPIR-V of the deko3d shader library as GLSL that uam (devkitPro's deko3d shader
// compiler) accepts. docs/HowToDeko3d.md, section 3.
//
// The deko3d library is the same HLSL as the Vulkan one compiled with -DNFSMW_DEKO3D (shader_common.h): no
// descriptor heaps, no push constants, no 64-bit integers, and each Xenos sampler register R as a texture and a
// sampler at binding R. SPIRV-Cross writes it as GLSL 4.60 without Vulkan semantics; this adds what uam needs on
// top: every texture with an explicit binding, and the specialization constant with its value, so one GLSL (and
// one DKSH) per (shader, value).
//
// Pure C++ with SPIRV-Cross: built for the PC (shaders/nfsmw_dksh.cpp, the offline DKSH cache) and the Switch
// (the compile worker), with the same output on both.

#pragma once

#include <cstdint>
#include <span>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace rex::deko3d {

// Raise it when the GLSL this writes changes for the same input: it is part of the DKSH cache id
// (rex/ui/deko3d/cache_dksh.h), and a cache made with another revision must not be loaded.
inline constexpr uint32_t kRevisionGlsl = 1;

// deko3d's limits per stage (uam's README): UBO and texture bindings.
inline constexpr uint32_t kMaxUbos = 16;
inline constexpr uint32_t kMaxTexturas = 32;

struct GlslDeko {
  std::string glsl;  // empty if the conversion failed (see error)
  bool vertices = false;
  uint32_t ubos = 0;      // bit N: the shader reads the UBO at binding N
  uint32_t texturas = 0;  // bit N: it samples or queries the texture at binding N
  uint32_t texturas_3d = 0;    // of those, the 3D ones
  uint32_t texturas_cubo = 0;  // and the cube ones (the rest are 2D)
  // Interpolator locations (TEXCOORDn = n, COLORn = 16 + n): what a pixel shader reads, what a vertex shader
  // still writes after `salidas` (SpirvAGlsl).
  uint32_t interpoladores = 0;
  // Storage buffers (Halo 3's vertex fetches with no vertex element, g_vf<slot> in set 5): bit N = binding N is read,
  // slots_vf[N] = the vertex fetch slot it holds (from the name).
  uint32_t buffers = 0;
  std::array<uint8_t, 16> slots_vf{};
  // What each of those reads around vertex i (the translator's "// xeno_vf" lines): dwords i * stride + [min, max).
  // Stride 0 = unknown or mixed: the whole buffer.
  std::array<uint32_t, 16> zancada_vf{};
  std::array<int32_t, 16> minimo_vf{}, maximo_vf{};
  std::string error;
  // Shapes that compile but that uam is known to get wrong (HowToDeko3d.md, section 6): a dynamic component
  // index into a UBO vector. One line each; the caller logs them.
  std::vector<std::string> avisos;
};

// `espirv` is a deko3d library shader, or the renderer's SPIR-V variant of one (the color-write pruning and the
// early depth test work on SPIR-V, before this). `especializacion` is the value of constant_id 0
// (g_SpecConstants), the pipeline key's especializacion.
//
// `salidas` (vertex shaders): the interpolator locations to keep, normally the pixel shader's `interpoladores`.
// Every library vertex shader writes all 22; NVK drops the ones the pixel shader does not read when it links the
// pipeline, and uam has no linking, so the others are removed here and Mesa's front end drops what only fed them.
// ~0u keeps them all.
GlslDeko SpirvAGlsl(std::span<const uint32_t> espirv, uint32_t especializacion, uint32_t salidas = ~0u);

/*
 * The bits of the specialization value that change this shader's code. The pipeline key has one value for both
 * stages, but a vertex shader reads none of it and a pixel shader at most 7 bits (alpha test and its function,
 * 1/size, cheap PCF, shadow minimum, no blur), so a DKSH is made and looked up for `value & mask` only: one per
 * vertex shader instead of one per pipeline.
 *
 * Every use of the constant in the deko3d library is an OpSpecConstantOp: an AND with a literal (its bits go in
 * the mask), or tfetch2DSombraMin's comparison with NFSMW_MARCA_SOMBRA_MINIMO, which has bits 24-30 set and is
 * never equal to a value the renderer uses (shader_common.h), so it reads nothing. Any other use gives ~0u: no
 * masking, correct and only slower to compile.
 */
uint32_t MascaraEspecializacion(std::span<const uint32_t> espirv);

/*
 * GLSL a game's translator wrote at run time (XenosRecomp's XENOS_GLSL mode, its prelude included, no #version):
 * the same result as SpirvAGlsl, with #version 460 and G_ESPECIALIZACION put in front, and what the shader reads
 * taken from its declarations' text:
 *   layout(location = k) in/out ...                  interpolators (a pixel shader's inputs, a vertex shader's outputs)
 *   layout(binding = N) uniform samplerXX ...        textures
 *   layout(std430, binding = N) readonly buffer XenoVf<slot>   storage buffers (Halo 3's element-less vertex fetches)
 *   layout(std140, binding = N) uniform ...          UBOs
 * Every vertex shader output stays (no `salidas`): the translator writes at most 16 and uam drops the dead ones only
 * as far as it can without linking.
 */
GlslDeko GlslDeTraductor(std::string_view texto, bool vertices, uint32_t especializacion);

// The specialization bits the translator's GLSL reads (shader_common.glsl): 1/size for both stages, the alpha test
// and its function for pixel shaders.
uint32_t MascaraTraductor(bool vertices);

}  // namespace rex::deko3d

// NFS's renderer code (nfsmw_pal/src/nfsmw_dk_*) still names these nfsmw::deko::..., until it is renamed.
namespace nfsmw::deko {
using namespace ::rex::deko3d;
}  // namespace nfsmw::deko
