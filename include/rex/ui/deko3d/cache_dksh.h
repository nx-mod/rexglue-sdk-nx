// rex::deko3d (from nfsmw-nx): the DKSH cache file (docs/HowToDeko3d.md, section 3.3).
//
// One DKSH per (SPIR-V, specialization value, variant): the SPIR-V the renderer converts (a library shader, or
// its color-write pruning or early depth variant), constant_id 0 masked to the bits the shader reads
// (MascaraEspecializacion), and for a vertex shader the interpolators it keeps (SpirvAGlsl's `salidas`; 0 for
// pixel shaders). The file is made by shaders/dksh/nfsmw_dksh on the PC, or by the console's compile worker, and
// kept on the SD card. It is made from the game's shaders, so it is never distributed.
//
// The DKSH depends on more than the key: uam and its patches, SPIRV-Cross, and sdk/src/ui/deko3d/glsl.cpp. Their identity
// (IdCompilador) is in the header, and a cache made by another one is not loaded: the lesson of NVK's shader
// cache, which kept old compiled shaders after a compiler fix (docs/shaders.md).
//
// Layout, little-endian:
//   0  "NFDK"            4  version (2)        8  XXH3 of IdCompilador()
//   16 entries           20 0                  24 XXH3 of the bytes from 32 to the end
//   32 entries x {u64 SPIR-V fingerprint, u32 specialization value, u32 variant, u32 DKSH bytes, u32 0}, sorted
//      by key
//   then the DKSH of each entry, in the same order, each starting at a multiple of 4.

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::deko3d {

// "uam ... ; SPIRV-Cross ... ; GLSL revision N". Change the first two with tools/fetch_thirdparty.py's commits.
std::string IdCompilador();

// XXH3 of the SPIR-V words as given to SpirvAGlsl.
uint64_t HuellaSpirv(std::span<const uint32_t> espirv);

class CacheDksh {
 public:
  // Replaces the content with the file's. false (and empty) if it is not a cache of this compiler or is damaged;
  // `motivo` says why, to log it.
  bool Cargar(std::span<const uint8_t> archivo, std::string& motivo);
  std::vector<uint8_t> Serializar() const;

  // The journal beside the file: what Anadir added since the last SerializarNuevas, appended to it instead of
  // writing the whole file again (18 MB with Halo 3's shaders, every 20 s while new ones compile). Layout: "NFDJ",
  // version, XXH3 of IdCompilador(), then entries {u64 fingerprint, u32 specialization, u32 variant, u32 bytes,
  // u32 low half of the DKSH's XXH3} each followed by its DKSH padded to 4. With `cabecera` the 16-byte header goes
  // first (a new journal).
  std::vector<uint8_t> SerializarNuevas(bool cabecera);
  bool HayNuevas() const { return !nuevas_.empty(); }
  // Adds a journal's entries; stops at the first one cut short or damaged (a write the console did not finish).
  // The number added, or -1 if it is not a journal of this compiler.
  int CargarDiario(std::span<const uint8_t> diario);

  struct Clave {
    uint64_t huella = 0;
    uint32_t especializacion = 0;
    uint32_t variante = 0;
    auto operator<=>(const Clave&) const = default;
  };
  // nullptr if it is not there. Valid until the next Anadir or Cargar.
  const std::vector<uint8_t>* Buscar(const Clave& clave) const;
  void Anadir(const Clave& clave, std::vector<uint8_t> dksh);
  size_t size() const { return entradas_.size(); }
  const std::map<Clave, std::vector<uint8_t>>& entradas() const { return entradas_; }

 private:
  std::map<Clave, std::vector<uint8_t>> entradas_;
  std::vector<Clave> nuevas_;  // added since the last SerializarNuevas
};

}  // namespace rex::deko3d

// NFS's renderer code (nfsmw_pal/src/nfsmw_dk_*) still names these nfsmw::deko::..., until it is renamed.
namespace nfsmw::deko {
using namespace ::rex::deko3d;
}  // namespace nfsmw::deko
