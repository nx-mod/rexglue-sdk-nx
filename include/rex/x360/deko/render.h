// nfsmw - deko3d renderer: the renderer backend (DestinosNativos) on deko3d. Switch only.
//
// One class, RenderDk, in three files:
//   nfsmw_dk_destinos.cpp   frames and work slots, render targets, copies (resolve and clear), the output pass with
//                           the gamma ramp, statistics;
//   nfsmw_dk_dibujos.cpp    each draw: shaders and their DKSH, vertex input, indices, constants, state;
//   nfsmw_dk_texturas.cpp   game textures (fingerprints, untiling, uploads), samplers and descriptors.
//
// What it reuses from the Vulkan backend: the PM4 reading and everything above DestinosNativos is the same code
// (nfsmw_nativo_sistema.cpp), and the Xbox 360 arithmetic is shared (nfsmw_nativo_xenos.h). The decisions of each
// draw (what is skipped, which shader variant, the framing) are the Vulkan backend's, ported as they are. What is
// not: there are no pipelines, render passes or descriptor sets. Each piece of state is emitted only when it changes,
// textures are bound as handles, and uploads and descriptor writes go in the slot's pre-list (Nucleo), which runs
// before the frame's draws, as the Vulkan path's upload command buffer does.
//
// Stage P2 (docs/HowToDeko3d.md): videos, menus, the title screen, races, and the exposure read-back to guest
// memory. Not yet: cubemaps made of resolved faces, the shadow minimum, the deferred sky, early depth variants,
// resolves without copies and occlusion queries. The log of this work is docs/deko3d-devlog.md.

#pragma once

#include <deko3d.h>
#include <rex/x360/shader_entry.h>
#include <rex/x360/render_hooks.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/ui/deko3d/cache_dksh.h>
#include <rex/x360/deko/geometria.h>
#include <rex/ui/deko3d/nucleo.h>
#include <rex/x360/destinos.h>
#include <rex/x360/vertices_dedupe.h>

namespace rex::memory {
class Memory;
}

// Forward-declare in the SDK namespace (rex::deko3d) and import, so these are the real SDK types, not phantoms
// local to nfsmw::deko that would shadow them (as happened with Nucleo).
namespace rex::deko3d {
class ProveedorDeko3d;
class Compilador;
}
// `deko::X` in this renderer means the SDK deko3d layer.
namespace deko = ::rex::deko3d;

namespace rex::x360 {

using ::rex::x360::EntradaShader;

// Deferred log from the renderer; the game's hooks decide where it goes (used in RenderDk methods, where hooks_ exists).
#define X360_INFORME(...) hooks_.Informe(fmt::format(__VA_ARGS__))

// An image of the renderer: a render target, a resolved texture or a game texture.
struct ImagenDk {
  DkImage imagen{};
  deko::BloqueImagen memoria;
  DkImageType tipo = DkImageType_2D;
  DkImageFormat formato = DkImageFormat_None;
  uint32_t ancho = 0, alto = 0;
  uint32_t capas = 1;  // 6 for a cubemap, the slices of a 3D texture
  uint32_t niveles = 1;
  bool profundidad = false;
  bool viva = false;
  // Resolved with copy_dest_swap: red and blue are swapped (the fetch constant's swizzle undoes it).
  bool intercambio_rb = false;
  uint64_t serie = 0;  // unique per creation: the key of its descriptors
  uint64_t version = 0;  // resolved textures: goes up every time a resolve writes it (the cubemaps' faces)
  // Render targets only, and they stay with the target when its image is swapped (IntercambiarImagenes):
  //  - contenido_invalido: a resolve without copying gave this target's image to the resolved texture at
  //    resuelta_base; if the game draws on it or resolves it again without clearing, it is brought back first.
  //  - borrado_pendiente: a depth clear asked for and not done yet. It is done right after the target is next bound
  //    for drawing, so the GPU's ZCULL data (which deko3d drops whenever another depth target is bound) starts the
  //    pass valid; or before the target is copied or swapped.
  bool contenido_invalido = false;
  uint32_t resuelta_base = 0;
  // Colour targets: where they live in the Xenos's EDRAM (tiles of 80x16 32-bit samples, a 64-bit target's 40x16
  // pixels) and their key in destinos_, for the tile ownership (TraerBaldosas).
  uint32_t edram_base = 0, edram_pitch = 0;
  bool edram_64 = false;
  uint64_t clave_edram = 0;
  // The ColorRenderTargetFormat this target was last drawn in (0xFF: never): a copy reading a 7e3 drawing as
  // 2_10_10_10 hands on its 7e3 bits (Copiar).
  uint8_t formato_dibujo = 0xFF;
  bool borrado_pendiente = false;
  float borrado_z = 1.0f;
  uint8_t borrado_estencil = 0;
  float borrado_color[4] = {};  // color targets: the last clear's value (pending or done)
  // The useful area (nfsmw_dk_borrar_area_util): targets are max(720, pitch) tall (1280x1280 for a 1280x720 scene,
  // 320x720 for a 256x256 cubemap face), and clears only cover the rows the game uses. alto_usado: the largest draw
  // scissor bottom or resolve y1 seen (0 = not known yet, the whole target is cleared). alto_borrado: the rows the last
  // clear covered; banda_sucia: the rows below hold what was there before it, and are cleared to the same value before
  // anything reaches them (CompletarBanda), so the result is exactly a whole clear.
  uint32_t alto_usado = 0;
  uint32_t alto_borrado = 0;
  bool banda_sucia = false;
  // A 4x MSAA target (sample space, 2x2 samples a pixel) bound through its real 4x companion (deko_msaa_real): the
  // pass draws there, and clears of the bound target use its rows halved.
  bool ms_ligada = false;
  // Which copy is newer: s_nueva, this sample-space image (written outside a 4x pass: packed before the next one);
  // ms_nueva, the 4x companion (drawn in a 4x pass: unpacked before anything else reads or writes this image). Only
  // when it matters, and over the rows the 4x passes drew (ms_filas, in this image's rows).
  bool s_nueva = true;
  bool ms_nueva = false;
  uint32_t ms_filas = 0;
};

// A texture the game reads from its memory, with what decides when to look at its bytes again.
struct TexturaDk {
  ImagenDk imagen;
  uint64_t huella_cruda = 0;      // XXH3 of the guest bytes (base level and mips)
  uint64_t fotograma = UINT64_MAX;  // last frame it was checked
  uint64_t siguiente = 0;           // frame of the next check
  uint32_t intervalo = 1;           // frames between checks: 1 to 32
  bool con_contenido = false;       // uploaded at least once
  // nfsmw_nativo_huellas_muestreo: the sample fingerprint (HuellaMuestra) of the content huella_cruda describes, and
  // how many rechecks in a row took the sample alone; the checks deferred in a row by the per-frame budget.
  uint64_t huella_muestra = 0;
  bool muestra_valida = false;
  uint32_t muestras_seguidas = 0;
  uint32_t aplazamientos = 0;
  uint64_t bytes = 0;
  std::array<uint64_t, 6> caras_version{};  // a cubemap of resolved faces: the version of each face it holds
  uint32_t rango_inicio = 0, rango_fin = 0;  // the guest bytes its last check read (base level)
  uint64_t serie_export = 0;                 // RenderDk::export_serie_ at its last check
};

// The vertex input of a vertex shader with the fetches D3D patched into it (the Vulkan backend's EntradaVertices,
// as deko3d state).
struct EntradaVerticesDk {
  std::array<DkVtxAttribState, 16> atributos{};  // by input location; the ones not given are fixed (0)
  uint32_t n_atributos = 0;                      // up to the last location used, plus one
  struct Enlace {
    uint32_t ranura = 0;   // vertex fetch constant (0-95)
    uint32_t zancada = 0;  // bytes
  };
  std::vector<Enlace> enlaces;
  std::array<DkVtxBufferState, 16> estado_bufers{};
  std::array<uint32_t, 16> remapeos{};  // g_InputRemap by location
  uint32_t especializacion = 0;         // bit 0: a packed 10_11_11 normal
  struct Atributo {
    uint32_t ubicacion, enlace, offset;
  };
  std::vector<Atributo> lista;  // for the diagnostics: the position's stream
};

class RenderDk final : public DestinosNativos {
 public:
  RenderDk(deko::ProveedorDeko3d& proveedor, rex::memory::Memory* memoria,
           rex::x360::RenderHooks& hooks = rex::x360::HooksPorDefecto());
  ~RenderDk() override;

  // ---- DestinosNativos (nfsmw_dk_destinos.cpp, except Dibujar)
  bool Copiar(const RegistrosCopia& registros) override;
  bool Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho, uint32_t alto) override;
  void RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) override;
  void EmpezarOclusion(uint32_t base) override { (void)base; }
  bool TerminarOclusion(uint32_t base, uint64_t& muestras) override {
    (void)base;
    muestras = 0;
    return false;  // not measured yet: the system answers the game with its "visible" value
  }
  void EstadisticasOclusion(uint64_t valores[5]) const override;
  bool Dibujar(const PeticionDibujo& peticion) override;  // nfsmw_dk_dibujos.cpp
  EstadisticasDibujos EstadisticasDeDibujos() const override;
  void EsperarSubidas() override {}  // vertices are copied on the ring thread, before Dibujar returns
  uint64_t CopiasProfundidad() const override { return copias_profundidad_; }
  uint64_t BorradosProfundidad() const override { return borrados_profundidad_; }
  void Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados, uint64_t& rechazos) const override;
  void TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const override;
  uint64_t MarcaGpuFinalNs() const override { return 0; }
  uint64_t TrabajosMarcasPrecisas() const override { return 0; }
  void EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const override;
  void DuracionTrabajosGpu(uint64_t& veces, uint64_t& nanosegundos) const override { veces = nanosegundos = 0; }
  void CosteGrabar(uint64_t coste[6]) const override;
  void TiempoGpuPorCategoria(std::array<uint64_t, kGpuCategorias>& nanosegundos) const override {
    nanosegundos = gpu_categorias_ns_;
  }
  void EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                            std::array<uint64_t, kGpuCategorias>& vertices,
                            std::array<uint64_t, kGpuCategorias>& primitivas) const override;
  void CopiasPorTamano(std::array<uint64_t, 4>& copias, std::array<uint64_t, 4>& pixeles) const override;
  void EstadisticasPorShader(std::vector<uint64_t>& fragmentos, std::vector<uint64_t>& dibujos,
                             uint64_t& fotogramas) const override;
  void IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                            double& peor_ms) const override;
  void CostePresentar(uint64_t coste[12]) const override;

 private:
  // ---- Frames and work slots (nfsmw_dk_destinos.cpp)
  // Opens a work slot if none is open: everything recorded goes into one.
  void AsegurarTrabajo();
  // Closes the open slot and submits it (the fence, then the pre-list and the commands).
  void CerrarTrabajo();
  // The upload ring of the slot is full: submit and continue in the next slot.
  bool CambiarDeRanura();
  // Everything the draw path remembers having emitted is forgotten: the next draw emits its state again. After a
  // new command list, a clear, a copy, the output pass.
  void OlvidarEstado();
  // Recorded before anything of a frame's first slot: what the previous frame's output sampled is read, and the
  // tiled cache stays off (deko3d never writes it: HowToDeko3d.md, section 6, item 7).
  void InicioDeTrabajo();
  // A Full barrier: transfers (copies, blits) are ordered against the 3D engine (HowToDeko3d.md, section 5.6).
  void BarreraTransferencia();
  // The barriers around copy engine transfers (copies, read-backs), only where there is a hazard: before a transfer
  // if 3D work was recorded since the last barrier, and after one before the next 3D work (HayTrabajo3D). The copy
  // engine runs its transfers one after another (non-pipelined, with flush), so transfers need nothing between them.
  void AntesDeTransferir();
  void AntesDe3D();
  bool trabajo_3d_sin_barrera_ = true;  // 3D work since the last Full barrier
  bool transferencia_sin_barrera_ = false;  // a transfer since the last Full barrier
  // A depth target's content changed behind the GPU's ZCULL data (a swap of images, a restore by copy): the next 3D
  // work invalidates it first (HowToDeko3d.md, section 6, item 6: deko3d only drops it when another address is bound).
  bool invalidar_zcull_ = false;
  // Copies on the 3D engine (Copiar3D): a resolve as a draw that reads the source with texelFetch and writes the
  // destination as a render target (depth through gl_FragDepth). The 3D engine is several times faster than the copy
  // engine (0.78 ms per Mpixel), keeps compressed images compressed, and orders against draws with Fragments barriers
  // instead of Full ones. rop_sin_barrera_: render target writes since the last barrier that made them visible to
  // sampling; copia3d_pendiente_: a copy since then, which the next draw or clear waits for.
  // `modo`: kCopiaValor (times `escala`, the copy's 2^exp_bias) or kCopia7e3 (each colour channel's 7e3 bits as unorm,
  // then times `escala`).
  // kCopiaDesde7e3: the reverse, 7e3 bits held as unorm back to their float value.
  static constexpr int32_t kCopiaValor = 0, kCopia7e3 = 1, kCopiaDesde7e3 = 2;
  // The EDRAM aliasing between an 8_8_8_8 image and a 7e3 one at the same base: the same 32-bit words read in the other
  // layout (bytes vs 10:10:10 7e3 fields and 2 alpha bits), not channel by channel.
  static constexpr int32_t kCopia7e3A8888 = 3, kCopia8888A7e3 = 4;
  // The Xenos has one EDRAM, and a game draws targets of different bases and pitches over the same tiles (Halo 3:
  // 1200-wide frames, 400-, 640- and 1160-wide passes, 1200x1200 clears that wrap past the end). Each base has its
  // own image here, so the last writer of every tile is kept (dueno_baldosa_): MarcarBaldosas when a target is drawn
  // on, TraerBaldosas before a target is drawn on or resolved, which copies in the tiles another target wrote since.
  void MarcarBaldosas(const ImagenDk& imagen, uint32_t y0, uint32_t y1);
  // Direct3D's fast clear: the bulk of a colour buffer is cleared by depth-only rectangles drawn through a 4x MSAA
  // depth view at the colour buffer's EDRAM base (the depth writes land in its memory); only the edges go through the
  // colour path. Here each image has its own memory, so the colour part is done directly: true if it was one.
  bool BorradoRapidoD3D(const uint32_t* r, uint32_t cuenta);
  uint64_t borrados_rapidos_ = 0;
  void TraerBaldosas(ImagenDk& imagen, uint32_t filas);
  // `muestras` (depth only): samples per pixel along each axis of a 4x MSAA source in sample space; x0/y0 stay in
  // pixels and each pixel takes its sample 0.
  bool Copiar3D(const ImagenDk& origen, uint32_t x0, uint32_t y0, uint32_t ancho, uint32_t alto, ImagenDk& destino,
                uint32_t dx, uint32_t dy, int32_t modo = kCopiaValor, float escala = 1.0f, uint32_t muestras = 1);
  bool rop_sin_barrera_ = true;
  bool copia3d_pendiente_ = false;
  uint64_t copias_3d_ = 0, barreras_fragmentos_ = 0, contenidos_llevados_ = 0;
  DkShader fs_copia_color_{}, fs_copia_profundidad_{}, fs_estencil_{}, fs_estencil_exportado_{};
  bool fs_copia_color_valido_ = false, fs_copia_profundidad_valido_ = false, fs_estencil_valido_ = false;
  bool fs_estencil_exportado_valido_ = false;  // the whole byte in one pass (gl_FragStencilRefARB)
  // Halo 3 draws its stencil (an 8x8 dither pattern, objects' fade levels test against it) as the red byte of 8_8_8_8
  // colour at the depth buffer's EDRAM base. With separate images, that red byte is carried into the depth image's
  // stencil here, a bit per pass (no stencil export): the rectangle drawn.
  void RestaurarEstencil(const ImagenDk& color, ImagenDk& profundidad, const DkScissor& rect);
  uint64_t estenciles_restaurados_ = 0;
  deko::Subida Reservar(uint32_t bytes, uint32_t alineacion);

  // ---- Render targets and resolved textures (nfsmw_dk_destinos.cpp)
  // para_copia: the image holding the base's content, whichever family (see propietario_).
  ImagenDk* DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch, bool para_copia = false);
  ImagenDk* ImagenColor(uint32_t base, uint32_t formato, uint32_t pitch);
  bool EsDestinoFlotante(uint32_t formato) const;
  ImagenDk* DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch);
  // The resolved texture at that address, if one exists and has content.
  const ImagenDk* TexturaResuelta(uint32_t direccion);
  // The one of that size when several were resolved to the address (resueltas_otras_), else as above.
  const ImagenDk* TexturaResuelta(uint32_t direccion, uint32_t ancho, uint32_t alto);
  struct Resuelta {
    ImagenDk imagen;
    uint32_t formato_guest = 0;
    bool escrita = false;
  };
  Resuelta* ObtenerResuelta(uint32_t base, uint32_t ancho, uint32_t alto, uint32_t formato, bool intercambio_rb,
                            bool profundidad);
  Resuelta* BandaDe(uint32_t& base, uint32_t ancho, uint32_t alto_destino, uint32_t pitch, bool profundidad,
                    uint32_t& filas);
  bool CrearImagen(ImagenDk& imagen, DkImageType tipo, DkImageFormat formato, uint32_t ancho, uint32_t alto,
                   uint32_t capas, uint32_t niveles, uint32_t flags, const char* que,
                   DkMsMode muestras = DkMsMode_1x);
  uint32_t VistaEstencil(const ImagenDk& imagen);  // a depth image's stencil, as a texture (usampler)
  void DestruirImagen(ImagenDk& imagen);
  // A new render target starts as the Vulkan path's: color 0, depth 1 and stencil 0.
  void PrepararDestino(ImagenDk& imagen);
  bool Rectangulo(const RegistrosCopia& reg, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1);
  void BorrarProfundidad(const RegistrosCopia& reg, uint32_t pitch);
  void CopiarProfundidad(const RegistrosCopia& reg, uint32_t pitch, bool borrara);
  // A pending depth clear (ImagenDk::borrado_pendiente) recorded now on its own: before a copy or a swap reads it.
  void HacerBorradoPendiente(ImagenDk& destino);
  // The rows of a clear: the useful area, if known.
  uint32_t AltoDeBorrado(const ImagenDk& destino) const;
  // The clear of a bound target (index `i` among the bound colors, or depth) on rows [desde, hasta).
  void BorrarFilas(ImagenDk& destino, uint32_t i, uint32_t desde, uint32_t hasta);
  // Something is about to reach row `hasta` of a target: its useful area grows, and the rows its last clear left are
  // cleared first. `ligado`: bound in the current pass at index `i` (color) or as depth.
  void CompletarBanda(ImagenDk& destino, uint32_t hasta, bool ligado, uint32_t i);
  // The bound pass's targets (EmpezarPase), for CompletarBanda in each draw.
  std::array<ImagenDk*, 5> imagenes_pase_{};
  // nfsmw_nativo_frontal_perezoso: the resolve of the scene to the front buffer is not copied. The Swap paints from the
  // render target (its clear is deferred until it is bound again, after the output pass); anything that would change
  // the target first, or a draw that samples the front buffer, makes the copy then (MaterializarFrontal).
  struct Frontal {
    bool pendiente = false;
    uint32_t base = 0;
    ImagenDk* destino = nullptr;
    Resuelta* resuelta = nullptr;
    uint32_t ancho = 0, alto = 0;
  } frontal_;
  uint32_t ultimo_frontal_ = 0;       // the front buffer of the last Swap
  uint32_t frontal_obsoleta_ = 0;     // painted from its target: the texture itself was not written
  bool frontal_apagado_ = false;      // a late read showed up: copies from then on
  uint64_t frontales_ahorrados_ = 0, frontales_copiados_ = 0, bandas_completadas_ = 0;
  void MaterializarFrontal();
  // Before a target's content changes (a pass binds it, a clear, a band): the pending front copy from it is made.
  void AntesDeCambiarDestino(const ImagenDk* destino) {
    if (frontal_.pendiente && frontal_.destino == destino) {
      MaterializarFrontal();
    }
  }
  // The images (and descriptors) of a render target and a resolved texture of the same size and format change places:
  // the resolve without a copy.
  void IntercambiarImagenes(ImagenDk& destino, ImagenDk& resuelta);
  // A render target whose content went to a resolved texture in a swap gets it back (copy), before it is drawn on or
  // resolved again; or, for the shadow map's car pass while the shadow minimum is applied, a clear instead.
  void RestaurarContenido(ImagenDk& destino, bool para_resolver);

  // ---- nfsmw_nativo_sombra_minimo (nfsmw_dk_destinos.cpp), the Vulkan backend's, ported with its guards: the shadow
  // map's car pass is drawn over a cleared target instead of the world copied back (1600x1600), and the draws that
  // sample the texture with the cars take the minimum with the world's (tfetch2DSombraMin, spec bit 23). It watches
  // kSombraMinimoCiclos clean cycles before applying and switches off for the session at the first difference.
  void SombraMinimoTrasIntercambio(const ImagenDk& destino, uint32_t base, bool borrara);
  bool SombraMinimoEnVezDeRestaurar(ImagenDk& destino);
  void SombraMinimoCopiaDesde(const ImagenDk& origen);
  void SombraMinimoAntesDeBorrar(const ImagenDk& destino);
  void SombraMinimoResueltaEscrita(uint32_t base);
  void SombraMinimoDiferencia(const char* motivo, uint32_t dato);
  void SombraMinimoNoApto(const char* motivo, uint32_t dato);
  void SombraMinimoFalloCiclo(const char* motivo, uint32_t dato, bool permanente);
  bool SombraMinimoEncendido() const;
  bool PaseDeCochesSombra(const ImagenDk* profundidad, bool solo_profundidad);
  void DibujoDeCochesSombra(bool exacto, uint32_t control_z);
  uint32_t DireccionSombraCoches() const;
  const ImagenDk* CompaneraSombraCoches(bool capaz, uint32_t ps);
  enum : uint32_t { kSmLibre = 0, kSmTrasMundo = 1, kSmCoches = 2 };
  enum : uint32_t { kSmMirando = 0, kSmAplicando = 1, kSmApagada = 2 };
  uint32_t sm_estado_ = kSmLibre, sm_fase_ = kSmMirando;
  const ImagenDk* sm_destino_ciclo_ = nullptr;
  const ImagenDk* sm_destino_ = nullptr;
  uint32_t sm_mundo_ciclo_ = 0, sm_mundo_ = 0, sm_coches_ = 0;
  bool sm_ciclo_limpio_ = false, sm_ciclo_borrado_ = false, sm_mundo_reescrito_ = false, sm_encendido_ = false;
  bool sm_virtual_ = false, sm_virtual_valida_ = false;
  bool pase_coches_sombra_ = false;
  uint64_t sm_limpios_seguidos_ = 0, sm_ciclos_ = 0, sm_ciclos_aplicados_ = 0, sm_diferencias_ = 0;
  uint64_t sm_dibujos_coches_ = 0, sm_dibujos_no_exactos_ = 0, sm_lecturas_minimo_ = 0, sm_lecturas_si_misma_ = 0;
  uint64_t sm_lecturas_normales_ = 0, sm_lecturas_a_destiempo_ = 0, sm_lecturas_incapaces_ = 0;
  uint64_t intercambios_ = 0, restauraciones_ = 0, borrados_aplazados_ = 0, cubos_refrescados_ = 0;
  uint64_t informe_intercambios_ = 0, informe_restauraciones_ = 0, informe_cubos_ = 0, informe_cielos_ = 0;
  bool Rechazar(uint32_t causa, const char* texto);
  void Avisar(uint32_t causa, const char* texto);

  // ---- Read-back of small resolved textures to guest memory (nfsmw_dk_destinos.cpp). The game measures the
  // scene's brightness for its automatic exposure by reading the 64x64 resolves on the CPU; without them it reads
  // stale memory, takes the scene for very dark and opens the exposure all the way (washed-out image, white sky).
  // The rectangle is copied to a CPU-cached block in the same work and written to guest memory, tiled and in the
  // guest's byte order, once that work's fence has passed: the Vulkan path's LeerResuelta and EscribirLecturas.
  void LeerResuelta(const RegistrosCopia& reg, const ImagenDk& imagen, int32_t x0, int32_t y0, uint32_t dx,
                    uint32_t dy, uint32_t ancho, uint32_t alto);
  // The read-backs of work slot `ranura`, whose fence has passed.
  void EscribirLecturas(uint32_t ranura);
  // Those of the submitted works whose fence has passed, without waiting (once per frame).
  void EscribirLecturasTerminadas();
  // GPU time by category, as the Vulkan path's C2 report: a timestamp each time the category changes (a pass with
  // other targets, a copy, a clear), kMarcasPorRanura at most per work, in the core's report block. `categoria` is
  // one of kGpu* (nfsmw_nativo_peticion.h) or kGpuFin, the end of the work.
  void MarcarGpu(uint32_t categoria);
  // The marks of the work last recorded in slot `ranura`, once its fence has passed: time per category, the whole
  // work, and the GPU idle gap since the previous work.
  void LeerMarcasGpu(uint32_t ranura);
  struct LecturaDk {
    uint32_t offset = 0;  // in the slot's part of lectura_bloque_
    uint32_t base = 0;    // RB_COPY_DEST_BASE
    int32_t x0 = 0, y0 = 0;  // texel of the texture where the rectangle starts
    uint32_t ancho = 0, alto = 0;
    uint32_t pitch = 0, alto_destino = 0;  // RB_COPY_DEST_PITCH
    uint32_t info = 0;                     // RB_COPY_DEST_INFO
    uint32_t bytes_texel = 4;              // 4: RGBA8; 8: RGBA16F (16_16_16_16_FLOAT, Halo 3's exposure)
  };
  struct LecturasRanura {
    std::vector<LecturaDk> pendientes;
    uint64_t trabajo = 0;  // the work they were recorded in
    uint32_t usados = 0;   // bytes of the slot's part of lectura_bloque_
  };

  // ---- Output (nfsmw_dk_destinos.cpp)
  bool CrearShadersSalida();
  const DkShader* ShaderRampa(uint32_t variante);
  bool PintarSalida(DkImage& salida, uint32_t ancho_salida, uint32_t alto_salida, const ImagenDk& origen, uint32_t ancho,
                    uint32_t alto);
  void RecalcularRampa();
  void GuardarCacheSiHaceFalta(bool ya);
  void InformePeriodico();

  // ---- The deferred sky (nfsmw_nativo_cielo_aplazado, nfsmw_dk_dibujos.cpp): the scene's sky dome, painted first on
  // an empty depth buffer and then mostly covered, is recorded after the opaque draws of its pass instead, where the
  // depth test discards what the world covers. The same image (the Vulkan path explains why), with the same guard: 90
  // frames with exactly one dome before deferring, and off for the session if two show up in one frame.
  void EmitirCieloPendiente();
  void CerrarFotogramaDeLaGuardiaDelCielo();

  // ---- Draws (nfsmw_dk_dibujos.cpp)
  const EntradaVerticesDk* EntradaDe(const PeticionDibujo& p);
  uint64_t ClaveEntrada(const PeticionDibujo& p) const;
  bool CalcularEntrada(const PeticionDibujo& p, EntradaVerticesDk& entrada);
  uint32_t CalcularEncuadre(const uint32_t* r, uint32_t vte, bool sin_recorte, uint32_t modo_sc, float viewport[6],
                            float ndc[4], DkScissor& tijera);
  bool EmpezarPase(const uint64_t claves[5], uint32_t pitch);
  // The DKSH of a library shader for that specialization value (masked) and, for a vertex shader, the
  // interpolators it keeps. nullptr if it cannot be had (logged once).
  // `variante_ps` (pixel shaders): kVariante* flags (nfsmw_dk_dibujos.cpp), part of the DKSH cache key; 0 is the
  // library's shader as is.
  const DkShader* ShaderDe(const EntradaShader& entrada, uint32_t especializacion, uint32_t salidas,
                           uint32_t variante_ps = 0);
  struct InfoShader {
    uint64_t huella = 0;      // HuellaSpirv
    uint32_t mascara = ~0u;    // MascaraEspecializacion
    uint32_t interpoladores = 0;  // pixel shader: the locations it reads
    bool interpoladores_listos = false;
    uint32_t ubos = 0;                 // bit N: reads the UBO at binding N (3 = Halo 3's booleans and loops)
    uint32_t buffers = 0;              // bit N: reads storage buffer N (a vertex fetch with no element)
    std::array<uint8_t, 16> slots_vf{};  // the vertex fetch slot of each storage buffer
    // What each buffer reads per vertex (GlslDeko's zancada_vf / minimo_vf / maximo_vf); stride 0 = the whole buffer.
    std::array<uint32_t, 16> zancada_vf{};
    std::array<int32_t, 16> minimo_vf{}, maximo_vf{};
    bool exporta = false;  // a vertex shader that writes memory (memory export)
  };
  InfoShader& InfoDe(const EntradaShader& entrada);

  // ---- Memory export (Halo 3's particles, light visibility tests, water). A draw whose vertex shader exports gets a
  // region of its work slot's record buffer (binding 15, the shader's xeno_memexport): (guest byte address, dword) pairs.
  // When the GPU has done that work the records are written to guest memory, those inside the streams found in the
  // draw's constants only (the shader's checks aside, nothing else of guest memory can be hit).
  static constexpr uint32_t kDwordsExportRanura = 1u << 20;  // 4 MB a slot
  static constexpr uint32_t kFlujosExport = 8;
  struct RegionExport {
    uint32_t dword = 0, capacidad = 0;
    uint32_t n_flujos = 0;
    std::array<std::pair<uint32_t, uint32_t>, kFlujosExport> flujos{};  // guest byte address, bytes
  };
  struct ExportRanura {
    std::vector<RegionExport> regiones;
    uint32_t usados = 0;     // dwords
    uint64_t trabajo = 0;    // the work that wrote them
    uint64_t fotograma = 0;  // the frame that work belongs to
    uint32_t minimo = UINT32_MAX, maximo = 0;  // the guest bytes all its regions' streams span (a quick no)
  };
  // A draw about to read guest memory that an earlier frame's exports are still writing (not applied yet): waits for
  // that work and applies them, oldest first (the H3Recomp PC port's SyncMemExportsFor). This frame's own exports are
  // not waited for: every particle update writes the same streams, and waiting on them would stall each one.
  void SincronizarExportaciones(uint32_t inicio, uint32_t fin);
  // ---- Real 4x MSAA (deko_msaa_real). A 4x surface's target lives in sample space (2x2 samples a pixel, the image
  // at twice the pitch: what the EDRAM aliasing, the resolves and the tiles work on). Drawn there, every pixel shader
  // ran once a sample; the Xenos shades once a pixel. A 4x pass now draws into a multisampled companion at pixel size
  // (samples at the same 2x2 positions, so coverage and depth are as before): on entering, each target is cleared
  // there (a pending clear) or packed from sample space; where the pass ends (TerminarPase: a copy, a clear, a
  // restore, another pass), the companions are unpacked back.
  ImagenDk* CompaneraMs(const ImagenDk& muestras);
  void EmpaquetarMs(ImagenDk& muestras, ImagenDk& ms, uint32_t filas);
  void DesempaquetarMs(ImagenDk& muestras, ImagenDk& ms, uint32_t filas);
  void CerrarPaseMs();
  void LeerMuestras(ImagenDk& imagen);     // before reading it outside a 4x pass
  void EscribirMuestras(ImagenDk& imagen);  // before writing it outside a 4x pass (a partial write keeps the rest)
  // Before a write that covers what the 4x pass drew (ancho x filas from the top left): the companion's content is
  // dropped, not unpacked. false: not covered, call EscribirMuestras.
  bool DescartarMuestras(ImagenDk& imagen, uint32_t ancho, uint32_t filas);
  void TerminarPase() {
    EmitirCieloPendiente();  // the deferred sky draws in the pass, before its targets go back to sample space
    CerrarPaseMs();
  }
  void DibujarRectanguloMs(bool destino_ms, bool profundidad, const DkShader& fs, uint32_t ranura, uint32_t ranura2,
                           uint32_t ancho, uint32_t filas);
  std::unordered_map<uint64_t, ImagenDk> ms_de_;  // by the sample-space image's serie
  bool ms_activo_ = false;
  DkShader fs_ms_color_{}, fs_ms_z_{}, fs_desms_color_{}, fs_desms_z_{};
  bool fs_ms_validos_ = false;
  uint64_t ms_empaquetados_ = 0, ms_desempaquetados_ = 0, ms_pases_ = 0, ms_descartados_ = 0;
  uint64_t ms_resueltas_directas_ = 0, ms_borrados_directos_ = 0;  // read or cleared on the companion, no unpack
  DkShader fs_copia_color_ms_{};  // kFsCopiaColor from a 4x companion
  bool fs_copia_color_ms_valido_ = false;
  void ReservarExportacion(const uint32_t* r, uint32_t vertices, uint32_t& dword, uint32_t& capacidad);
  void AplicarExportaciones(uint32_t ranura, bool en_medio);  // en_medio: inside a work (the geometry store forgets)
  DkMemBlock export_bloque_ = nullptr;
  uint32_t* export_cpu_ = nullptr;
  DkGpuAddr export_gpu_ = 0;
  std::array<ExportRanura, 3> export_ranuras_{};
  // The guest ranges memory export wrote, and the frame: a texture over one is checked whole at its next use, whatever
  // its interval or sample said (Halo 3 reads exported results as textures: exposure, light visibility).
  struct RangoExportado {
    uint32_t inicio, fin;
    uint64_t fotograma, serie;
  };
  std::vector<RangoExportado> exportados_;
  // Read-backs of small resolves (LeerResuelta): copied on the 3D engine into an image of their own when they happen,
  // and to the read-back buffer all together when the work closes, in one copy engine phase: one transfer barrier a
  // work instead of two per read-back (Halo 3 reads about 8 a frame back: 16 full GPU drains).
  struct LecturaAplazada {
    ImagenDk imagen;
    DkCopyBuf destino;
    uint32_t ancho, alto;
  };
  std::vector<LecturaAplazada> lecturas_aplazadas_;
  uint64_t export_serie_ = 0;  // goes up each time records are written
  uint64_t export_dibujos_ = 0, export_registros_ = 0, export_escritos_ = 0, export_fuera_ = 0, export_llenos_ = 0;
  uint64_t export_esperas_ = 0, envios_parciales_ = 0;
  uint64_t informe_export_dibujos_ = 0, informe_export_escritos_ = 0, informe_export_fuera_ = 0;
  void RecogerCompilaciones();

  // ---- Textures, samplers and descriptors (nfsmw_dk_texturas.cpp)
  struct FormatoDk {
    DkImageFormat formato = DkImageFormat_None;
    uint8_t bloque = 1;        // texels per block side
    uint8_t bytes = 1;         // bytes per block
    uint8_t unidad_orden = 0;  // 0 = no byte swap, else 2 or 4 bytes
    uint16_t swizzle_host = 0;
    bool ctx1 = false;  // k_CTX1: guest blocks of 4x4 (8 bytes), decoded on the CPU to the host's RG8 texels
    bool rgba4 = false;  // k_4_4_4_4: guest 16-bit texels, expanded on the CPU to the host's RGBA8
    bool dxt3a = false;  // k_DXT3A: guest blocks of 4x4 4-bit alphas (8 bytes), decoded on the CPU to the host's R8
  };
  static bool FormatoDe(uint32_t formato, FormatoDk& f);
  // The descriptor slot of the texture a fetch constant names (0 = the empty one of that type), with what the shader
  // needs: the host size (1/size) and whether it is sampled without filtering (depth).
  // `valido_hasta`: the last frame the answer holds without asking again (a game texture until its next fingerprint
  // check; resolved textures and cubemaps, this frame only).
  uint32_t PrepararTextura(const uint32_t* f, uint16_t tipo_shader, uint32_t& ancho, uint32_t& alto, bool& puntual,
                           uint64_t& valido_hasta);
  // `gamma`: read through the format's sRGB view (a fetch whose RGB signs are gamma), when it has one.
  uint32_t VistaDe(const ImagenDk& imagen, uint32_t swizzle_guest, uint16_t swizzle_host, bool gamma = false);
  uint32_t SamplerDe(const uint32_t* f, bool puntual);
  uint32_t VaciaDe(uint16_t tipo_shader) const;
  bool LeerNivel(uint64_t direccion, bool mosaico, uint32_t pitch_bloques, uint64_t fila_bytes, const FormatoDk& tf,
                 uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host, uint8_t* destino);
  bool LeerVolumen(const uint32_t* f, const FormatoDk& tf, uint32_t bloques_x, uint32_t bloques_y, uint32_t fondo,
                   uint32_t bloques_x_host, uint32_t bloques_y_host, uint32_t pitch_bloques, bool mosaico,
                   uint8_t* datos);
  bool CrearVacias();
  uint32_t ReservarDescriptorImagen();
  void LiberarDescriptoresDe(uint64_t serie);
  void EscribirDescriptorImagen(uint32_t ranura, const DkImageView& vista);
  void SoltarTexturasSiHaceFalta();

  deko::ProveedorDeko3d& proveedor_;
  deko::Compilador& compilador_;
  rex::memory::Memory* memoria_;
  rex::x360::RenderHooks& hooks_;  // the game's effects and image settings (default: none)
  deko::Nucleo nucleo_;  // the ring thread's

  // Work slots and frames
  uint64_t trabajo_ = 0;
  uint64_t fotograma_ = 1;
  // g_volcar_dibujos_captura: the capture whose next frame is being listed, and its last draws.
  int32_t volcando_captura_ = 0;
  std::deque<std::string> volcado_dibujos_;
  uint64_t serie_imagenes_ = 0;

  // Render targets, resolved textures
  std::unordered_map<uint64_t, ImagenDk> destinos_;
  // With deko_destinos_flotantes, a 32-bit EDRAM base has two images (8-bit and float): the key of the one holding its
  // latest content, by (base << 20 | pitch). A draw in the other family carries the content over first (LlevarContenido),
  // as the EDRAM's bits would be read in the new format; a copy reads the owner.
  std::unordered_map<uint64_t, uint64_t> propietario_;
  void LlevarContenido(ImagenDk& origen, ImagenDk& destino, uint32_t formato_destino);
  std::unordered_map<uint64_t, ImagenDk> profundidades_;
  std::unordered_map<uint32_t, Resuelta> resueltas_;
  uint64_t bandas_resueltas_ = 0;  // resolves of a predicated-tiling band into a texture's lower rows
  // Resolved textures displaced from resueltas_ by another size at the same address, by ClaveOtra (base and size):
  // Halo 3 resolves the 1152x640 scene and a 288x160 downscale to the same address every frame, and both are
  // sampled. Swapped back into resueltas_ when that size is resolved again, so neither is recreated every frame.
  std::unordered_map<uint64_t, Resuelta> resueltas_otras_;
  static uint64_t ClaveOtra(uint32_t base, uint32_t ancho, uint32_t alto, bool profundidad) {
    return uint64_t(base) | uint64_t(ancho & 0x3FFF) << 32 | uint64_t(alto & 0x3FFF) << 46 | uint64_t(profundidad) << 60;
  }
  // Read-backs: kBytesLecturaRanura per work slot in one CPU-cached, GPU-uncached block
  std::vector<LecturasRanura> lecturas_;
  DkMemBlock lectura_bloque_ = nullptr;
  uint8_t* lectura_cpu_ = nullptr;
  DkGpuAddr lectura_gpu_ = 0;
  uint64_t lecturas_hechas_ = 0, lecturas_escritas_ = 0, lecturas_sin_sitio_ = 0;
  uint64_t siguiente_aviso_exposicion_ = 0;
  uint32_t escribir_vista_exposicion_ = 0;  // [vistas] diagnostic: the address to compare through the CPU's views
  std::array<uint64_t, 2048> dueno_baldosa_{};  // clave_edram of each EDRAM tile's last writer (0: none)
  uint64_t pase_numero_ = 0;  // every EmpezarPase (g_.pase_serie restarts with OlvidarEstado)
  uint64_t marcado_serie_ = ~uint64_t(0);
  uint32_t marcado_desde_ = 0, marcado_hasta_ = 0;
  uint64_t baldosas_traidas_ = 0;  // tile rows copied in by TraerBaldosas, in total
  // deko_volcar_pasos: on two capture frames, every resolve shrunk to 288x160 and written as capturas/paso_*.ppm.
  struct PasoVolcado {
    ImagenDk imagen;
    uint64_t trabajo = 0;
    uint32_t offset = 0, base = 0, ancho = 0, alto = 0, numero = 0;
    int32_t captura = 0;
  };
  std::vector<PasoVolcado> pasos_;
  DkMemBlock bloque_pasos_ = nullptr;
  uint32_t pasos_usados_ = 0, pasos_numero_ = 0;
  int32_t pasos_captura_ = 0, pasos_menu_ = 0, pasos_juego_ = 0;  // the capture being dumped; menu and game ones done
  uint64_t dibujados_al_swap_ = 0, dibujos_ultimo_fotograma_ = 0;
  void VolcarPaso(const ImagenDk& origen, uint32_t base);
  void EscribirPasos();  // [exposicion] diagnostic: the frame of its next line
  // GPU time: timestamps at the start and the end of each work's command list (the pre-list's uploads before it are
  // not counted). Totals since start, as TiempoGpu gives them.
  std::vector<std::vector<uint8_t>> marcas_;  // per slot: the category starting at each timestamp
  uint64_t gpu_ns_ = 0, gpu_trabajos_ = 0;
  std::array<uint64_t, kGpuCategorias> gpu_categorias_ns_{};
  // The clears by kind (kGpuSub*), inside gpu_categorias_ns_[kGpuBorrados]: at a pass's start, D3D fast clears,
  // stencil restores, new targets, pending clears and bands.
  static constexpr uint32_t kGpuSubBorradoPase = 32, kGpuSubBorradoRapido = 33, kGpuSubEstencil = 34,
                            kGpuSubPreparar = 35, kGpuSubPendiente = 36, kGpuSubBanda = 37, kGpuSubBorrados = 6;
  std::array<uint64_t, kGpuSubBorrados> gpu_sub_borrados_ns_{}, informe_sub_borrados_{};
  uint64_t marca_fin_previa_ = 0;
  std::array<uint64_t, 4> fin_fotogramas_{};  // the last work of each recent frame (deko_fotogramas_en_vuelo)  // raw end timestamp of the last work read
  // nfsmw_nativo_saltar_borrados_repetidos: per image (its serie), the last clear value while nothing has been
  // drawn since (the global draw count then).
  struct EstadoBorrado {
    uint64_t valor = 0;
    uint64_t dibujos = UINT64_MAX;
  };
  std::unordered_map<uint64_t, EstadoBorrado> borrado_previo_;
  uint64_t borrados_saltados_ = 0;
  // Pixel shader variants in use (report): early depth, glow natural or soft.
  uint64_t dibujos_z_temprana_ = 0, dibujos_resplandor_ = 0;

  // Textures, samplers, descriptors. Image descriptors: 0-2 the empty 2D, 3D and cube textures, then on demand.
  // A freed slot waits for the fence of the slot it was freed in.
  std::unordered_map<uint64_t, TexturaDk> texturas_;
  uint64_t bytes_texturas_ = 0;
  // Texture checks (the Vulkan path's C3): the sampled recheck of stable textures (nfsmw_nativo_huellas_muestreo,
  // its guard: the first kMuestrasAComprobar stable rechecks compute both hashes) and the per-frame budget
  // (nfsmw_nativo_huellas_kb_fotograma). Counters are totals.
  static constexpr uint64_t kMuestrasAComprobar = 3000;
  static constexpr uint32_t kAplazamientoMax = 8;
  // Guest addresses whose sample missed a change the whole hash saw: always rechecked whole (at most this many,
  // then sampling is off for all).
  static constexpr size_t kBasesSinMuestraMax = 4;
  std::unordered_set<uint32_t> bases_sin_muestra_;
  int32_t muestreo_huellas_ = -1;  // -1: not read yet; 0: off; N: 1 in N stable rechecks is whole
  uint64_t presupuesto_huellas_ = 0;  // bytes per frame, 0 = no limit
  uint64_t fotograma_huellas_ = 0, bytes_huella_fotograma_ = 0;
  uint64_t muestras_comprobadas_ = 0, muestras_aciertos_ = 0, muestras_distintas_ = 0;
  uint64_t bytes_huella_ = 0, bytes_ahorrados_muestra_ = 0, huellas_aplazadas_ = 0;
  uint64_t informe_bytes_huella_ = 0, informe_bytes_ahorrados_muestra_ = 0, informe_muestras_aciertos_ = 0;
  uint64_t informe_huellas_aplazadas_ = 0;
  std::unordered_map<uint64_t, uint32_t> vistas_;                  // (serie, swizzles) -> descriptor
  std::unordered_map<uint64_t, std::vector<uint64_t>> vistas_por_serie_;
  std::vector<uint32_t> descriptores_libres_;
  std::vector<std::vector<uint32_t>> descriptores_por_liberar_;  // per work slot
  uint32_t siguiente_descriptor_ = 8;
  std::unordered_map<uint32_t, uint32_t> samplers_;  // key -> descriptor
  uint32_t siguiente_sampler_ = 1;
  std::array<ImagenDk, 3> vacias_;
  std::vector<uint8_t> temporal_;  // untiled texture levels before they go to the upload ring
  // Per fetch constant, within one frame: the slot, the size and whether it is a point sample.
  struct CacheFetch {
    std::array<uint32_t, 6> fetch{};
    uint64_t fotograma = UINT64_MAX;
    uint64_t valido_hasta = 0;
    uint64_t generacion = 0;
    uint32_t ranura = 0, sampler = 0, ancho = 0, alto = 0;
  };
  std::array<CacheFetch, 512> cache_fetch_{};
  uint64_t generacion_texturas_ = 1;  // goes up when a resolved texture appears or changes size

  // Shaders
  std::unordered_map<const EntradaShader*, InfoShader> info_shaders_;
  struct ClaveCargado {
    const EntradaShader* entrada;
    uint32_t especializacion;
    uint32_t salidas;
    bool operator==(const ClaveCargado&) const = default;
  };
  struct HashCargado {
    size_t operator()(const ClaveCargado& c) const {
      return std::hash<const void*>()(c.entrada) ^ (size_t(c.especializacion) * 0x9E3779B97F4A7C15ull) ^
             (size_t(c.salidas) << 17);
    }
  };
  std::unordered_map<ClaveCargado, std::unique_ptr<DkShader>, HashCargado> cargados_;  // nullptr: failed
  deko::CacheDksh cache_;
  bool cache_sucia_ = false;
  std::thread guardando_;  // writes the cache file (GuardarCacheSiHaceFalta)
  uint64_t cache_guardada_ns_ = 0;
  uint32_t compilados_en_el_acto_ = 0, compilados_en_fondo_ = 0, aciertos_cache_ = 0;
  // Background compiles (the exact vertex shader variant, while the one with every interpolator draws). The
  // worker's callback outlives this object if it has to, so its mailbox is shared.
  struct Bandeja {
    std::mutex candado;
    std::vector<std::pair<deko::CacheDksh::Clave, std::vector<uint8_t>>> hechas;
    uint32_t fallidas = 0;
  };
  std::shared_ptr<Bandeja> bandeja_ = std::make_shared<Bandeja>();
  std::unordered_map<uint64_t, bool> en_fondo_;  // keys queued (by a hash of the key)
  DkShader fs_vacio_{};
  bool fs_vacio_valido_ = false;
  // Rectangle lists (primitive 8): each triangle of three corners becomes a strip of four (destinos.cpp).
  DkShader gs_rectangulos_{};
  bool gs_rectangulos_valido_ = false;

  // Vertex input (the same VS with the same patched fetches gives the same input)
  std::unordered_map<uint64_t, std::unique_ptr<EntradaVerticesDk>> entradas_;
  const EntradaShader* entrada_vs_ = nullptr;
  uint64_t entrada_generacion_ = UINT64_MAX;
  const EntradaVerticesDk* entrada_actual_ = nullptr;

  // What the draw path has emitted in the current command list
  struct Grabado {
    bool valido = false;
    uint64_t pase[5] = {};
    bool pase_valido = false;
    uint32_t pase_ancho = 0, pase_alto = 0;
    uint64_t pase_serie = 0;  // goes up at every pass change (the framing cache depends on it)
    const DkShader* vs = nullptr;
    const DkShader* ps = nullptr;
    const DkShader* gs = nullptr;  // gs_rectangulos_ or none
    DkRasterizerState raster{};
    DkColorState color{};
    DkColorWriteState mascaras{};
    DkDepthStencilState profundidad{};
    std::array<DkBlendState, 4> mezclas{};
    float viewport[6] = {};
    bool volteado = false;
    DkScissor tijera{};
    float mezcla_constante[4] = {};
    float sesgo[2] = {};
    uint32_t stencil[2] = {};
    bool reinicio = false;
    const EntradaVerticesDk* entrada = nullptr;
    std::array<DkResHandle, 32> texturas{};
    uint32_t n_texturas = 0;
    DkGpuAddr ubo[3] = {};
  } g_;
  // The deferred sky: its whole state as the draw left it in g_, and what g_ does not hold.
  struct CieloDk {
    Grabado estado;
    std::array<DkBufExtents, 16> bufers{};
    uint32_t n_bufers = 0;
    bool con_indices = false, indices_de_16 = false;
    DkGpuAddr indices = 0;
    DkPrimitive primitiva = DkPrimitive_Triangles;
    uint32_t n_indices = 0, vmin = 0, cuenta = 0;
  };
  CieloDk cielo_{};
  bool cielo_pendiente_ = false;
  uint32_t dibujos_en_pase_ = 0;
  uint32_t categoria_pase_ = kGpuOtros;  // the bound pass's GPU category, again after a transfer in the middle of it
  uint64_t claves_pase_previo_[5] = {};
  enum : uint32_t { kCieloMirando = 0, kCieloAplazando = 1, kCieloDescartado = 2 };
  uint32_t cielo_guardia_ = kCieloMirando;
  uint32_t cielo_guardia_en_fotograma_ = 0, cielo_guardia_max_ = 0;
  uint64_t cielo_guardia_fotogramas_ = 0, cielo_guardia_con_uno_ = 0;
  uint64_t cielo_aplazados_ = 0, cielo_emitidos_ = 0;
  // Vertex uploads already made in this work slot (the Vulkan path's DedupeVertices: exact within a work, forgotten at
  // every guest wait for the GPU, g_sincronizaciones_anillo).
  DedupeVertices dedupe_;
  uint32_t dedupe_sinc_vista_ = 0;
  // nfsmw_dk_perfil_gpu_s: one frame every N seconds gets a GPU timestamp right before and right after each draw
  // (that frame is slower: the GPU finishes each draw before the next), so each pass and each shader gets its own
  // time; the report names the shaders by their library fingerprint, with uam's registers and code size.
  struct DatosShaderDk {
    uint64_t huella = 0;
    uint32_t numero = 0, espec = 0, variante = 0;
    bool vertices = false;
    uint32_t registros = 0, bytes_codigo = 0, scratch = 0;
  };
  std::unordered_map<const DkShader*, DatosShaderDk> datos_shaders_;
  std::unordered_set<const DkShader*> constantes_registradas_;  // pixel shaders whose constants the variants logged
  struct MarcaPerfil {
    const DkShader* vs;
    const DkShader* ps;
    uint64_t pase;  // g_.pase_serie
    uint32_t categoria, ancho, alto;
    uint64_t color, profundidad;  // the pass keys of target 0 and depth
    uint32_t vertices;            // indices or vertices drawn
    // For the most expensive single draws: the viewport and scissor sizes, the color write masks, RB_DEPTHCONTROL,
    // the first texture (size, format, sampler key) and how many textures.
    float vp_ancho, vp_alto;
    uint32_t tijera_ancho, tijera_alto, mascaras, profundidad_control;
    uint32_t tex_ancho, tex_alto, tex_formato, tex_sampler, texturas;
    // 0: the draw. Otherwise an extra copy of the draw before it (the repeat test): 1 identical, 2 with sampler 0,
    // 3 with the empty 1x1 textures, 4 writing only target 0 (nfsmw_dk_perfil_variantes).
    uint32_t repeticion;
  };
  static constexpr uint32_t kMarcasPerfil = 4096;
  std::vector<MarcaPerfil> perfil_marcas_;
  DkMemBlock perfil_bloque_ = nullptr;
  uint8_t* perfil_cpu_ = nullptr;
  DkGpuAddr perfil_gpu_ = 0;
  bool perfilando_ = false, perfil_pendiente_ = false;
  uint64_t perfil_fotograma_ = 0, perfil_trabajo_ = 0, perfil_ns_ = 0;
  void PerfilAlCambiarDeFotograma();
  void InformeDelPerfil();
  // The previous draw's textures (Dibujar reuses them when the pixel shader and its samplers' fetch words match).
  struct TexturasPrevias {
    const EntradaShader* ps = nullptr;
    uint64_t trabajo = UINT64_MAX, generacion = 0, valido_hasta = 0;
    bool toca_coches = false;
    uint32_t dir_coches = 0;
    std::array<uint32_t, 192> fetch{};  // the 32 texture fetch constants
    std::array<DkResHandle, 32> texturas{};
    uint32_t n_texturas = 0;
    std::array<uint32_t, 32> inversos{};
    std::array<uint32_t, 32> signos{};  // the texture slots' sign words (kPalabraSignos)
    std::array<uint32_t, 4> tex0{};
  } texturas_previas_;
  uint64_t texturas_reusadas_ = 0, informe_texturas_reusadas_ = 0;
  // Vertex and index ranges kept across frames (nfsmw_dk_geometria.h, nfsmw_dk_geometria_mb).
  deko::AlmacenGeometria geometria_;
  deko::AlmacenGeometria::Estadisticas informe_geometria_{};
  // Ring thread time per draw by stage (ns, totals): preparation (indices, vertex ranges), textures, state, uploads and
  // the draw.
  std::array<uint64_t, 6> ns_etapas_{};  // preparing, textures, state, vertices, indices, constants and the draw
  std::array<uint64_t, 6> informe_etapas_{};
  uint64_t informe_dedupe_bytes_ = 0;
  uint64_t informe_copias_3d_ = 0, informe_barreras_fragmentos_ = 0, informe_frontales_ = 0, informe_bandas_ = 0;
  // Framing cache (generacion_encuadre and the pass)
  uint64_t encuadre_generacion_ = UINT64_MAX;
  uint64_t encuadre_pase_serie_ = UINT64_MAX;
  float encuadre_viewport_[6] = {};
  float encuadre_ndc_[4] = {};
  DkScissor encuadre_tijera_{};
  uint32_t encuadre_vacio_ = 0;
  // Constants: the last upload of each block, valid within one slot
  uint64_t constantes_vs_generacion_ = UINT64_MAX, constantes_ps_generacion_ = UINT64_MAX;
  uint32_t constantes_vs_bytes_ = 0, constantes_ps_bytes_ = 0;
  DkGpuAddr constantes_vs_gpu_ = 0, constantes_ps_gpu_ = 0;
  uint64_t constantes_trabajo_ = UINT64_MAX;
  // Constants by differences (nfsmw_dk_constantes_por_diferencias): one block per stage and work slot, written whole
  // once when the work starts and then kept in step with the game's registers by pushing only the 16-byte registers
  // that changed (dkCmdBufPushConstants: inline in the command list, in order with the draws). The game changes a
  // few registers per draw (a world matrix) and the copy of whole blocks into uncached memory was most of the ring
  // thread's "uploads" stage. The comparison is exact (against what was pushed); the generations only decide
  // whether to compare. The deferred sky keeps whole copies: later pushes would change what it reads.
  uint64_t bloques_trabajo_ = UINT64_MAX;
  DkGpuAddr bloque_vs_ = 0, bloque_ps_ = 0, bloque_compartidas_ = 0;
  std::array<uint32_t, 1024> sombra_vs_{};  // 256 registers
  std::array<uint32_t, 1024> sombra_ps_{};  // 256 registers
  std::array<uint32_t, 192> sombra_compartidas_{};
  uint64_t bloque_vs_generacion_ = UINT64_MAX, bloque_ps_generacion_ = UINT64_MAX;
  uint32_t bloque_vs_cubierto_ = 0, bloque_ps_cubierto_ = 0;  // bytes compared at that generation
  uint64_t registros_empujados_ = 0, empujes_ = 0, informe_registros_empujados_ = 0, informe_empujes_ = 0;
  // 32 float4: the deko3d library declares 31 (NFS), the translator's GLSL 32 (bytes 496-508: Halo 3's
  // render target exponent bias factors).
  static constexpr uint32_t kPalabrasCompartidas = 192;
  uint32_t compartidas_previas_[kPalabrasCompartidas] = {};
  DkGpuAddr compartidas_gpu_ = 0;
  uint64_t compartidas_trabajo_ = UINT64_MAX;
  float pase_escala_ = 1.0f;

  // Output: the guest output pass and the game's gamma ramp
  DkShader vs_salida_{};
  bool vs_salida_valido_ = false;
  std::array<std::unique_ptr<DkShader>, 8> rampas_{};  // by variant: exact, grading, FXAA
  std::array<bool, 8> rampas_intentadas_{};
  std::array<std::array<uint16_t, 3>, 256> rampa_juego_{};
  std::array<float, 256 * 4 + 8> rampa_valores_{};
  ImagenDk rampa_tabla_;     // the ramp as a 256x1 RGBA32F texture (kFsRampa)
  bool rampa_subir_ = true;   // rampa_valores_ changed: upload it before the next output pass
  uint64_t version_posproceso_ = UINT64_MAX;
  bool graduacion_ = false;
  bool rampa_gamma_ = true;
  bool fxaa_anotado_ = false;
  uint64_t serie_salida_ = UINT64_MAX;  // the guest output image's version, for its descriptor
  uint32_t ranura_salida_ = 3;            // image descriptor 3: the output pass's source

  // Statistics
  uint64_t copias_ = 0, borrados_ = 0, presentados_ = 0, rechazados_ = 0, dibujados_ = 0;
  uint64_t copias_profundidad_ = 0, borrados_profundidad_ = 0;
  uint64_t texturas_creadas_ = 0, texturas_subidas_ = 0, bytes_subidos_ = 0;
  uint64_t cambios_ranura_ = 0, barreras_ = 0;
  std::array<uint64_t, kCubetasSwap> cubetas_swap_{};
  double peor_swap_ms_ = 0;
  uint64_t ultimo_swap_ns_ = 0;
  uint64_t ns_grabando_ = 0;
  std::unordered_map<uint32_t, uint64_t> causas_;
  std::unordered_map<uint32_t, bool> avisados_;
  uint64_t informe_ns_ = 0;
  uint64_t informe_dibujos_ = 0, informe_fotogramas_ = 0;
  // The counters at the previous report: the report gives what happened in its interval
  uint64_t informe_copias_ = 0, informe_borrados_ = 0, informe_barreras_ = 0, informe_creadas_ = 0;
  uint64_t informe_subidas_ = 0, informe_bytes_subidos_ = 0, informe_ns_grabando_ = 0, informe_lecturas_ = 0;
  uint64_t informe_gpu_ns_ = 0, informe_gpu_trabajos_ = 0, informe_espera_ns_ = 0, informe_saltados_ = 0;
  uint64_t informe_z_temprana_ = 0;
  std::array<uint64_t, kGpuCategorias> informe_categorias_{};
};

std::unique_ptr<DestinosNativos> CrearDestinosDeko3d(deko::ProveedorDeko3d* proveedor, rex::memory::Memory* memoria,
                                                    rex::x360::RenderHooks& hooks);  // default in nfsmw_dk_destinos.h

}  // namespace rex::x360
