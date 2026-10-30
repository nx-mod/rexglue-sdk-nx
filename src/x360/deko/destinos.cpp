// nfsmw - deko3d renderer: frames and work slots, render targets, copies (resolve and clear), the output pass with
// the game's gamma ramp, the DKSH cache file and the statistics. See nfsmw_dk_render.h.

#include <rex/x360/deko/destinos.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <rex/ui/deko3d/compilador.h>
#include <rex/ui/deko3d/presentador.h>
#include <rex/x360/deko/render.h>
#include <rex/x360/xenos.h>

// Settings of the renderer that the game hooks also read (nfsmw_nativo_ganchos.cpp). The Vulkan build defines them
// in nfsmw_nativo_dibujos.cpp, which this build leaves out: same names, types and defaults.
REXCVAR_DEFINE_BOOL(deko_bandas_profundidad, false, "deko3d",
                    "Testing: depth resolves of predicated-tiling bands into one texture, as the color ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(deko_ver_resueltas, "", "deko3d",
                      "Diagnostic: resolved texture addresses in hex, comma-separated (e.g. 044B0000,04780000); the "
                      "screen shows the front buffer and then each of them in turn, 4 s each")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(deko_edram_baldosas, false, "deko3d",
                    "Testing: EDRAM tile ownership (TraerBaldosas): a target copies in the tiles other targets wrote over "
                    "it. Costly (many small copies a frame) and not shown to help Halo 3 yet")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(deko_volcar_pasos, 0, "deko3d",
                     "Diagnostic: how many capture frames (nfsmw_captura_cada_s) of each kind, menu (under 1000 draws) "
                     "and game, get every resolve written shrunk to 288x160 as capturas/paso_*.ppm; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(deko_exportacion_memoria, true, "deko3d",
                    "Memory export (Halo 3's particles, light visibility tests, water): the vertex shaders' writes to "
                    "memory reach guest memory. false = dropped, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(deko_msaa_real, true, "deko3d",
                    "4x MSAA passes drawn into real 4x images (pixel shaders once a pixel, as the Xenos) instead of "
                    "straight into sample space (once a sample); the targets go back to sample space where the pass ends")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(deko_fotogramas_en_vuelo, 1, "deko3d",
                     "Frames the game may record ahead of the one the GPU finishes: each one more is a frame later to "
                     "the screen and no faster once the GPU is the limit. 0 = only the work slots limit it")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(deko_destinos_flotantes, false, "deko3d",
                    "Testing: 64-bit render targets and resolves as RGBA16F instead of RGBA8 (Halo 3's exposure chain); "
                    "off = everything RGBA8 as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_ps_solo_alfa, true, "NFSMW",
                    "Renderizador nativo: en las pasadas sin destino de color, el pixel shader sin las escrituras "
                    "de color (queda la prueba de alfa)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_ps_solo_alfa_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 alterna cada N segundos el pixel shader con y sin las "
                     "escrituras de color en las pasadas sin color");
REXCVAR_DEFINE_BOOL(nfsmw_sombras_sin_vegetacion, true, "NFSMW",
                    "No dibujar en el mapa de sombras lo que lleva prueba de alfa: arboles, arbustos y vallas de "
                    "alambre. Los arboles dejan de proyectar sombra");
// The same as the Vulkan build's (nfsmw_nativo_destinos.cpp), read once.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_rampa_gamma, true, "NFSMW",
                    "Renderizador nativo: aplica en la salida la rampa de gamma que carga el juego, como la pantalla "
                    "de la Xbox 360 (sin ella, los medios tonos y las sombras salen mas oscuros). false: la imagen tal "
                    "cual, como antes de la build 137")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The same as the Vulkan build's (nfsmw_nativo_destinos.cpp).
REXCVAR_DEFINE_INT32(nfsmw_nativo_leer_resueltas_texels, 4096, "NFSMW",
                     "Renderizador nativo: las texturas resueltas de hasta estos texels se copian "
                     "tambien a la memoria del invitado, que el juego lee para su exposicion (0 = "
                     "ninguna; 4096 = 64x64, las que usa la exposicion; 57600 = 320x180)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_borrados_repetidos, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): saltarse un borrado cuando el destino ya esta borrado "
                    "con ese mismo valor y no se ha dibujado nada desde entonces. No cambia ni un pixel; "
                    "'borrados saltados' en el informe C2 dice cuantos se ahorran");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_sin_copia, true, "NFSMW",
                    "Renderizador nativo (17/09, build 154; por defecto desde la 161, medido en consola: -2,36 ms "
                    "de los 7,41 de copias): al resolver un destino entero a una textura del mismo "
                    "tamano y formato, intercambia las imagenes en vez de copiar los pixeles");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_sin_borrado, true, "NFSMW",
                    "Renderizador nativo (20/09): intercambiar la imagen tambien cuando la orden de resolve no "
                    "borra el destino. Se lleva por delante la copia de 1600x1600 del segundo mapa de sombras "
                    "(2,4 ms reales). Si el juego dibuja encima sin borrar, se restaura");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_minimo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el mapa de sombras del mundo ya no se restaura copiando "
                    "1600x1600; los coches se dibujan sobre el destino borrado y el mundo muestrea el minimo de las dos "
                    "texturas. Misma imagen. Necesita la biblioteca con tfetch2DSombraMin. Se comprueba sola. false = "
                    "copiar como antes");
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);  // nfsmw_dk_dibujos.cpp
REXCVAR_DEFINE_BOOL(nfsmw_nativo_frontal_perezoso, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el Swap pinta el frontal desde su destino de render en vez "
                    "de copiarlo antes a la textura. Misma imagen; con FXAA se copia como siempre. false = copiar "
                    "siempre");
// deko3d only.
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrar_area_util, true, "NFSMW",
                    "deko3d: los borrados solo cubren las filas que el juego usa del destino (la escena de 1280x720 "
                    "vive en un destino de 1280x1280); si algo llega mas abajo, esa banda se borra antes. Misma imagen. "
                    "false = borrar el destino entero");
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrado_color_aplazado, true, "NFSMW",
                    "deko3d: el borrado de un destino de color se hace al volver a usarlo, como el de profundidad (y "
                    "deja pintar el frontal desde el destino). false = se borra en el acto");
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrado_z_aplazado, true, "NFSMW",
                    "deko3d: el borrado de una profundidad se hace al volver a usarla para dibujar, justo despues de "
                    "enlazarla, para que el ZCULL (que deko3d tira al cambiar de profundidad) empiece el pase valido. "
                    "false = se borra en el acto");
REXCVAR_DEFINE_BOOL(nfsmw_dk_compresion_profundidad, true, "NFSMW",
                    "deko3d: los destinos de profundidad (y sus texturas resueltas) con compresion por hardware: menos "
                    "ancho de banda en la escena y borrados casi gratis. Sus copias van por el motor 3D. false = sin "
                    "comprimir, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_dk_compresion_color, true, "NFSMW",
                    "deko3d: los destinos de color con compresion por hardware (DkImageFlags_HwCompression), como "
                    "SwitchWakerHD. false = sin comprimir")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_dk_subida_mb, 32, "NFSMW",
                     "deko3d: MB del anillo de subida por ranura de trabajo (vertices, indices, constantes y "
                     "texturas). Si un fotograma no cabe se envia y sigue en la siguiente ranura")
    .range(8, 128)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_dk_perfil_gpu_s, 20, "NFSMW",
                     "deko3d: cada cuantos segundos se mide un fotograma dibujo a dibujo en la GPU (una marca de tiempo "
                     "antes y despues de cada dibujo; ese fotograma va algo mas lento) para el informe de pases y "
                     "shaders mas caros. 0 = nunca");
REXCVAR_DEFINE_BOOL(nfsmw_dk_perfil_barrera, true, "NFSMW",
                    "deko3d: en el fotograma medido, el primer dibujo de cada pase se mide despues de que la GPU acabe "
                    "todo lo anterior (una barrera completa), para no contarle trabajo ajeno (copias, borrados, esperas)");
REXCVAR_DEFINE_BOOL(nfsmw_dk_perfil_variantes, true, "NFSMW",
                    "deko3d: en el fotograma medido, los dibujos a pantalla completa con texturas que se pueden repetir "
                    "se dibujan ademas con el sampler 0, con texturas vacias de 1x1 y escribiendo solo el destino 0, "
                    "antes de su copia real: que parte de su coste es el muestreo, la lectura o la salida");
REXCVAR_DEFINE_INT32(nfsmw_dk_geometria_mb, 64, "NFSMW",
                     "deko3d: MB para los vertices e indices que se repiten entre fotogramas (nfsmw_dk_geometria.h): "
                     "un rango visto dos veces con el mismo contenido (XXH3) se queda y los dibujos siguientes lo usan "
                     "sin copiarlo ni convertirlo. 0 = apagado, todo al anillo como antes")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_dk_geometria_muestreo, true, "NFSMW",
                    "deko3d: los rangos guardados se recomprueban leyendo 1 de cada 8 lineas de 64 bytes (rotando, "
                    "asi cada linea se lee al menos cada 8 fotogramas) mas la primera y la ultima, en vez de todo. Las "
                    "20000 primeras comprobaciones hacen tambien la huella completa: con un solo desacuerdo se apaga "
                    "solo. false = huella completa cada fotograma")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void RexSwitchPerfCount(unsigned id);

// The 30 FPS guard (nfsmw_guardia_30) lives in nfsmw_recorte_sombras.cpp.
namespace rex::x360 {

// Guest waits handled by the ring thread (nfsmw_nativo_peticion.h). The Vulkan build defines it in
// nfsmw_nativo_dibujos.cpp; this build has no upload deduplication yet.
std::atomic<uint32_t> g_sincronizaciones_anillo{0};

namespace {

// The geometry shader of rectangle lists (as Xenia's): the three corners of each rectangle in, a strip of four out.
// The corner at the right angle (c) is the one off the longest edge; the fourth is a + b - c for the position and for
// every interpolator (the 25 locations the translators write: TEXCOORD0-15 at 0-15, then COLOR and the rest).
// Strip c, a, b, d keeps the winding of the game's triangle (c, a, b is it, rotated).
std::string GlslRectangulos() {
  constexpr int kUbicaciones = 25;
  std::string g = "#version 460\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
  for (int k = 0; k < kUbicaciones; ++k) {
    g += "layout(location = " + std::to_string(k) + ") in vec4 e" + std::to_string(k) + "[];\n";
    g += "layout(location = " + std::to_string(k) + ") out vec4 s" + std::to_string(k) + ";\n";
  }
  std::string copiar, cuarto;
  for (int k = 0; k < kUbicaciones; ++k) {
    const std::string n = std::to_string(k);
    copiar += " s" + n + " = e" + n + "[i];";
    cuarto += " s" + n + " = e" + n + "[a] + e" + n + "[b] - e" + n + "[c];";
  }
  g += "void Copiar(int i) { gl_Position = gl_in[i].gl_Position;" + copiar + " EmitVertex(); }\n";
  g += "void Cuarto(int a, int b, int c) {\n"
       "  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;" + cuarto + "\n"
       "  EmitVertex();\n}\n";
  g += "vec2 Punto(int i) { return gl_in[i].gl_Position.xy / gl_in[i].gl_Position.w; }\n"
       "void main() {\n"
       "  vec2 p0 = Punto(0), p1 = Punto(1), p2 = Punto(2);\n"
       "  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);\n"
       "  int c = d01 >= d12 && d01 >= d20 ? 2 : (d12 >= d20 ? 0 : 1);\n"
       "  int a = c == 2 ? 0 : c + 1, b = c == 0 ? 2 : (c == 1 ? 0 : 1);\n"
       "  Copiar(c);\n  Copiar(a);\n  Copiar(b);\n  Cuarto(a, b, c);\n  EndPrimitive();\n}\n";
  return g;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using deko::ContextoSalidaDeko3d;

constexpr uint32_t kAltoMaximoDestino = 2048;
constexpr uint32_t kRanuras = 3;
// Read-backs per work slot: a frame has two of 64x64 (32 KB); room for 16 of them.
constexpr uint32_t kBytesLecturaRanura = 256u << 10;
// The shadow map (nfsmw_nativo_sombra_minimo): its side, and the clean cycles watched before applying (~10 s of race).
constexpr uint32_t kLadoSombras = 1600;
constexpr uint64_t kSombraMinimoCiclos = 300;
// GPU timestamps per work slot (16 bytes each in the core's report block) and the end-of-work category.
constexpr uint32_t kMarcasPorRanura = 1024;  // 3 slots x 1024 x 16 bytes = 48 KB of the 64 KB query block
constexpr uint8_t kGpuFin = 0xFF;

// With 4-byte texels, DesplazamientoMosaico2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (the Vulkan path's TablaMosaico2DTexel4).
const std::array<uint16_t, 1024>& TablaMosaico2DTexel4() {
  static const std::array<uint16_t, 1024> tabla = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(DesplazamientoMosaico2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return tabla;
}

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fijo16p8(float valor) {
  if (!(std::abs(valor) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double escalado = std::clamp(double(valor) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(escalado));
}

int32_t ExtenderSigno15(uint32_t valor) { return int32_t(valor << 17) >> 17; }

DkImageView VistaCompleta(const DkImage& imagen) {
  DkImageView v;
  dkImageViewDefaults(&v, &imagen);
  return v;
}

std::filesystem::path RutaCache() { return rex::filesystem::GetExecutableFolder() / "cache" / "nfsmw_dksh.bin"; }
std::filesystem::path RutaDiario() { return RutaCache().string() + ".diario"; }

// A whole file in one read (istreambuf_iterator went a byte at a time: seconds for the 18 MB cache on the console).
bool LeerArchivo(const std::filesystem::path& ruta, std::vector<uint8_t>& datos) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  const std::streamsize bytes = f.tellg();
  if (bytes < 0) {
    return false;
  }
  datos.resize(size_t(bytes));
  f.seekg(0);
  return bytes == 0 || bool(f.read(reinterpret_cast<char*>(datos.data()), bytes));
}

// The whole file, through a temporary and a rename (a cut write leaves the old one).
bool EscribirArchivo(const std::filesystem::path& ruta, const std::vector<uint8_t>& datos) {
  std::error_code error;
  std::filesystem::create_directories(ruta.parent_path(), error);
  const std::filesystem::path temporal = ruta.string() + ".tmp";
  {
    std::ofstream f(temporal, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(datos.data()), std::streamsize(datos.size()));
    if (!f) {
      REXLOG_WARN("[deko3d] no se pudo escribir {}", temporal.string());
      return false;
    }
  }
  std::filesystem::rename(temporal, ruta, error);
  if (error) {
    std::filesystem::remove(ruta, error);
    std::filesystem::rename(temporal, ruta, error);
  }
  return !error;
}

// ---- The output pass's shaders (GLSL for uam). The same work as the Vulkan path's nfsmw_salida_rampa_gamma.frag
// (see it for the reasons): a full-screen triangle, the source's texel, and the game's ramp.
constexpr const char* kVsSalida = R"(#version 460
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// The ramp is a 256x1 texture, not a uniform array: each pixel indexes it with its own color, and a uniform array
// indexed by a different value in every lane of a warp is read one lane at a time (the constant cache serializes
// divergent addresses). It cost 2.3-2.5 ms per frame at 307.2 MHz.
constexpr const char* kFsRampa = R"(
layout(binding = 0) uniform sampler2D origen;
layout(binding = 1) uniform sampler2D tabla;
layout(std140, binding = 0) uniform Rampa {
  vec4 mezcla;   // saturacion, vibracion, 1 / gamma (GRADUACION)
  vec4 efectos;  // vineta, lineas (GRADUACION)
  ivec4 desplazamiento;
  vec4 inverso;
} rampa;
layout(location = 0) out vec4 color;

const vec3 kPesosLuma = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) { return dot(textureLod(origen, uv, 0.0).rgb, kPesosLuma); }

vec3 Fxaa(vec2 uv) {
  vec2 paso = rampa.inverso.xy;
  vec3 rgb_centro = textureLod(origen, uv, 0.0).rgb;
  float luma_centro = dot(rgb_centro, kPesosLuma);
  float luma_no = Luma(uv + vec2(-paso.x, -paso.y));
  float luma_ne = Luma(uv + vec2(paso.x, -paso.y));
  float luma_so = Luma(uv + vec2(-paso.x, paso.y));
  float luma_se = Luma(uv + vec2(paso.x, paso.y));
  float luma_min = min(luma_centro, min(min(luma_no, luma_ne), min(luma_so, luma_se)));
  float luma_max = max(luma_centro, max(max(luma_no, luma_ne), max(luma_so, luma_se)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return rgb_centro;
  }
  vec2 direccion = vec2(-((luma_no + luma_ne) - (luma_so + luma_se)), (luma_no + luma_so) - (luma_ne + luma_se));
  float reduccion = max((luma_no + luma_ne + luma_so + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float inversa_min = 1.0 / (min(abs(direccion.x), abs(direccion.y)) + reduccion);
  direccion = clamp(direccion * inversa_min, vec2(-8.0), vec2(8.0)) * paso;
  vec3 rgb_a = 0.5 * (textureLod(origen, uv + direccion * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(origen, uv + direccion * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(origen, uv + direccion * -0.5, 0.0).rgb +
                                     textureLod(origen, uv + direccion * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kPesosLuma);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - rampa.desplazamiento.xy;
  vec3 texel;
#if FXAA
  texel = Fxaa((vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy);
#elif EXACTA
  texel = texelFetch(origen, pixel, 0).rgb;
#else
  texel = textureLod(origen, (vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy, 0.0).rgb;
#endif
#if RAMPA
  ivec3 i = ivec3(texel * 255.0 + 0.5);
  vec3 c = vec3(texelFetch(tabla, ivec2(i.r, 0), 0).r, texelFetch(tabla, ivec2(i.g, 0), 0).g,
                texelFetch(tabla, ivec2(i.b, 0), 0).b);
#else
  vec3 c = texel;
#endif
#if GRADUACION
  float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(luma), c, rampa.mezcla.x);
  float saturado = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
  c = mix(vec3(luma), c, 1.0 + rampa.mezcla.y * (1.0 - clamp(saturado, 0.0, 1.0)));
  c = pow(max(c, vec3(0.0001)), vec3(rampa.mezcla.z));
  vec2 uv = (vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy;
  float v = rampa.efectos.x;
  c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
       (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
  if ((uint(pixel.y) % 3u) == 0u) {
    c *= 1.0 - rampa.efectos.y * 0.7;
  }
  c = clamp(c, 0.0, 1.0);
#endif
  color = vec4(c, 1.0);
}
)";

// The 3D engine's copies (Copiar3D): the source's texel at this pixel plus the offset, as color or as depth.
// Mode 1: the Xenos's 7e3 bits of each colour channel read as unorm (a 7e3 drawing copied as 2_10_10_10: the EDRAM
// holds the bits), with Xenia's conversion (spirv_translator_rb.cpp, PreClampedFloat32To7e3: round to nearest even);
// alpha is the format's 2 bits. Then, in both modes, the copy's 2^exp_bias (RB_COPY_DEST_INFO), as Xenia's resolve.
constexpr const char* kFsCopiaColor = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modo;
  vec4 escala;
} copia;
layout(location = 0) out vec4 color;
uint a7e3(float f) {
  uint u = floatBitsToUint(clamp(f, 0.0, 31.875));
  uint subnormal = ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - min(u >> 23, 125u), 24u);
  uint b = u < 0x3E800000u ? subnormal : u - (124u << 23);
  return ((b + 0x7FFFu + ((b >> 16) & 1u)) >> 16) & 0x3FFu;
}
// Xenia's Float7e3To32 (xenos.cpp): 7e3 bits back to their value.
float de7e3(uint f) {
  f &= 0x3FFu;
  if (f == 0u) {
    return 0.0;
  }
  uint m = f & 0x7Fu;
  int e = int(f >> 7);
  if (e == 0) {
    int l = 7 - findMSB(m);
    e = 1 - l;
    m = (m << uint(l)) & 0x7Fu;
  }
  return uintBitsToFloat((uint(e + 124) << 23) | (m << 16));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 v;
  if (copia.desplazamiento.z > 1) {
    // A 4x MSAA resolve: the pixel's 2x2 samples averaged (xy: the source's sample-space origin, w: the destination's
    // origin, x in the low 16 bits).
    ivec2 s = (p - ivec2(copia.desplazamiento.w & 0xFFFF, copia.desplazamiento.w >> 16)) * 2 + copia.desplazamiento.xy;
    v = (texelFetch(origen, s, 0) + texelFetch(origen, s + ivec2(1, 0), 0) + texelFetch(origen, s + ivec2(0, 1), 0) +
         texelFetch(origen, s + ivec2(1, 1), 0)) * 0.25;
  } else {
    v = texelFetch(origen, p + copia.desplazamiento.xy, 0);
  }
  if (copia.modo.x == 3) {
    // A 7e3 drawing read as 8_8_8_8: its 32-bit word (R | G << 10 | B << 20 | A << 30), byte by byte.
    uint w = a7e3(v.r) | (a7e3(v.g) << 10) | (a7e3(v.b) << 20) | (uint(round(clamp(v.a, 0.0, 1.0) * 3.0)) << 30);
    v = vec4(float(w & 0xFFu), float((w >> 8) & 0xFFu), float((w >> 16) & 0xFFu), float(w >> 24)) / 255.0;
  } else if (copia.modo.x == 4) {
    // An 8_8_8_8 drawing read as 7e3: its bytes as one word, then the three 7e3 fields and the 2 alpha bits.
    uvec4 b = uvec4(round(clamp(v, 0.0, 1.0) * 255.0));
    uint w = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    v = vec4(de7e3(w), de7e3(w >> 10), de7e3(w >> 20), float(w >> 30) / 3.0);
  } else if (copia.modo.x == 2) {
    uvec3 b = uvec3(round(clamp(v.rgb, 0.0, 1.0) * 1023.0));
    v = vec4(de7e3(b.r), de7e3(b.g), de7e3(b.b), v.a);
  } else if (copia.modo.x == 1) {
    v = vec4(float(a7e3(v.r)), float(a7e3(v.g)), float(a7e3(v.b)), 0.0) / 1023.0 +
        vec4(0.0, 0.0, 0.0, round(clamp(v.a, 0.0, 1.0) * 3.0) / 3.0);
  }
  color = v * copia.escala.x;
}
)";
constexpr const char* kFsCopiaProfundidad = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;  // xy: source minus destination, in pixels; z: samples per pixel along each axis (0 or 1: none)
} copia;
void main() {
  // A 4x MSAA source is in sample space: a pixel's depth is its sample 0, the top-left of its 2x2 (as the console
  // resolves Halo 3's menu G-buffer depth; averaging would make in-between depths along every silhouette).
  ivec2 fuente = ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy;
  gl_FragDepth = texelFetch(origen, fuente * max(copia.desplazamiento.z, 1), 0).r;
}
)";

// The stencil restore (RestaurarEstencil): the red byte of the colour drawn at the depth buffer's base. Depth tiles hold
// the two 40-sample halves of every 80-sample-wide tile swapped against colour tiles (as Xenia's render target cache
// swaps them), so the texel with this pixel's byte is in the other half. bit < 0: every fragment (the clear pass);
// otherwise only those whose byte has that bit.
constexpr const char* kFsEstencil = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Estencil {
  ivec4 bit;
} e;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int x = (p.x / 80) * 80 + ((p.x % 80) + 40) % 80;
  uint byte = uint(round(clamp(texelFetch(origen, ivec2(x, p.y), 0).r, 0.0, 1.0) * 255.0));
  if (e.bit.x >= 0 && ((byte >> uint(e.bit.x)) & 1u) == 0u) {
    discard;
  }
}
)";

// The same restore in one pass: the fragment exports the byte as its stencil reference (ARB_shader_stencil_export,
// which Maxwell has), and the replace op writes it. Nine passes over the target cost Halo 3's menu 89 ms a frame.
constexpr const char* kFsEstencilExportado = R"(#version 460
#extension GL_ARB_shader_stencil_export : require
layout(binding = 0) uniform sampler2D origen;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int x = (p.x / 80) * 80 + ((p.x % 80) + 40) % 80;
  gl_FragStencilRefARB = int(round(clamp(texelFetch(origen, ivec2(x, p.y), 0).r, 0.0, 1.0) * 255.0));
}
)";

// Real 4x MSAA (CompaneraMs): sample i of a pixel is texel (2x + (i & 1), 2y + (i >> 1)) of the sample-space image,
// as the sample locations are set (DibujarRectanguloMs). Packing runs once a sample (gl_SampleID); depth carries its
// stencil through the exported reference.
constexpr const char* kFsMsColor = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(location = 0) out vec4 salida;
void main() {
  salida = texelFetch(origen, ivec2(gl_FragCoord.xy) * 2 + ivec2(gl_SampleID & 1, gl_SampleID >> 1), 0);
}
)";
constexpr const char* kFsMsZ = R"(#version 460
#extension GL_ARB_shader_stencil_export : require
layout(binding = 0) uniform sampler2D profundidad;
layout(binding = 1) uniform usampler2D estencil;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) * 2 + ivec2(gl_SampleID & 1, gl_SampleID >> 1);
  gl_FragDepth = texelFetch(profundidad, p, 0).r;
  gl_FragStencilRefARB = int(texelFetch(estencil, p, 0).r);
}
)";
constexpr const char* kFsDesmsColor = R"(#version 460
layout(binding = 0) uniform sampler2DMS origen;
layout(location = 0) out vec4 salida;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  salida = texelFetch(origen, p >> 1, (p.x & 1) | ((p.y & 1) << 1));
}
)";
constexpr const char* kFsDesmsZ = R"(#version 460
#extension GL_ARB_shader_stencil_export : require
layout(binding = 0) uniform sampler2DMS profundidad;
layout(binding = 1) uniform usampler2DMS estencil;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int m = (p.x & 1) | ((p.y & 1) << 1);
  gl_FragDepth = texelFetch(profundidad, p >> 1, m).r;
  gl_FragStencilRefARB = int(texelFetch(estencil, p >> 1, m).r);
}
)";

constexpr uint32_t kRampaExacta = 1, kRampaGraduacion = 2, kRampaFxaa = 4;
constexpr uint32_t kBytesUboRampa = 4 * 16;  // mezcla, efectos, desplazamiento, inverso (the ramp is a texture)

}  // namespace

// ---- Construction

RenderDk::RenderDk(deko::ProveedorDeko3d& proveedor, rex::memory::Memory* memoria, rex::x360::RenderHooks& hooks)
    : proveedor_(proveedor), compilador_(proveedor.compilador()), memoria_(memoria), hooks_(hooks) {
  deko::ConfigNucleo config;
  config.ranuras = kRanuras;
  config.mb_subida = uint32_t(REXCVAR_GET(nfsmw_dk_subida_mb));
  config.kb_comandos = 2048;
  config.kb_previos = 512;
  config.mb_codigo = 16;
  config.mb_trozo_imagenes = 32;
  config.descriptores_imagen = 8192;
  config.descriptores_sampler = 1024;
  nucleo_.Crear(proveedor_.dispositivo(), config);
  geometria_.Crear(nucleo_, uint32_t(REXCVAR_GET(nfsmw_dk_geometria_mb)), REXCVAR_GET(nfsmw_dk_geometria_muestreo));
  descriptores_por_liberar_.resize(kRanuras);
  lecturas_.resize(kRanuras);
  marcas_.resize(kRanuras);
  lectura_bloque_ = nucleo_.CrearBloqueLectura(kRanuras * kBytesLecturaRanura, "lecturas de vuelta (por ranura)");
  // GPU-cached: atomics and stores in uncached memory went to DRAM one by one (Halo 3's particle updates, 20 ms a
  // frame). The fence flushes them; the region's counter is zeroed through the command list.
  export_bloque_ = REXCVAR_GET(deko_exportacion_memoria)
                       ? nucleo_.CrearBloqueLecturaCacheado(kRanuras * kDwordsExportRanura * 4,
                                                            "registros de exportacion a memoria")
                       : nullptr;
  if (export_bloque_) {
    export_cpu_ = static_cast<uint32_t*>(dkMemBlockGetCpuAddr(export_bloque_));
    export_gpu_ = dkMemBlockGetGpuAddr(export_bloque_);
  }
  lectura_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(lectura_bloque_));
  lectura_gpu_ = dkMemBlockGetGpuAddr(lectura_bloque_);
  if (REXCVAR_GET(nfsmw_dk_perfil_gpu_s) > 0) {
    perfil_bloque_ = nucleo_.CrearBloqueLectura(kMarcasPerfil * 32, "perfil de la GPU por dibujo");
    perfil_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(perfil_bloque_));
    perfil_gpu_ = dkMemBlockGetGpuAddr(perfil_bloque_);
    perfil_marcas_.reserve(kMarcasPerfil);
    perfil_ns_ = deko::AhoraNs();
  }
  rampa_gamma_ = REXCVAR_GET(nfsmw_nativo_rampa_gamma);
  for (uint32_t i = 0; i < 256; ++i) {
    const uint16_t v = uint16_t((i * 1023 + 127) / 255);
    rampa_juego_[i] = {v, v, v};
  }
  RecalcularRampa();

  // The DKSH cache: the console's own, or the one the PC tool made (shaders/dksh) next to the NRO.
  const uint64_t inicio_cache = deko::AhoraNs();
  std::string motivo;
  for (const std::filesystem::path& ruta :
       {RutaCache(), rex::filesystem::GetExecutableFolder() / "shadercache_dksh.bin"}) {
    std::vector<uint8_t> datos;
    if (!LeerArchivo(ruta, datos)) {
      continue;
    }
    if (cache_.Cargar(datos, motivo)) {
      REXLOG_INFO("[deko3d] cache de DKSH: {} entradas de {}", cache_.size(), ruta.string());
      break;
    }
    REXLOG_WARN("[deko3d] cache de DKSH {} no se usa: {}", ruta.string(), motivo);
  }
  {
    // The journal of the last runs (GuardarCacheSiHaceFalta): merged, then written into the file once, here at start
    // and on the writer thread, so the journal starts empty.
    std::vector<uint8_t> datos;
    bool hay_diario = false;
    {
      // Closed before it may be deleted: the SD card's filesystem keeps an open file (a journal in an old format,
      // never deleted, threw away every run's new shaders at the next start).
      hay_diario = LeerArchivo(RutaDiario(), datos);
    }
    if (hay_diario) {
      const int anadidas = cache_.CargarDiario(datos);
      REXLOG_INFO("[deko3d] diario de la cache de DKSH: {} entradas ({} KB)", anadidas, datos.size() >> 10);
      if (anadidas > 0) {
        auto completa = std::make_shared<std::vector<uint8_t>>(cache_.Serializar());
        guardando_ = std::thread([completa] {
          if (EscribirArchivo(RutaCache(), *completa)) {
            std::error_code error;
            std::filesystem::remove(RutaDiario(), error);
          }
        });
      } else {
        std::error_code error;
        std::filesystem::remove(RutaDiario(), error);
      }
    }
  }
  REXLOG_INFO("[deko3d] cache de DKSH lista en {:.0f} ms: {} entradas", double(deko::AhoraNs() - inicio_cache) / 1e6,
              cache_.size());
  if (!cache_.size()) {
    REXLOG_WARN("[deko3d] sin cache de DKSH: cada shader se compila en la consola la primera vez que se usa");
  }
  cache_guardada_ns_ = deko::AhoraNs();

  const deko::Compilador::Resultado vacio = compilador_.Compilar(false, "#version 460\nvoid main() {}\n");
  if (vacio.ok) {
    fs_vacio_valido_ = nucleo_.CargarShader(fs_vacio_, vacio.dksh, "fragmentos vacio");
  }
  if (!fs_vacio_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar el pixel shader vacio: {}", vacio.log);
  }
  const deko::Compilador::Resultado rect = compilador_.Compilar(deko::Compilador::Etapa::kGeometria, GlslRectangulos());
  if (rect.ok) {
    gs_rectangulos_valido_ = nucleo_.CargarShader(gs_rectangulos_, rect.dksh, "rectangulos");
  }
  if (!gs_rectangulos_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar el geometry shader de rectangulos: {}", rect.log);
  }
  CrearShadersSalida();
  AsegurarTrabajo();
  CrearVacias();
  REXLOG_INFO("[deko3d] destinos: etapa P2 (videos, menus y titulo): {} MB de subida por ranura, compresion de color "
              "{}, rampa de gamma {}",
              config.mb_subida, REXCVAR_GET(nfsmw_dk_compresion_color) ? "SI" : "no", rampa_gamma_ ? "SI" : "no");
}

RenderDk::~RenderDk() {
  if (nucleo_.TrabajoAbierto()) {
    CerrarTrabajo();
  }
  nucleo_.EsperarGpu();
  GuardarCacheSiHaceFalta(true);
  if (guardando_.joinable()) {
    guardando_.join();
  }
  geometria_.Destruir();
  nucleo_.Destruir();
}

// ---- Frames and work slots

void RenderDk::AsegurarTrabajo() {
  if (nucleo_.TrabajoAbierto()) {
    return;
  }
  nucleo_.EmpezarTrabajo(++trabajo_);
  // The descriptors freed the last time this slot was recorded: its fence has just been waited for. The same for
  // the read-backs recorded then, if Presentar has not written them yet.
  auto& libres = descriptores_por_liberar_[trabajo_ % kRanuras];
  descriptores_libres_.insert(descriptores_libres_.end(), libres.begin(), libres.end());
  libres.clear();
  EscribirLecturas(uint32_t(trabajo_ % kRanuras));
  AplicarExportaciones(uint32_t(trabajo_ % kRanuras));  // its fence has been waited for: the records are written
  export_ranuras_[trabajo_ % kRanuras].trabajo = trabajo_;
  export_ranuras_[trabajo_ % kRanuras].fotograma = fotograma_;
  if (!pasos_.empty()) {
    EscribirPasos();
  }
  lecturas_[trabajo_ % kRanuras].trabajo = trabajo_;
  LeerMarcasGpu(uint32_t(trabajo_ % kRanuras));
  dedupe_.NuevoFotograma(trabajo_);  // a new upload ring: the copies of the previous works are not here
  geometria_.NuevoTrabajo(trabajo_);
  InicioDeTrabajo();
}

void RenderDk::InicioDeTrabajo() {
  DkCmdBuf cmd = nucleo_.comandos();
  marcas_[trabajo_ % kRanuras].clear();
  MarcarGpu(kGpuOtros);  // the work's start
  // Another command list: what the previous one drew may still be in flight when a transfer of this one starts.
  trabajo_3d_sin_barrera_ = true;
  transferencia_sin_barrera_ = false;
  rop_sin_barrera_ = false;  // the Fragments barrier above made it visible
  copia3d_pendiente_ = false;
  // What an earlier submission sampled or wrote (the previous output pass, the presenter's paint of the same image)
  // is done before this one's draws; the caches forget what the CPU rewrote in this slot's memory.
  dkCmdBufBarrier(cmd, DkBarrier_Fragments,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
  dkCmdBufTiledCacheOp(cmd, DkTiledCacheOp_Disable);
  nucleo_.BindDescriptores();
  OlvidarEstado();
}

void RenderDk::CerrarTrabajo() {
  if (!nucleo_.TrabajoAbierto()) {
    return;
  }
  TerminarPase();  // the deferred sky belongs to this work's pass
  // The work's read-backs, from their images to the buffer in one copy engine phase (LeerResuelta).
  if (!lecturas_aplazadas_.empty()) {
    AntesDeTransferir();
    for (LecturaAplazada& l : lecturas_aplazadas_) {
      const DkImageView vista = VistaCompleta(l.imagen.imagen);
      const DkImageRect rect = {0, 0, 0, l.ancho, l.alto, 1};
      dkCmdBufCopyImageToBuffer(nucleo_.comandos(), &vista, &rect, &l.destino, 0);
      DestruirImagen(l.imagen);  // its memory is given back after this work's fence
    }
    lecturas_aplazadas_.clear();
  }
  // The fence is released by the 3D engine: a transfer still running on the copy engine is waited for first (the
  // read-backs are read when the fence has passed).
  if (transferencia_sin_barrera_) {
    BarreraTransferencia();
    transferencia_sin_barrera_ = false;
  }
  MarcarGpu(kGpuFin);
  nucleo_.TerminarTrabajo();
}

void RenderDk::MarcarGpu(uint32_t categoria) {
  if (!nucleo_.TrabajoAbierto()) {
    return;
  }
  std::vector<uint8_t>& m = marcas_[trabajo_ % kRanuras];
  // The same category goes on; past the limit the last segment absorbs the rest (the end always fits).
  if (categoria != kGpuFin && ((!m.empty() && m.back() == categoria) || m.size() + 2 > kMarcasPorRanura)) {
    return;
  }
  dkCmdBufReportCounter(nucleo_.comandos(), DkCounter_Timestamp,
                        nucleo_.ConsultasGpu() + ((trabajo_ % kRanuras) * kMarcasPorRanura + m.size()) * 16);
  m.push_back(uint8_t(categoria));
}

void RenderDk::LeerMarcasGpu(uint32_t ranura) {
  std::vector<uint8_t>& m = marcas_[ranura];
  if (m.size() < 2 || m.back() != kGpuFin) {
    m.clear();
    return;
  }
  const uint8_t* const base = nucleo_.ConsultasCpu() + size_t(ranura) * kMarcasPorRanura * 16;
  auto marca = [&](size_t i) {
    uint64_t t = 0;
    std::memcpy(&t, base + i * 16 + 8, sizeof(t));
    return t;
  };
  uint64_t previa = marca(0);
  const uint64_t inicio = previa;
  for (size_t i = 1; i < m.size(); ++i) {
    const uint64_t t = marca(i);
    if (t >= previa && m[i - 1] < kGpuCategorias) {
      gpu_categorias_ns_[m[i - 1]] += uint64_t(double(t - previa) * deko::kNsPorUnidadGpu);
    } else if (t >= previa && m[i - 1] >= kGpuSubBorradoPase && m[i - 1] < kGpuSubBorradoPase + kGpuSubBorrados) {
      // A kind of clear: counted in the clears and on its own (the report's clear line).
      const uint64_t ns = uint64_t(double(t - previa) * deko::kNsPorUnidadGpu);
      gpu_categorias_ns_[kGpuBorrados] += ns;
      gpu_sub_borrados_ns_[m[i - 1] - kGpuSubBorradoPase] += ns;
    }
    previa = t;
  }
  if (previa > inicio) {
    gpu_ns_ += uint64_t(double(previa - inicio) * deko::kNsPorUnidadGpu);
    ++gpu_trabajos_;
  }
  // The GPU idle between the previous work's end and this one's start; longer than 100 ms is a pause, not a gap.
  if (marca_fin_previa_ && inicio > marca_fin_previa_) {
    const double hueco = double(inicio - marca_fin_previa_) * deko::kNsPorUnidadGpu;
    if (hueco < 100e6) {
      gpu_categorias_ns_[kGpuHuecoEntreTrabajos] += uint64_t(hueco);
    }
  }
  marca_fin_previa_ = previa;
  m.clear();
}

bool RenderDk::CambiarDeRanura() {
  ++cambios_ranura_;
  CerrarTrabajo();
  AsegurarTrabajo();
  return true;
}

void RenderDk::OlvidarEstado() {
  g_ = Grabado{};
  constantes_trabajo_ = UINT64_MAX;
  compartidas_trabajo_ = UINT64_MAX;
}

void RenderDk::BarreraTransferencia() {
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full, DkInvalidateFlags_Image);
  ++barreras_;
}

void RenderDk::AntesDeTransferir() {
  if (trabajo_3d_sin_barrera_) {
    BarreraTransferencia();  // the draws that wrote the source, and the ones that sampled the destination
    trabajo_3d_sin_barrera_ = false;
  }
  transferencia_sin_barrera_ = true;
}

void RenderDk::AntesDe3D() {
  if (transferencia_sin_barrera_) {
    // The transfer is done and the texture caches forget the old texels (and ZCULL, if a depth changed).
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_;
    transferencia_sin_barrera_ = false;
    invalidar_zcull_ = false;
    copia3d_pendiente_ = false;
  } else if (copia3d_pendiente_) {
    // A 3D copy: what it wrote is sampled from now on, and its source may be written (cleared, drawn on) next.
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Fragments,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_fragmentos_;
    copia3d_pendiente_ = false;
    invalidar_zcull_ = false;
  } else if (invalidar_zcull_) {
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_None, DkInvalidateFlags_Zcull);
    invalidar_zcull_ = false;
  }
  trabajo_3d_sin_barrera_ = true;
  rop_sin_barrera_ = true;
}

bool RenderDk::Copiar3D(const ImagenDk& origen, uint32_t x0, uint32_t y0, uint32_t ancho, uint32_t alto,
                        ImagenDk& destino, uint32_t dx, uint32_t dy, int32_t modo, float escala, uint32_t muestras) {
  LeerMuestras(const_cast<ImagenDk&>(origen));
  EscribirMuestras(destino);
  const bool z = destino.profundidad;
  if (!vs_salida_valido_ || !(z ? fs_copia_profundidad_valido_ : fs_copia_color_valido_)) {
    return false;
  }
  AsegurarTrabajo();
  TerminarPase();  // the copy binds other targets
  const deko::Subida ubo = Reservar(256, DK_UNIFORM_BUF_ALIGNMENT);
  if (!ubo) {
    return false;
  }
  // Color from a 4x surface (muestras > 1) averages each pixel's samples: the shader takes both origins.
  const bool promedio = !z && muestras > 1;
  const int32_t desplazamiento[4] = {promedio ? int32_t(x0) : int32_t(x0) - int32_t(dx),
                                     promedio ? int32_t(y0) : int32_t(y0) - int32_t(dy), int32_t(muestras),
                                     promedio ? int32_t(dx | (dy << 16)) : 0};
  const int32_t modos[4] = {modo, 0, 0, 0};
  const float escalas[4] = {escala, 0.0f, 0.0f, 0.0f};
  std::memcpy(ubo.cpu, desplazamiento, sizeof(desplazamiento));
  std::memcpy(ubo.cpu + 16, modos, sizeof(modos));
  std::memcpy(ubo.cpu + 32, escalas, sizeof(escalas));
  const uint32_t ranura = VistaDe(origen, 0x688, z ? 0 /* RRRR */ : 0x688);
  DkCmdBuf cmd = nucleo_.comandos();
  MarcarGpu(kGpuCopias);
  if (transferencia_sin_barrera_) {
    BarreraTransferencia();  // a copy engine transfer wrote what this reads (or reads what this writes)
    transferencia_sin_barrera_ = false;
    copia3d_pendiente_ = false;
    rop_sin_barrera_ = false;
  }
  if (rop_sin_barrera_ || copia3d_pendiente_) {
    // The source was drawn (or copied into) since the last barrier: its writes must reach the texture units.
    dkCmdBufBarrier(cmd, DkBarrier_Fragments,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_fragmentos_;
    rop_sin_barrera_ = false;
    copia3d_pendiente_ = false;
    invalidar_zcull_ = false;
  }
  DkImageView vista;
  dkImageViewDefaults(&vista, &destino.imagen);
  if (z) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  }
  const DkViewport vp = {float(dx), float(dy), float(ancho), float(alto), 0.0f, 1.0f};
  const DkScissor tijera = {dx, dy, ancho, alto};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  if (z) {
    cws.masks = 0;
  }
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = z;
  ds.depthWriteEnable = z;
  ds.depthCompareOp = DkCompareOp_Always;
  ds.stencilTestEnable = false;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_, z ? &fs_copia_profundidad_ : &fs_copia_color_};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, 256);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(ranura, 0));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  ++copias_3d_;
  copia3d_pendiente_ = true;
  trabajo_3d_sin_barrera_ = true;  // for a copy engine transfer that comes next
  OlvidarEstado();
  return true;
}

bool RenderDk::BorradoRapidoD3D(const uint32_t* r, uint32_t cuenta) {
  const uint32_t base = r[gr::XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF;
  const uint32_t superficie = r[gr::XE_GPU_REG_RB_SURFACE_INFO];
  const uint32_t msaa = (superficie >> 16) & 0x3;  // 0 = 1x, 1 = 2x, 2 = 4x
  const uint32_t por_x = msaa == 2 ? 2 : 1, por_y = msaa == 0 ? 1 : 2;
  const uint32_t pitch_color = (superficie & 0x3FFF) * por_x;
  ImagenDk* imagenes[4] = {};
  uint32_t n = 0;
  for (auto& [clave, imagen] : destinos_) {
    if (imagen.viva && imagen.edram_base == base && imagen.edram_pitch == pitch_color && n < 4) {
      imagenes[n++] = &imagen;
    }
  }
  if (!n) {
    return false;  // no colour buffer there: a real depth clear
  }
  // The clear's vertices, through vertex fetch constant 0: x, y, z, then the colour (r, g, b, a), 7 floats each.
  constexpr uint32_t kZancada = 7;
  const uint32_t w0 = r[kRegFetch], w1 = r[kRegFetch + 1];
  if ((w0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex) || cuenta < 3 ||
      ((w1 >> 2) & 0xFFFFFF) < cuenta * kZancada) {
    return false;
  }
  const uint8_t* datos = memoria_->TranslatePhysical(w0 & ~0x3u);
  const auto orden = static_cast<xenos::Endian>(w1 & 0x3);
  const auto valor = [&](uint32_t i) {
    uint32_t crudo;
    std::memcpy(&crudo, datos + size_t(i) * 4, 4);
    crudo = xenos::GpuSwap(crudo, orden);
    float f;
    std::memcpy(&f, &crudo, 4);
    return f;
  };
  const float color[4] = {valor(3), valor(4), valor(5), valor(6)};
  for (const float c : color) {
    if (!std::isfinite(c)) {
      return false;
    }
  }
  AsegurarTrabajo();
  TerminarPase();
  AntesDe3D();
  DkCmdBuf cmd = nucleo_.comandos();
  MarcarGpu(kGpuSubBorradoRapido);
  for (uint32_t i = 0; i < n; ++i) {
    ImagenDk& imagen = *imagenes[i];
    EscribirMuestras(imagen);
    HacerBorradoPendiente(imagen);
    DkImageView vista;
    dkImageViewDefaults(&vista, &imagen.imagen);
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
    for (uint32_t v = 0; v + 2 < cuenta; v += 3) {
      // A rectangle's corners sit on pixel edges; the surface's pixels are por_x x por_y of the buffer's.
      const float xs[3] = {valor(v * kZancada), valor((v + 1) * kZancada), valor((v + 2) * kZancada)};
      const float ys[3] = {valor(v * kZancada + 1), valor((v + 1) * kZancada + 1), valor((v + 2) * kZancada + 1)};
      const int32_t x0 = std::clamp(int32_t(std::floor(std::min({xs[0], xs[1], xs[2]}) + 0.5f)) * int32_t(por_x), 0,
                                    int32_t(imagen.ancho));
      const int32_t y0 = std::clamp(int32_t(std::floor(std::min({ys[0], ys[1], ys[2]}) + 0.5f)) * int32_t(por_y), 0,
                                    int32_t(imagen.alto));
      const int32_t x1 = std::clamp(int32_t(std::floor(std::max({xs[0], xs[1], xs[2]}) + 0.5f)) * int32_t(por_x), x0,
                                    int32_t(imagen.ancho));
      const int32_t y1 = std::clamp(int32_t(std::floor(std::max({ys[0], ys[1], ys[2]}) + 0.5f)) * int32_t(por_y), y0,
                                    int32_t(imagen.alto));
      if (x1 <= x0 || y1 <= y0) {
        continue;
      }
      const DkScissor tijera = {uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
      dkCmdBufSetScissors(cmd, 0, &tijera, 1);
      dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, color[0], color[1], color[2], color[3]);
      imagen.alto_usado = std::max(imagen.alto_usado, uint32_t(y1));
    }
    std::memcpy(imagen.borrado_color, color, sizeof(color));
  }
  ++borrados_rapidos_;
  rop_sin_barrera_ = true;
  OlvidarEstado();
  return true;
}

void RenderDk::MarcarBaldosas(const ImagenDk& imagen, uint32_t y0, uint32_t y1) {
  static const bool con_baldosas = REXCVAR_GET(deko_edram_baldosas);
  if (!con_baldosas) {
    return;
  }
  constexpr uint32_t kBaldosas = 2048;  // 10 MB of 5120-byte tiles
  const uint32_t por_fila = ((imagen.edram_pitch + 79) / 80) * (imagen.edram_64 ? 2 : 1);
  const uint32_t hasta = std::min((y1 + 15) / 16, (imagen.alto + 15) / 16);
  for (uint32_t k = y0 / 16; k < hasta; ++k) {
    for (uint32_t j = 0; j < por_fila; ++j) {
      dueno_baldosa_[(imagen.edram_base + k * por_fila + j) % kBaldosas] = imagen.clave_edram;
    }
  }
}

void RenderDk::TraerBaldosas(ImagenDk& imagen, uint32_t filas) {
  static const bool con_baldosas = REXCVAR_GET(deko_edram_baldosas);
  constexpr uint32_t kBaldosas = 2048;
  if (!con_baldosas || !imagen.clave_edram) {
    return;
  }
  const uint32_t ancho_baldosa = imagen.edram_64 ? 40 : 80;
  const uint32_t por_fila = ((imagen.edram_pitch + 79) / 80) * (imagen.edram_64 ? 2 : 1);
  const uint32_t filas_baldosas = std::min((filas + 15) / 16, (imagen.alto + 15) / 16);
  // A run: consecutive tiles of one tile row that come from consecutive tiles of one row of the same other target.
  ImagenDk* origen = nullptr;
  uint32_t desde_j = 0, origen_j = 0, origen_k = 0, cuantas = 0, fila = 0;
  bool traidas = false;
  const auto copiar = [&]() {
    if (!origen || !cuantas) {
      return;
    }
    const uint32_t xo = origen_j * ancho_baldosa, yo = origen_k * 16;
    const uint32_t xd = desde_j * ancho_baldosa, yd = fila * 16;
    if (xo < origen->ancho && yo < origen->alto && xd < imagen.ancho && yd < imagen.alto) {
      const uint32_t w = std::min({cuantas * ancho_baldosa, origen->ancho - xo, imagen.ancho - xd});
      const uint32_t h = std::min({16u, origen->alto - yo, imagen.alto - yd});
      const bool origen_flotante = origen->formato == DkImageFormat_RGBA16_Float;
      const bool destino_flotante = imagen.formato == DkImageFormat_RGBA16_Float;
      int32_t modo = kCopiaValor;
      if (origen_flotante && !destino_flotante && (origen->formato_dibujo == 3 || origen->formato_dibujo == 12)) {
        modo = kCopia7e3A8888;
      } else if (!origen_flotante && destino_flotante &&
                 (imagen.formato_dibujo == 3 || imagen.formato_dibujo == 12)) {
        modo = kCopia8888A7e3;
      }
      HacerBorradoPendiente(*origen);
      if (Copiar3D(*origen, xo, yo, w, h, imagen, xd, yd, modo, 1.0f)) {
        imagen.alto_usado = std::max(imagen.alto_usado, yd + h);
        ++baldosas_traidas_;
        traidas = true;
      }
    }
    for (uint32_t j = desde_j; j < desde_j + cuantas; ++j) {
      dueno_baldosa_[(imagen.edram_base + fila * por_fila + j) % kBaldosas] = imagen.clave_edram;
    }
    origen = nullptr;
    cuantas = 0;
  };
  for (fila = 0; fila < filas_baldosas; ++fila) {
    for (uint32_t j = 0; j < por_fila; ++j) {
      const uint32_t baldosa = (imagen.edram_base + fila * por_fila + j) % kBaldosas;
      const uint64_t dueno = dueno_baldosa_[baldosa];
      ImagenDk* otra = nullptr;
      uint32_t oj = 0, ok = 0;
      if (dueno && dueno != imagen.clave_edram) {
        auto it = destinos_.find(dueno);
        // The same base and pitch in the other family carries its content over itself (DestinoColor); a 64-bit
        // target's tiles hold other pixels than a 32-bit one's, which a copy of values cannot rearrange.
        if (it != destinos_.end() && it->second.viva && it->second.edram_64 == imagen.edram_64 &&
            !(it->second.edram_base == imagen.edram_base && it->second.edram_pitch == imagen.edram_pitch)) {
          const ImagenDk& o = it->second;
          const uint32_t por_fila_otra = ((o.edram_pitch + 79) / 80) * (o.edram_64 ? 2 : 1);
          const uint32_t d = (baldosa + kBaldosas - o.edram_base) % kBaldosas;
          if (d < por_fila_otra * ((o.alto + 15) / 16)) {
            otra = &it->second;
            oj = d % por_fila_otra;
            ok = d / por_fila_otra;
          }
        }
      }
      if (otra && otra == origen && ok == origen_k && oj == origen_j + cuantas && j == desde_j + cuantas) {
        ++cuantas;
        continue;
      }
      copiar();
      if (otra) {
        origen = otra;
        desde_j = j;
        origen_j = oj;
        origen_k = ok;
        cuantas = 1;
      }
    }
    copiar();
  }
  if (traidas) {
    OlvidarEstado();
  }
}

namespace {
constexpr uint32_t kPasoAncho = 288, kPasoAlto = 160, kPasosMaximo = 64;
}

void RenderDk::VolcarPaso(const ImagenDk& origen, uint32_t base) {
  static const int32_t capturas = REXCVAR_GET(deko_volcar_pasos);
  if (!capturas || origen.profundidad || pasos_usados_ >= kPasosMaximo) {
    return;
  }
  // The first capture frames of each kind: a menu's (the frame before drew under 1000 draws) and a level's.
  if (pasos_captura_ != volcando_captura_) {
    int32_t& hechas = dibujos_ultimo_fotograma_ >= 1000 ? pasos_juego_ : pasos_menu_;
    if (hechas >= capturas) {
      return;
    }
    pasos_captura_ = volcando_captura_;
    ++hechas;
  }
  constexpr uint32_t kBytes = kPasoAncho * kPasoAlto * 4;
  if (!bloque_pasos_) {
    bloque_pasos_ = nucleo_.CrearBloqueLectura(kPasosMaximo * kBytes, "volcado de pasos");
    if (!bloque_pasos_) return;
  }
  PasoVolcado p;
  if (!CrearImagen(p.imagen, DkImageType_2D, DkImageFormat_RGBA8_Unorm, kPasoAncho, kPasoAlto, 1, 1,
                   DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, "paso volcado")) {
    return;
  }
  p.offset = pasos_usados_++ * kBytes;
  p.base = base;
  p.ancho = origen.ancho;
  p.alto = origen.alto;
  p.numero = pasos_numero_++;
  p.captura = volcando_captura_;
  DkCmdBuf cmd = nucleo_.comandos();
  MarcarGpu(kGpuCopias);
  BarreraTransferencia();  // the resolve's writes, on either engine
  const DkImageView de = VistaCompleta(origen.imagen);
  const DkImageView a = VistaCompleta(p.imagen.imagen);
  const DkImageRect r_de = {0, 0, 0, origen.ancho, origen.alto, 1};
  const DkImageRect r_a = {0, 0, 0, kPasoAncho, kPasoAlto, 1};
  dkCmdBufBlitImage(cmd, &de, &r_de, &a, &r_a, DkBlitFlag_FilterLinear, 0);
  BarreraTransferencia();
  dkMemBlockFlushCpuCache(bloque_pasos_, p.offset, kBytes);
  const DkCopyBuf buf = {dkMemBlockGetGpuAddr(bloque_pasos_) + p.offset, kPasoAncho * 4, kBytes};
  dkCmdBufCopyImageToBuffer(cmd, &a, &r_a, &buf, 0);
  transferencia_sin_barrera_ = true;
  p.trabajo = trabajo_;
  if (volcando_captura_) {
    volcado_dibujos_.push_back(fmt::format("  p paso {} ({:08X} {}x{})", p.numero, base, p.ancho, p.alto));
  }
  pasos_.push_back(std::move(p));
}

void RenderDk::EscribirPasos() {
  constexpr uint32_t kBytes = kPasoAncho * kPasoAlto * 4;
  for (size_t i = 0; i < pasos_.size();) {
    PasoVolcado& p = pasos_[i];
    if ((nucleo_.TrabajoAbierto() && p.trabajo == trabajo_) || !nucleo_.TrabajoTerminado(p.trabajo)) {
      ++i;
      continue;
    }
    dkMemBlockFlushCpuCache(bloque_pasos_, p.offset, kBytes);
    const uint8_t* rgba = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(bloque_pasos_)) + p.offset;
    const std::filesystem::path ruta =
        rex::filesystem::GetExecutableFolder() / "capturas" /
        fmt::format("paso_c{}_{:02}_{:08X}_{}x{}.ppm", p.captura, p.numero, p.base, p.ancho, p.alto);
    if (FILE* f = std::fopen(ruta.string().c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", kPasoAncho, kPasoAlto);
      std::vector<uint8_t> rgb(kPasoAncho * kPasoAlto * 3);
      for (uint32_t t = 0; t < kPasoAncho * kPasoAlto; ++t) {
        std::memcpy(&rgb[t * 3], rgba + t * 4, 3);
      }
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
    }
    DestruirImagen(p.imagen);
    pasos_.erase(pasos_.begin() + i);
  }
}

void RenderDk::RestaurarEstencil(const ImagenDk& color, ImagenDk& profundidad, const DkScissor& rect) {
  if (!vs_salida_valido_ || !fs_estencil_valido_ || !rect.width || !rect.height) {
    return;
  }
  LeerMuestras(const_cast<ImagenDk&>(color));
  EscribirMuestras(profundidad);
  AsegurarTrabajo();
  TerminarPase();
  HacerBorradoPendiente(profundidad);  // a pending clear first: the stencil written here must survive it
  const uint32_t ranura = VistaDe(color, 0x688, 0x688);
  DkCmdBuf cmd = nucleo_.comandos();
  MarcarGpu(kGpuSubEstencil);
  // The colour was just drawn: its writes must reach the texture units.
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
  ++barreras_fragmentos_;
  DkImageView vista;
  dkImageViewDefaults(&vista, &profundidad.imagen);
  dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
  const DkViewport vp = {float(rect.x), float(rect.y), float(rect.width), float(rect.height), 0.0f, 1.0f};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &rect, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  cws.masks = 0;
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);  // stencil ops replace, compare always
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  ds.stencilTestEnable = true;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_, fs_estencil_exportado_valido_ ? &fs_estencil_exportado_ : &fs_estencil_};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(ranura, 0));
  if (fs_estencil_exportado_valido_) {
    // Every fragment, write mask 0xFF: the replace op writes the exported reference.
    dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, 0x00, 0xFF);
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  }
  // The clear pass (write mask 0xFF, reference 0, every fragment), then one pass per bit (write mask 1 << bit,
  // reference 0xFF, only the fragments whose byte has it).
  for (int32_t bit = -1; bit < 8 && !fs_estencil_exportado_valido_; ++bit) {
    const deko::Subida ubo = Reservar(256, DK_UNIFORM_BUF_ALIGNMENT);
    if (!ubo) {
      break;
    }
    const int32_t datos[4] = {bit, 0, 0, 0};
    std::memcpy(ubo.cpu, datos, sizeof(datos));
    dkCmdBufBindUniformBuffer(nucleo_.comandos(), DkStage_Fragment, 0, ubo.gpu, 256);
    const uint8_t mascara = bit < 0 ? 0xFF : uint8_t(1u << bit);
    dkCmdBufSetStencil(nucleo_.comandos(), DkFace_FrontAndBack, mascara, bit < 0 ? 0x00 : 0xFF, 0xFF);
    dkCmdBufDraw(nucleo_.comandos(), DkPrimitive_Triangles, 3, 1, 0, 0);
  }
  ++estenciles_restaurados_;
  invalidar_zcull_ = true;  // ZCULL culls by stencil too
  rop_sin_barrera_ = true;
  OlvidarEstado();
}

deko::Subida RenderDk::Reservar(uint32_t bytes, uint32_t alineacion) {
  AsegurarTrabajo();
  deko::Subida s = nucleo_.Reservar(bytes, alineacion);
  if (!s && bytes + alineacion <= (nucleo_.config().mb_subida << 20)) {
    CambiarDeRanura();
    s = nucleo_.Reservar(bytes, alineacion);
  }
  return s;
}

// ---- Images

bool RenderDk::CrearImagen(ImagenDk& imagen, DkImageType tipo, DkImageFormat formato, uint32_t ancho, uint32_t alto,
                           uint32_t capas, uint32_t niveles, uint32_t flags, const char* que, DkMsMode muestras) {
  if (!ancho || !alto || ancho > 16384 || alto > 16384) {
    REXLOG_WARN("[deko3d] {} de {}x{}: tamano imposible", que, ancho, alto);
    return false;
  }
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, nucleo_.dispositivo());
  m.type = tipo;
  m.flags = flags;
  m.format = formato;
  m.dimensions[0] = ancho;
  m.dimensions[1] = alto;
  m.dimensions[2] = tipo == DkImageType_Cubemap ? 6 : (tipo == DkImageType_3D ? capas : 1);
  m.mipLevels = std::max(niveles, 1u);
  m.msMode = muestras;
  const bool comprimido = formato >= DkImageFormat_RGB_BC1 && formato <= DkImageFormat_RG_BC5_Snorm;
  deko::ArreglarTamanoDeTesela(m, comprimido ? (alto + 3) / 4 : alto);
  DkImageLayout disposicion;
  dkImageLayoutInitialize(&disposicion, &m);
  const uint64_t bytes = dkImageLayoutGetSize(&disposicion);
  if (bytes >= (1ull << 31)) {
    return false;
  }
  imagen.memoria = nucleo_.ReservarImagen(uint32_t(bytes), dkImageLayoutGetAlignment(&disposicion));
  dkImageInitialize(&imagen.imagen, &disposicion, imagen.memoria.bloque, imagen.memoria.offset);
  imagen.tipo = tipo;
  imagen.formato = formato;
  imagen.ancho = ancho;
  imagen.alto = alto;
  imagen.capas = tipo == DkImageType_Cubemap ? 6 : capas;
  imagen.niveles = m.mipLevels;
  imagen.profundidad = formato == DkImageFormat_Z24S8 || formato == DkImageFormat_ZF32_X24S8;
  imagen.viva = true;
  imagen.serie = ++serie_imagenes_;
  return true;
}

void RenderDk::DestruirImagen(ImagenDk& imagen) {
  if (!imagen.viva) {
    return;
  }
  if (const auto ms = ms_de_.find(imagen.serie); ms != ms_de_.end()) {
    ImagenDk companera = ms->second;
    ms_de_.erase(ms);
    DestruirImagen(companera);
  }
  LiberarDescriptoresDe(imagen.serie);
  nucleo_.LiberarImagenLuego(imagen.memoria);
  imagen = ImagenDk{};
}

void RenderDk::PrepararDestino(ImagenDk& imagen) {
  EscribirMuestras(imagen);
  AsegurarTrabajo();
  MarcarGpu(kGpuSubPreparar);
  // The values as the game's clears write them: color 0, and depth 1.0 with stencil 0 as RB_DEPTH_CLEAR.
  borrado_previo_[imagen.serie] = {imagen.profundidad ? uint64_t(0xFFFFFF00u) : 0, dibujados_};
  TerminarPase();
  AntesDe3D();
  DkCmdBuf cmd = nucleo_.comandos();
  const DkImageView vista = VistaCompleta(imagen.imagen);
  const DkScissor tijera = {0, 0, imagen.ancho, imagen.alto};
  imagen.alto_borrado = imagen.alto;
  imagen.banda_sucia = false;
  imagen.borrado_z = 1.0f;
  imagen.borrado_estencil = 0;
  std::fill(std::begin(imagen.borrado_color), std::end(imagen.borrado_color), 0.0f);
  if (imagen.profundidad) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
  }
  OlvidarEstado();
}

// The image for a base in one format (one per base and family: 8-bit, float, and the 64-bit ones), created cleared.
ImagenDk* RenderDk::ImagenColor(uint32_t base, uint32_t formato, uint32_t pitch) {
  if (!pitch) {
    Rechazar(7, "destino de render con pitch 0");
    return nullptr;
  }
  // EDRAM is memory: a game draws into a base with one format and resolves it with another (Halo 3 draws 8_8_8_8_GAMMA
  // and copies as 8_8_8_8 out of the same tiles). Every format here is RGBA8 on the host, so the key only tells the
  // 32-bit formats from the 64-bit ones (16_16_16_16 and its float, 5 and 7), whose tiles hold half the pixels.
  const bool flotante = EsDestinoFlotante(formato);
  const uint32_t clase = ((formato == 5 || formato == 7) ? 1u : 0u) | (flotante ? 2u : 0u);
  const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(clase) << 16) | pitch;
  if (auto it = destinos_.find(clave); it != destinos_.end()) {
    return &it->second;
  }
  // The height from the pitch, as the Vulkan path (nfsmw_nativo_destinos.cpp, ObtenerDestino explains why).
  const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  ImagenDk imagen;
  uint32_t flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  if (REXCVAR_GET(nfsmw_dk_compresion_color)) {
    flags |= DkImageFlags_HwCompression;
  }
  if (!CrearImagen(imagen, DkImageType_2D, flotante ? DkImageFormat_RGBA16_Float : DkImageFormat_RGBA8_Unorm, pitch, alto,
                   1, 1, flags, "destino de color")) {
    Rechazar(8, "no se pudo crear un destino de render");
    return nullptr;
  }
  REXLOG_INFO("[deko3d] destino de render base {:03X}, formato {}, {}x{}", base, formato, pitch, alto);
  imagen.edram_base = base;
  imagen.edram_pitch = pitch;
  imagen.edram_64 = (clase & 1u) != 0;
  imagen.clave_edram = clave;
  ImagenDk& nueva = destinos_.emplace(clave, imagen).first->second;
  PrepararDestino(nueva);
  return &nueva;
}

// With deko_destinos_flotantes on, the 2_10_10_10 family (2, 3 = 7e3, 10, 12 = 7e3 as 16_16_16_16) is float as well:
// a 7e3 drawing keeps its [0, 32) range (Halo 3's tone-mapped frame). Its 8-bit formats stay 8-bit.
bool RenderDk::EsDestinoFlotante(uint32_t formato) const {
  static const bool con_flotantes = REXCVAR_GET(deko_destinos_flotantes);
  return con_flotantes &&
         (DestinoFlotante(formato) || formato == 2 || formato == 3 || formato == 10 || formato == 12);
}

// A base's colour target for a draw (para_copia false) or a copy (true). A 32-bit base used in both families has two
// images (Halo 3's 0x4C0: 7e3 drawings, 2_10_10_10 and 8_8_8_8 ones, copies as 10, 0 and 12): a draw in the other
// family first carries the content over (LlevarContenido) and takes the base; a copy reads whichever image holds it.
ImagenDk* RenderDk::DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch, bool para_copia) {
  static const bool con_flotantes = REXCVAR_GET(deko_destinos_flotantes);
  const bool de_64 = formato == 5 || formato == 7;
  if (!con_flotantes || de_64 || !pitch) {
    return ImagenColor(base, formato, pitch);
  }
  const uint64_t clave_base = (uint64_t(base) << 20) | pitch;
  ImagenDk* propietaria = nullptr;
  if (auto p = propietario_.find(clave_base); p != propietario_.end()) {
    if (auto o = destinos_.find(p->second); o != destinos_.end() && o->second.viva) {
      propietaria = &o->second;
    }
  }
  if (para_copia && propietaria) {
    return propietaria;
  }
  ImagenDk* imagen = ImagenColor(base, formato, pitch);
  if (!imagen) {
    return nullptr;
  }
  if (propietaria && propietaria != imagen && !para_copia) {
    LlevarContenido(*propietaria, *imagen, formato);
  }
  if (!para_copia || !propietaria) {
    const bool flotante = EsDestinoFlotante(formato);
    propietario_[clave_base] = (uint64_t(base) << 20) | (uint64_t(flotante ? 2u : 0u) << 16) | pitch;
  }
  return imagen;
}

// A 32-bit base switching between its 8-bit and float images: the content goes across as the EDRAM's bits would read
// in the new format (the H3Recomp PC port's CarryOver, as facts): a 7e3 drawing read as an 8-bit format is its 7e3
// bits; 7e3 bits read as 7e3 are their value again; anything else is the value. The rows the game has used.
void RenderDk::LlevarContenido(ImagenDk& origen, ImagenDk& destino, uint32_t formato_destino) {
  const bool origen_flotante = origen.formato == DkImageFormat_RGBA16_Float;
  const bool destino_flotante = destino.formato == DkImageFormat_RGBA16_Float;
  const bool origen_7e3 = origen.formato_dibujo == 3 || origen.formato_dibujo == 12;
  const bool destino_7e3 = formato_destino == 3 || formato_destino == 12;
  // Between the float image and the 8-bit one the 32-bit words are read in the other layout.
  int32_t modo = kCopiaValor;
  if (origen_flotante && !destino_flotante && origen_7e3) {
    modo = kCopia7e3A8888;
  } else if (!origen_flotante && destino_flotante && destino_7e3) {
    modo = kCopia8888A7e3;
  }
  HacerBorradoPendiente(origen);
  const uint32_t alto = std::min(origen.alto, std::max(origen.alto_usado, destino.alto_usado) ? std::max(origen.alto_usado, destino.alto_usado) : origen.alto);
  const uint32_t ancho = std::min(origen.ancho, destino.ancho);
  if (Copiar3D(origen, 0, 0, ancho, alto, destino, 0, 0, modo, 1.0f)) {
    destino.formato_dibujo = modo == kCopia8888A7e3 ? uint8_t(formato_destino) : origen.formato_dibujo;
    destino.alto_usado = std::max(destino.alto_usado, alto);
    destino.borrado_pendiente = false;
    ++contenidos_llevados_;
  }
}

ImagenDk* RenderDk::DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) {
  if (!pitch) {
    Rechazar(11, "sin destino de profundidad (pitch 0)");
    return nullptr;
  }
  const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
  if (auto it = profundidades_.find(clave); it != profundidades_.end()) {
    return &it->second;
  }
  const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  ImagenDk imagen;
  // Both guest depth formats (24-bit fixed and float) as D24S8, as the Vulkan path does on the console. No
  // compression: untested on deko3d, and copies broke it on nouveau (HowToDeko3d.md, section 5.10).
  static const uint32_t flags_z =
      DkImageFlags_UsageRender | (REXCVAR_GET(nfsmw_dk_compresion_profundidad) ? DkImageFlags_HwCompression : 0u);
  if (!CrearImagen(imagen, DkImageType_2D, DkImageFormat_Z24S8, pitch, alto, 1, 1, flags_z, "destino de profundidad")) {
    Rechazar(12, "no se pudo crear un destino de profundidad");
    return nullptr;
  }
  REXLOG_INFO("[deko3d] destino de profundidad base {:03X}, formato {}, {}x{}", base, formato, pitch, alto);
  ImagenDk& nueva = profundidades_.emplace(clave, imagen).first->second;
  PrepararDestino(nueva);
  return &nueva;
}

// Predicated tiling (Halo 3): the frame is drawn in bands of EDRAM and each band is resolved into its rows of one
// texture, with the destination base moved down to the band and the height left below it as the destination height
// (640, then 480, 320, 160). A base a whole number of 32-row tile rows below a texture resolved from its top, of the
// same width and kind, whose rows down plus that height make its height, is that texture at those rows: `base` becomes
// the texture's base and `filas` the rows down. nullptr if there is none.
RenderDk::Resuelta* RenderDk::BandaDe(uint32_t& base, uint32_t ancho, uint32_t alto_destino, uint32_t pitch,
                                      bool profundidad, uint32_t& filas) {
  static const bool bandas_profundidad = REXCVAR_GET(deko_bandas_profundidad);
  if (profundidad && !bandas_profundidad) {
    return nullptr;
  }
  const uint64_t fila_baldosas = uint64_t((pitch + 31) / 32) * 4096;  // 32 rows of 4-byte texels
  for (auto& [b, r] : resueltas_) {
    if (b >= base || r.imagen.profundidad != profundidad || r.imagen.ancho != ancho) {
      continue;
    }
    const uint64_t d = base - b;
    if (d % fila_baldosas == 0 && (d / fila_baldosas) * 32 + alto_destino == r.imagen.alto) {
      filas = uint32_t(d / fila_baldosas) * 32;
      base = b;
      ++bandas_resueltas_;
      return &r;
    }
  }
  return nullptr;
}

RenderDk::Resuelta* RenderDk::ObtenerResuelta(uint32_t base, uint32_t ancho, uint32_t alto, uint32_t formato,
                                              bool intercambio_rb, bool profundidad) {
  if (!ancho || !alto) {
    Rechazar(9, "copia a una textura de tamano 0");
    return nullptr;
  }
  static const bool con_flotantes = REXCVAR_GET(deko_destinos_flotantes);
  const DkImageFormat formato_host = profundidad                                ? DkImageFormat_Z24S8
                                     : con_flotantes && CopiaFlotante(formato) ? DkImageFormat_RGBA16_Float
                                                                                : DkImageFormat_RGBA8_Unorm;
  if (auto it = resueltas_.find(base); it != resueltas_.end()) {
    Resuelta& r = it->second;
    if (r.imagen.ancho == ancho && r.imagen.alto == alto && r.imagen.formato == formato_host) {
      if (r.formato_guest != formato || r.imagen.intercambio_rb != intercambio_rb) {
        ++generacion_texturas_;  // changes the swizzle it is sampled with
      }
      r.formato_guest = formato;
      r.imagen.intercambio_rb = intercambio_rb;
      return &r;
    }
    // Another size or format at the same address: the current one is kept aside (resueltas_otras_), and one of the
    // new size kept from before comes back, so the two a frame alternates are not recreated every time.
    if (frontal_.pendiente && frontal_.resuelta == &r) {
      frontal_.pendiente = false;  // its texture moves: nothing to copy into
    }
    const bool era_profundidad = r.imagen.formato == DkImageFormat_Z24S8;
    const uint64_t aparte = ClaveOtra(base, r.imagen.ancho, r.imagen.alto, era_profundidad);
    if (auto viejo = resueltas_otras_.find(aparte); viejo != resueltas_otras_.end()) {
      DestruirImagen(viejo->second.imagen);  // its memory is only reused after the fence of this slot
      resueltas_otras_.erase(viejo);
    }
    resueltas_otras_.emplace(aparte, std::move(r));
    resueltas_.erase(it);
    SombraMinimoResueltaEscrita(base);
    if (auto vuelta = resueltas_otras_.find(ClaveOtra(base, ancho, alto, profundidad));
        vuelta != resueltas_otras_.end() && vuelta->second.imagen.formato == formato_host) {
      Resuelta& de_vuelta = resueltas_.emplace(base, std::move(vuelta->second)).first->second;
      resueltas_otras_.erase(vuelta);
      if (de_vuelta.formato_guest != formato || de_vuelta.imagen.intercambio_rb != intercambio_rb) {
        ++generacion_texturas_;
      }
      de_vuelta.formato_guest = formato;
      de_vuelta.imagen.intercambio_rb = intercambio_rb;
      ++generacion_texturas_;  // that address is sampled from this one again
      return &de_vuelta;
    }
  }
  Resuelta r;
  // A depth texture has the depth targets' flags (resolves without copy swap their images).
  const uint32_t flags =
      profundidad ? DkImageFlags_UsageRender |
                        (REXCVAR_GET(nfsmw_dk_compresion_profundidad) ? DkImageFlags_HwCompression : 0u)
                  : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  if (!CrearImagen(r.imagen, DkImageType_2D, formato_host, ancho, alto, 1, 1, flags, "textura resuelta")) {
    Rechazar(10, "no se pudo crear una textura resuelta");
    return nullptr;
  }
  r.formato_guest = formato;
  r.imagen.intercambio_rb = intercambio_rb;
  REXLOG_INFO("[deko3d] textura resuelta en {:08X}, {}x{}, formato {}{}", base, ancho, alto, formato,
              profundidad ? " (profundidad)" : "");
  ++generacion_texturas_;  // that address is now sampled from the resolved texture
  return &resueltas_.emplace(base, std::move(r)).first->second;
}

const ImagenDk* RenderDk::TexturaResuelta(uint32_t direccion) {
  if (frontal_.pendiente && direccion == frontal_.base) {
    MaterializarFrontal();  // a draw samples the front buffer: it gets the copy now
  } else if (frontal_obsoleta_ && direccion == frontal_obsoleta_) {
    // Painted from its target and sampled afterwards: the texture holds an older frame. Copies from now on.
    frontal_obsoleta_ = 0;
    if (!frontal_apagado_) {
      frontal_apagado_ = true;
      REXLOG_WARN("[deko3d] frontal perezoso: un dibujo muestrea el frontal {:08X} despues del Swap; se copia siempre "
                  "a partir de ahora",
                  direccion);
    }
  }
  const auto it = resueltas_.find(direccion);
  if (it == resueltas_.end() || !it->second.escrita) {
    return nullptr;
  }
  return &it->second.imagen;
}

const ImagenDk* RenderDk::TexturaResuelta(uint32_t direccion, uint32_t ancho, uint32_t alto) {
  const ImagenDk* actual = TexturaResuelta(direccion);
  if (resueltas_otras_.empty() || (actual && actual->ancho == ancho && actual->alto == alto)) {
    return actual;
  }
  // The texture's size may be its pitch-aligned width: the smallest kept image that covers it, of those written.
  const ImagenDk* mejor = nullptr;
  auto considerar = [&](const ImagenDk& i) {
    if (i.ancho >= ancho && i.alto >= alto && (!mejor || uint64_t(i.ancho) * i.alto < uint64_t(mejor->ancho) * mejor->alto)) {
      mejor = &i;
    }
  };
  if (actual) {
    considerar(*actual);
  }
  for (const auto& [clave, r] : resueltas_otras_) {
    if (uint32_t(clave) == direccion && r.escrita && r.imagen.formato != DkImageFormat_Z24S8) {
      considerar(r.imagen);
    }
  }
  return mejor ? mejor : actual;
}

// ---- Copies (resolve and clear)

bool RenderDk::Rechazar(uint32_t causa, const char* texto) {
  ++rechazados_;
  ++causas_[causa];
  Avisar(causa, texto);
  return false;
}

void RenderDk::Avisar(uint32_t causa, const char* texto) {
  if (avisados_.emplace(causa, true).second) {
    REXLOG_WARN("[deko3d] {} (causa {})", texto, causa);
  }
}

// The copy rectangle, as the Vulkan path computes it (nfsmw_nativo_destinos.cpp, Rectangulo).
bool RenderDk::Rectangulo(const RegistrosCopia& reg, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1,
                          int32_t& y1) {
  const uint32_t tipo = reg.fetch_vertices[0] & 0x3;
  const uint32_t direccion = reg.fetch_vertices[0] >> 2;
  const auto orden = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
  const uint32_t tamano = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
  if (tipo != uint32_t(xenos::FetchConstantType::kVertex) || tamano != 3 * 2) {
    return Rechazar(5, "vertices de la copia en un formato no soportado");
  }
  const uint8_t* vertices = memoria_->TranslatePhysical(direccion * 4);
  const float medio_pixel = (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
  int32_t fijo[6];
  for (int i = 0; i < 6; ++i) {
    float valor;
    std::memcpy(&valor, vertices + i * 4, sizeof(valor));
    fijo[i] = Fijo16p8(xenos::GpuSwap(valor, orden) + medio_pixel);
  }
  x0 = (std::min({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y0 = (std::min({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
  x1 = (std::max({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y1 = (std::max({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
  const int32_t desplazamiento_x = ExtenderSigno15(reg.pa_sc_window_offset & 0x7FFF);
  const int32_t desplazamiento_y = ExtenderSigno15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
  if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
    x0 += desplazamiento_x;
    y0 += desplazamiento_y;
    x1 += desplazamiento_x;
    y1 += desplazamiento_y;
  }
  int32_t izquierda = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
  int32_t arriba = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
  int32_t derecha = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
  int32_t abajo = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
  if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
    izquierda += desplazamiento_x;
    arriba += desplazamiento_y;
    derecha += desplazamiento_x;
    abajo += desplazamiento_y;
  }
  izquierda = std::max(izquierda, 0);
  arriba = std::max(arriba, 0);
  derecha = std::max(derecha, izquierda);
  abajo = std::max(abajo, arriba);
  x0 = std::clamp(x0, izquierda, derecha);
  y0 = std::clamp(y0, arriba, abajo);
  x1 = std::clamp(x1, izquierda, derecha);
  y1 = std::clamp(y1, arriba, abajo);
  x0 &= ~int32_t(7);  // D3D9 aligns to 8 (kResolveAlignmentPixels)
  y0 &= ~int32_t(7);
  x1 = (x1 + 7) & ~int32_t(7);
  y1 = (y1 + 7) & ~int32_t(7);
  const int32_t pitch_alineado = int32_t(pitch & ~uint32_t(7));
  x0 = std::min(x0, pitch_alineado);
  x1 = std::min(x1, pitch_alineado);
  if (x0 >= x1 || y0 >= y1) {
    return Rechazar(6, "rectangulo de copia vacio");
  }
  return true;
}

bool RenderDk::Copiar(const RegistrosCopia& reg) {
  AsegurarTrabajo();
  TerminarPase();  // a resolve reads the pass's target: the deferred sky goes in first
  const uint32_t control = reg.rb_copy_control;
  const uint32_t origen = control & 0x7;
  bool borrar_color = (control >> 8) & 0x1;
  const uint32_t comando = (control >> 20) & 0x3;
  const bool copiar =
      comando == uint32_t(xenos::CopyCommand::kRaw) || comando == uint32_t(xenos::CopyCommand::kConvert);
  const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
  if (origen >= xenos::kMaxColorRenderTargets && volcando_captura_) {
    volcado_dibujos_.push_back(fmt::format("  c copia de profundidad {:03X} pitch {} -> {:08X} | copiar {} borrar z {} valor {:08X}",
                                           reg.rb_depth_info & 0xFFF, pitch, reg.rb_copy_dest_base, comando,
                                           (control >> 9) & 1, reg.rb_depth_clear));
  }
  if (origen >= xenos::kMaxColorRenderTargets) {
    // From depth (IsClearingColor: a depth source clears no color).
    if (copiar) {
      CopiarProfundidad(reg, pitch, (control >> 9) & 0x1);
    }
    if ((control >> 9) & 0x1) {
      BorrarProfundidad(reg, pitch);
    }
    return true;
  }
  const uint32_t info_color = reg.rb_color_info[origen];
  const uint32_t formato_color = (info_color >> 16) & 0xF;
  if (volcando_captura_) {  // the capture's draw list also shows the copies, in order
    volcado_dibujos_.push_back(fmt::format(
        "  c copia de {:03X}/f{} pitch {} -> {:08X} f{} pitch {} sesgo {} | copiar {} borrar color {} z {} valor {:08X}",
        info_color & 0xFFF, formato_color, pitch, reg.rb_copy_dest_base, (reg.rb_copy_dest_info >> 7) & 0x3F,
        reg.rb_copy_dest_pitch & 0x3FFF, int32_t(reg.rb_copy_dest_info << 10) >> 26, comando, (control >> 8) & 1,
        (control >> 9) & 1, reg.rb_color_clear));
    if (volcado_dibujos_.size() > 2500) {
      volcado_dibujos_.pop_front();
    }
  }
  if (!DestinoComoRgba8(formato_color)) {
    return Rechazar(100 + formato_color, "formato de destino de render todavia no soportado");
  }
  int32_t x0, y0, x1, y1;
  if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
    return false;
  }
  // A 4x MSAA surface's pixels are 2x2 samples of the image at twice the pitch (Dibujar): the rectangle is in pixels.
  const uint32_t muestras = ((reg.rb_surface_info >> 16) & 0x3) == 2 ? 2 : 1;
  ImagenDk* destino_render = DestinoColor(info_color & 0xFFF, formato_color, pitch * muestras, true);
  if (destino_render) {
    LeerMuestras(*destino_render);  // what a 4x pass drew there, back in sample space for the resolve
  }
  if (!destino_render) {
    return false;
  }
  TraerBaldosas(*destino_render, uint32_t(std::max(y1, 0)) * muestras);  // what other targets wrote over its tiles
  DkCmdBuf cmd = nucleo_.comandos();
  if (copiar) {
    const uint32_t info_destino = reg.rb_copy_dest_info;
    const uint32_t formato_destino = (info_destino >> 7) & 0x3F;
    Resuelta* resuelta = nullptr;
    uint32_t dx = 0, dy = 0;
    uint32_t base_resuelta = 0;
    if ((info_destino >> 3) & 0x1) {
      Rechazar(3, "copia a textura 3D o array: todavia no");
    } else if (!CopiaComoRgba8(formato_destino)) {
      Rechazar(300 + formato_destino, "formato de copia todavia no soportado");
    } else {
      const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
      const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
      // 4 bytes per texel: the base in multiples of 32 texels (GetResolveInfo).
      const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
      const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
      const uint32_t base = reg.rb_copy_dest_base + uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y),
                                                                                     pitch_destino, 2));
      dx = uint32_t(x0) - base_x;
      dy = uint32_t(y0) - base_y;
      base_resuelta = base & 0x1FFFFFFF;
      // The copy destination gives the texture's pitch (a multiple of 32 texels) and its real height, not its width.
      // A resolve from the left edge that is narrower than the pitch by less than 32 texels is the whole texture and
      // the rest is padding: the texture is created at that width, or it is sampled across the padding too (with
      // nfsmw_resolucion_interna = escena_960x540 the 240-wide bloom lived in a 256-wide texture and the bloom,
      // shadows and reflections came out shifted to the left). At 1280x720 every resolved width is a multiple of 32.
      const uint32_t ancho_resolve = uint32_t(x1 - x0);
      const uint32_t ancho_textura =
          x0 == 0 && ancho_resolve < pitch_destino && ancho_resolve + 32 > pitch_destino ? ancho_resolve : pitch_destino;
      uint32_t filas_banda = 0;
      if ((resuelta = BandaDe(base_resuelta, ancho_textura, alto_destino, pitch_destino, false, filas_banda))) {
        dy += filas_banda;  // base_resuelta is now the texture's own
      }
      if (!resuelta) {
        resuelta = ObtenerResuelta(base & 0x1FFFFFFF, ancho_textura, alto_destino, formato_destino,
                                   (info_destino >> 24) & 0x1, false);
      }
    }
    if (resuelta) {
      const uint32_t pedido_ancho = std::min(uint32_t(x1 - x0), destino_render->ancho - uint32_t(x0));
      const uint32_t pedido_alto = std::min(uint32_t(y1 - y0), destino_render->alto - uint32_t(y0));
      const uint32_t cabe_ancho = resuelta->imagen.ancho > dx ? resuelta->imagen.ancho - dx : 0;
      const uint32_t cabe_alto = resuelta->imagen.alto > dy ? resuelta->imagen.alto - dy : 0;
      const uint32_t ancho = std::min(pedido_ancho, cabe_ancho);
      const uint32_t alto = std::min(pedido_alto, cabe_alto);
      // A scene larger than its resolved texture (an internal resolution above the front buffer's) is shrunk with
      // a linear blit, as the Vulkan path does; otherwise it is copied texel for texel.
      const bool encoger = cabe_ancho && cabe_alto &&
                           (pedido_ancho * 4 >= cabe_ancho * 5 || pedido_alto * 4 >= cabe_alto * 5);
      // nfsmw_nativo_frontal_perezoso: the whole scene to the front buffer of the last Swap, not read back.
      const int32_t maximo_lectura = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
      const bool se_lee = maximo_lectura > 0 && uint64_t(ancho) * alto <= uint64_t(maximo_lectura);
      // What the copy does to the values (Copiar3D): a 7e3 drawing read as 2_10_10_10 hands on its bits, and every color
      // copy scales by 2^RB_COPY_DEST_INFO.copy_dest_exp_bias (signed, bits 16-21; Halo 3 copies its 7e3 frame out
      // with -5). Either needs the 3D engine: a raw copy or the lazy front buffer would hand on the plain value.
      const int32_t sesgo = int32_t(reg.rb_copy_dest_info << 10) >> 26;
      const float escala = std::ldexp(1.0f, sesgo);
      const bool bits_7e3 = (formato_color == 2 || formato_color == 10) &&
                            (destino_render->formato_dibujo == 3 || destino_render->formato_dibujo == 12) &&
                            destino_render->formato == DkImageFormat_RGBA16_Float;
      const bool desde_7e3 = (formato_color == 3 || formato_color == 12) &&
                             destino_render->formato == DkImageFormat_RGBA8_Unorm;
      const int32_t modo_copia = bits_7e3 ? kCopia7e3 : desde_7e3 ? kCopia8888A7e3 : kCopiaValor;
      const bool transforma = bits_7e3 || desde_7e3 || sesgo != 0;
      static const bool frontal_perezoso = REXCVAR_GET(nfsmw_nativo_frontal_perezoso);
      const bool frontal = frontal_perezoso && !frontal_apagado_ && base_resuelta == ultimo_frontal_ && !encoger &&
                           !transforma &&
                           !se_lee && x0 == 0 && y0 == 0 && dx == 0 && dy == 0 && ancho == resuelta->imagen.ancho &&
                           alto == resuelta->imagen.alto;
      // What the resolve reads must be there: the useful area grows to it, a pending clear is done, the band too.
      if (y1 > 0) {
        destino_render->alto_usado =
            std::max(destino_render->alto_usado, std::min(uint32_t(y1), destino_render->alto));
      }
      HacerBorradoPendiente(*destino_render);
      CompletarBanda(*destino_render, uint32_t(std::max(y1, 0)) * muestras, false, 0);
      if (muestras > 1) {
        // From a 4x MSAA surface: each pixel's 2x2 samples averaged by a linear blit at exactly half size.
        const uint32_t fx0 = uint32_t(x0) * muestras, fy0 = uint32_t(y0) * muestras;
        const uint32_t w = fx0 < destino_render->ancho
                               ? std::min({uint32_t(x1 - x0), cabe_ancho, (destino_render->ancho - fx0) / muestras})
                               : 0;
        const uint32_t h = fy0 < destino_render->alto
                               ? std::min({uint32_t(y1 - y0), cabe_alto, (destino_render->alto - fy0) / muestras})
                               : 0;
        if (w && h) {
          destino_render->alto_usado = std::max(destino_render->alto_usado, fy0 + h * muestras);
          // On the 3D engine (a fragment barrier, not the copy engine's full one: 8 of them a menu frame), with the
          // copy's 7e3 handling and exponent bias as any other resolve. The linear blit only if that cannot be done.
          if (!Copiar3D(*destino_render, fx0, fy0, w, h, resuelta->imagen, dx, dy, modo_copia, escala, muestras)) {
            MarcarGpu(kGpuCopias);
            AntesDeTransferir();
            const DkImageView origen_vista = VistaCompleta(destino_render->imagen);
            const DkImageView destino_vista = VistaCompleta(resuelta->imagen.imagen);
            const DkImageRect a = {fx0, fy0, 0, w * muestras, h * muestras, 1};
            const DkImageRect b = {dx, dy, 0, w, h, 1};
            dkCmdBufBlitImage(cmd, &origen_vista, &a, &destino_vista, &b, DkBlitFlag_FilterLinear, 0);
            cmd = nucleo_.comandos();
          }
          resuelta->escrita = true;
          ++resuelta->imagen.version;
          SombraMinimoResueltaEscrita(base_resuelta);
          ++copias_;
          LeerResuelta(reg, resuelta->imagen, x0, y0, dx, dy, w, h);
          if (volcando_captura_) {
            VolcarPaso(resuelta->imagen, base_resuelta);
          }
          OlvidarEstado();
        }
      } else if (frontal && uint32_t(x0) < destino_render->ancho && ancho && alto) {
        if (frontal_.pendiente && frontal_.base != base_resuelta) {
          MaterializarFrontal();
        }
        frontal_ = {true, base_resuelta, destino_render, resuelta, ancho, alto};
        frontal_obsoleta_ = 0;
        resuelta->escrita = true;
        ++resuelta->imagen.version;
        SombraMinimoResueltaEscrita(base_resuelta);
        ++frontales_ahorrados_;
      } else if (uint32_t(x0) < destino_render->ancho && uint32_t(y0) < destino_render->alto && ancho && alto) {
        const DkImageView origen_vista = VistaCompleta(destino_render->imagen);
        const DkImageView destino_vista = VistaCompleta(resuelta->imagen.imagen);
        // On the 3D engine, except the shrink (a linear blit) and the small resolves the game reads back (their
        // read-back is a copy engine transfer: both go there, one after the other, with no barrier between).
        // Different host formats (a float target to an 8-bit texture or back) only on the 3D engine or the blit:
        // the copy engine moves bytes.
        const bool mismo_formato = destino_render->formato == resuelta->imagen.formato;
        if ((!transforma && encoger) ||
            !Copiar3D(*destino_render, uint32_t(x0), uint32_t(y0), ancho, alto, resuelta->imagen, dx, dy, modo_copia,
                      escala)) {
          MarcarGpu(kGpuCopias);
          AntesDeTransferir();
          if (!encoger && !mismo_formato) {
            Avisar(15, "copia entre formatos distintos sin el motor 3D: no se hace");
          } else if (encoger) {
            const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, pedido_ancho, pedido_alto, 1};
            const DkImageRect b = {dx, dy, 0, cabe_ancho, cabe_alto, 1};
            dkCmdBufBlitImage(cmd, &origen_vista, &a, &destino_vista, &b, DkBlitFlag_FilterLinear, 0);
          } else {
            const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, ancho, alto, 1};
            const DkImageRect b = {dx, dy, 0, ancho, alto, 1};
            dkCmdBufCopyImage(cmd, &origen_vista, &a, &destino_vista, &b, 0);
          }
        }
        resuelta->escrita = true;
        ++resuelta->imagen.version;
        SombraMinimoResueltaEscrita(base_resuelta);
        ++copias_;
        if (encoger) {
          LeerResuelta(reg, resuelta->imagen, x0, y0, dx, dy, cabe_ancho, cabe_alto);
        } else {
          LeerResuelta(reg, resuelta->imagen, x0, y0, dx, dy, ancho, alto);
        }
        if (volcando_captura_) {
          VolcarPaso(resuelta->imagen, base_resuelta);
        }
        OlvidarEstado();
      }
    }
  }
  // nfsmw_nativo_saltar_borrados_repetidos: the target already holds that value and nothing has been drawn since.
  static const bool saltar_repetidos = REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos);
  if (borrar_color && saltar_repetidos) {
    const uint64_t valor = uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
    const auto it = borrado_previo_.find(destino_render->serie);
    if (it != borrado_previo_.end() && it->second.dibujos == dibujados_ && it->second.valor == valor) {
      ++borrados_saltados_;
      borrar_color = false;
    }
  }
  if (borrar_color) {
    const uint64_t valor = uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
    borrado_previo_[destino_render->serie] = {valor, dibujados_};
    for (uint32_t j = 0; j < 4; ++j) {
      destino_render->borrado_color[j] = float((valor >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
    }
    // Done when the target is next bound (EmpezarPase), resolved or painted, not now: the front buffer may still be
    // painted from it, and a later clear makes this one unnecessary.
    destino_render->borrado_pendiente = true;
    ++borrados_;
    static const bool aplazar = REXCVAR_GET(nfsmw_dk_borrado_color_aplazado);
    if (aplazar) {
      ++borrados_aplazados_;
    } else {
      HacerBorradoPendiente(*destino_render);
    }
  }
  if ((control >> 9) & 0x1) {
    BorrarProfundidad(reg, pitch);
  }
  return true;
}

void RenderDk::BorrarProfundidad(const RegistrosCopia& reg, uint32_t pitch) {
  const uint32_t info = reg.rb_depth_info;
  const uint32_t muestras = ((reg.rb_surface_info >> 16) & 0x3) == 2 ? 2 : 1;  // 4x: the sample-space image
  ImagenDk* profundidad = DestinoProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch * muestras);
  if (!profundidad) {
    return;
  }
  SombraMinimoAntesDeBorrar(*profundidad);
  profundidad->contenido_invalido = false;  // cleared: what a swap took away is no longer needed
  static const bool saltar_repetidos = REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos);
  EstadoBorrado& previo = borrado_previo_[profundidad->serie];
  if (saltar_repetidos && previo.dibujos == dibujados_ && previo.valor == reg.rb_depth_clear) {
    ++borrados_saltados_;
    return;
  }
  previo = {reg.rb_depth_clear, dibujados_};
  ++borrados_profundidad_;
  profundidad->borrado_z = float(reg.rb_depth_clear >> 8) / 16777215.0f;
  profundidad->borrado_estencil = uint8_t(reg.rb_depth_clear & 0xFF);
  profundidad->borrado_pendiente = true;
  static const bool aplazar = REXCVAR_GET(nfsmw_dk_borrado_z_aplazado);
  if (aplazar) {
    ++borrados_aplazados_;  // done when the target is next bound (EmpezarPase), copied or swapped
    return;
  }
  HacerBorradoPendiente(*profundidad);
}

void RenderDk::HacerBorradoPendiente(ImagenDk& destino) {
  if (!destino.borrado_pendiente) {
    return;
  }
  AntesDeCambiarDestino(&destino);
  EscribirMuestras(destino);
  destino.borrado_pendiente = false;
  AsegurarTrabajo();
  TerminarPase();
  AntesDe3D();
  MarcarGpu(kGpuSubPendiente);
  const DkImageView vista = VistaCompleta(destino.imagen);
  if (destino.profundidad) {
    dkCmdBufBindRenderTargets(nucleo_.comandos(), nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(nucleo_.comandos(), &vista, nullptr);
  }
  BorrarFilas(destino, 0, 0, AltoDeBorrado(destino));
  OlvidarEstado();
}

uint32_t RenderDk::AltoDeBorrado(const ImagenDk& destino) const {
  static const bool area_util = REXCVAR_GET(nfsmw_dk_borrar_area_util);
  if (!area_util || !destino.alto_usado) {
    return destino.alto;
  }
  return std::min(destino.alto, (destino.alto_usado + 7) & ~7u);
}

void RenderDk::BorrarFilas(ImagenDk& destino, uint32_t i, uint32_t desde, uint32_t hasta) {
  if (hasta <= desde) {
    return;
  }
  DkCmdBuf cmd = nucleo_.comandos();
  // Bound through its 4x companion: the same rows there are half as many.
  const DkScissor tijera = destino.ms_ligada ? DkScissor{0, desde / 2, destino.ancho / 2, (hasta + 1) / 2 - desde / 2}
                                             : DkScissor{0, desde, destino.ancho, hasta - desde};
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);  // clears respect the scissor (HowToDeko3d.md, section 6, item 9)
  if (destino.profundidad) {
    // The clear also sets the ZCULL data of the bound depth (deko3d).
    dkCmdBufClearDepthStencil(cmd, true, destino.borrado_z, 0xFF, destino.borrado_estencil);
  } else {
    const float* c = destino.borrado_color;
    dkCmdBufClearColorFloat(cmd, i, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
  }
  destino.alto_borrado = desde == 0 ? hasta : std::max(destino.alto_borrado, hasta);
  destino.banda_sucia = destino.alto_borrado < destino.alto;
  if (destino.ms_ligada) {
    destino.ms_filas = std::max(destino.ms_filas, std::min(hasta, destino.alto));
  }
}

void RenderDk::CompletarBanda(ImagenDk& destino, uint32_t hasta, bool ligado, uint32_t i) {
  hasta = std::min(hasta, destino.alto);
  if (hasta > destino.alto_usado) {
    destino.alto_usado = hasta;
  }
  // A pending clear will cover the useful area as it is when it is done.
  if (!destino.banda_sucia || hasta <= destino.alto_borrado || destino.borrado_pendiente) {
    return;
  }
  const uint32_t desde = destino.alto_borrado;
  const uint32_t hasta_banda = std::max(AltoDeBorrado(destino), std::min(destino.alto, (hasta + 7) & ~7u));
  ++bandas_completadas_;
  if (ligado) {
    BorrarFilas(destino, i, desde, hasta_banda);  // the target is bound in the current pass
    g_.valido = false;  // the scissor is the clear's
    return;
  }
  AntesDeCambiarDestino(&destino);
  EscribirMuestras(destino);
  AsegurarTrabajo();
  TerminarPase();
  AntesDe3D();
  MarcarGpu(kGpuSubBanda);
  const DkImageView vista = VistaCompleta(destino.imagen);
  if (destino.profundidad) {
    dkCmdBufBindRenderTargets(nucleo_.comandos(), nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(nucleo_.comandos(), &vista, nullptr);
  }
  BorrarFilas(destino, 0, desde, hasta_banda);
  OlvidarEstado();
}

void RenderDk::MaterializarFrontal() {
  if (!frontal_.pendiente) {
    return;
  }
  frontal_.pendiente = false;
  ++frontales_copiados_;
  ImagenDk& destino = frontal_.resuelta->imagen;
  if (!Copiar3D(*frontal_.destino, 0, 0, frontal_.ancho, frontal_.alto, destino, 0, 0)) {
    AsegurarTrabajo();
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(frontal_.destino->imagen);
    const DkImageView destino_vista = VistaCompleta(destino.imagen);
    const DkImageRect rect = {0, 0, 0, frontal_.ancho, frontal_.alto, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &rect, &destino_vista, &rect, 0);
  }
}

void RenderDk::IntercambiarImagenes(ImagenDk& destino, ImagenDk& resuelta) {
  EscribirMuestras(destino);  // its image is about to be another: what its companion drew goes in first
  std::swap(destino.imagen, resuelta.imagen);
  std::swap(destino.memoria, resuelta.memoria);
  std::swap(destino.serie, resuelta.serie);  // the descriptors go with their image
  ++generacion_texturas_;  // the address now samples another image: the per-fetch cache looks again
  borrado_previo_.erase(destino.serie);  // the target holds another image: its last clear no longer holds
  OlvidarEstado();  // the bound target, if it was this one, is another image now
  invalidar_zcull_ |= destino.profundidad;
  ++intercambios_;
}

void RenderDk::RestaurarContenido(ImagenDk& destino, bool para_resolver) {
  if (!destino.contenido_invalido) {
    return;
  }
  destino.contenido_invalido = false;
  const auto it = resueltas_.find(destino.resuelta_base);
  if (it == resueltas_.end() || it->second.imagen.ancho != destino.ancho || it->second.imagen.alto != destino.alto ||
      it->second.imagen.formato != destino.formato) {
    return;  // unknown where it went: the target keeps what it has, as the Vulkan path
  }
  // nfsmw_nativo_sombra_minimo: the car pass of the shadow map, drawn over a cleared target instead.
  if (!para_resolver && SombraMinimoEnVezDeRestaurar(destino)) {
    return;
  }
  EscribirMuestras(destino);
  AsegurarTrabajo();
  TerminarPase();
  if (!Copiar3D(it->second.imagen, 0, 0, destino.ancho, destino.alto, destino, 0, 0)) {
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(it->second.imagen.imagen);
    const DkImageView destino_vista = VistaCompleta(destino.imagen);
    const DkImageRect rect = {0, 0, 0, destino.ancho, destino.alto, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &rect, &destino_vista, &rect, 0);
  }
  borrado_previo_.erase(destino.serie);
  invalidar_zcull_ |= destino.profundidad;
  destino.alto_borrado = destino.alto;  // all of it holds the texture's content now: no band left
  destino.banda_sucia = false;
  ++restauraciones_;
  OlvidarEstado();
}

// Depth to a resolved texture of the host depth format, which the draws sample as k_24_8: the Vulkan path's
// CopiarProfundidad without the scaled shadow map. A whole target resolved to a texture of its size and format (the
// 1600x1600 shadow maps) swaps images instead of copying (nfsmw_nativo_resolver_sin_copia), with or without the clear
// (nfsmw_nativo_intercambiar_sin_borrado); the target gets its content back if the game draws on it again first.
void RenderDk::CopiarProfundidad(const RegistrosCopia& reg, uint32_t pitch, bool borrara) {
  const uint32_t info_destino = reg.rb_copy_dest_info;
  if ((info_destino >> 3) & 0x1) {
    Rechazar(3, "copia a textura 3D o array: todavia no");
    return;
  }
  int32_t x0, y0, x1, y1;
  if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
    return;
  }
  const uint32_t info = reg.rb_depth_info;
  ++copias_profundidad_;
  // A 4x MSAA depth buffer is the image at twice the pitch (sample space, Dibujar); the rectangle is in pixels.
  const uint32_t muestras = ((reg.rb_surface_info >> 16) & 0x3) == 2 ? 2 : 1;
  ImagenDk* profundidad = DestinoProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch * muestras);
  if (!profundidad) {
    return;
  }
  const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
  const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
  const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
  const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
  const uint32_t base =
      (reg.rb_copy_dest_base + uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y), pitch_destino, 2))) &
      0x1FFFFFFF;
  const uint32_t dx = uint32_t(x0) - base_x;
  uint32_t dy = uint32_t(y0) - base_y;
  const uint32_t formato_textura = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
  // Its width as for color (the pitch is padded to 32 texels; see Copiar).
  const uint32_t ancho_resolve = uint32_t(x1 - x0);
  const uint32_t ancho_textura =
      x0 == 0 && ancho_resolve < pitch_destino && ancho_resolve + 32 > pitch_destino ? ancho_resolve : pitch_destino;
  uint32_t base_textura = base, filas_banda = 0;
  Resuelta* resuelta = BandaDe(base_textura, ancho_textura, alto_destino, pitch_destino, true, filas_banda);
  if (resuelta) {
    dy += filas_banda;
  } else {
    resuelta = ObtenerResuelta(base, ancho_textura, alto_destino, formato_textura, false, true);
  }
  if (!resuelta) {
    return;
  }
  const uint32_t ancho_px = profundidad->ancho / muestras, alto_px = profundidad->alto / muestras;
  if (uint32_t(x0) >= ancho_px || uint32_t(y0) >= alto_px || dx >= resuelta->imagen.ancho ||
      dy >= resuelta->imagen.alto) {
    return;
  }
  const uint32_t ancho = std::min({uint32_t(x1 - x0), ancho_px - uint32_t(x0), resuelta->imagen.ancho - dx});
  const uint32_t alto = std::min({uint32_t(y1 - y0), alto_px - uint32_t(y0), resuelta->imagen.alto - dy});
  if (!ancho || !alto) {
    return;
  }
  AsegurarTrabajo();
  TerminarPase();
  // What is resolved must be the target's real content: a swap may have taken it away, a clear may be pending, the
  // useful area grows to what is read.
  LeerMuestras(*profundidad);
  RestaurarContenido(*profundidad, true);
  if (y1 > 0) {
    profundidad->alto_usado =
        std::max(profundidad->alto_usado, std::min(uint32_t(y1) * muestras, profundidad->alto));
  }
  HacerBorradoPendiente(*profundidad);
  CompletarBanda(*profundidad, uint32_t(std::max(y1, 0)) * muestras, false, 0);
  static const bool sin_copia = REXCVAR_GET(nfsmw_nativo_resolver_sin_copia);
  const bool entero = muestras == 1 && x0 == 0 && y0 == 0 && dx == 0 && dy == 0 && ancho == profundidad->ancho &&
                      alto == profundidad->alto && resuelta->imagen.ancho == profundidad->ancho &&
                      resuelta->imagen.alto == profundidad->alto && resuelta->imagen.formato == profundidad->formato;
  if (sin_copia && entero && (borrara || REXCVAR_GET(nfsmw_nativo_intercambiar_sin_borrado))) {
    IntercambiarImagenes(*profundidad, resuelta->imagen);
    profundidad->contenido_invalido = true;  // its content is in the texture now
    profundidad->resuelta_base = base;
    resuelta->escrita = true;
    ++resuelta->imagen.version;
    SombraMinimoResueltaEscrita(base);
    SombraMinimoTrasIntercambio(*profundidad, base, borrara);
    return;
  }
  SombraMinimoCopiaDesde(*profundidad);
  // On the 3D engine (gl_FragDepth): compressed depth must not go through the copy engine.
  if (!Copiar3D(*profundidad, uint32_t(x0), uint32_t(y0), ancho, alto, resuelta->imagen, dx, dy, kCopiaValor, 1.0f,
                muestras)) {
    if (muestras > 1) {
      return;  // the copy engine moves texels one to one
    }
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(profundidad->imagen);
    const DkImageView destino_vista = VistaCompleta(resuelta->imagen.imagen);
    const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, ancho, alto, 1};
    const DkImageRect b = {dx, dy, 0, ancho, alto, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &a, &destino_vista, &b, 0);
  }
  resuelta->escrita = true;
  ++resuelta->imagen.version;
  SombraMinimoResueltaEscrita(base);
  ++copias_;
  OlvidarEstado();
}

// ---- nfsmw_nativo_sombra_minimo (the Vulkan backend's, nfsmw_nativo_destinos.cpp, with the same guards)

void RenderDk::SombraMinimoDiferencia(const char* motivo, uint32_t dato) {
  ++sm_diferencias_;
  if (sm_fase_ != kSmApagada) {
    sm_fase_ = kSmApagada;
    REXLOG_ERROR("[deko3d] sombra por minimo: DIFERENCIA ({}; dato {:08X}). Se apaga para la sesion: el mapa de "
                 "sombras se vuelve a restaurar copiando",
                 motivo, dato);
  }
}

void RenderDk::SombraMinimoNoApto(const char* motivo, uint32_t dato) {
  if (sm_fase_ != kSmApagada) {
    sm_fase_ = kSmApagada;
    REXLOG_WARN("[deko3d] sombra por minimo: no se aplica en esta sesion ({}; dato {:08X})", motivo, dato);
  }
}

void RenderDk::SombraMinimoFalloCiclo(const char* motivo, uint32_t dato, bool permanente) {
  sm_ciclo_limpio_ = false;
  if (sm_ciclo_borrado_) {
    SombraMinimoDiferencia(motivo, dato);
  } else if (permanente) {
    SombraMinimoNoApto(motivo, dato);
  }
}

bool RenderDk::SombraMinimoEncendido() const {
  return REXCVAR_GET(nfsmw_nativo_sombra_minimo) && REXCVAR_GET(nfsmw_nativo_pcf_barato);
}

// After a whole depth target was resolved by swapping: without a clear it starts a cycle (the texture got the world),
// with a clear after the car pass it ends it (the texture got the cars).
void RenderDk::SombraMinimoTrasIntercambio(const ImagenDk& destino, uint32_t base, bool borrara) {
  if (destino.ancho != kLadoSombras || destino.alto != kLadoSombras) {
    return;
  }
  if (!borrara) {
    if (sm_estado_ == kSmCoches && sm_ciclo_borrado_) {
      SombraMinimoDiferencia("el destino con los coches solos se resuelve sin borrar", base);
    }
    if (sm_estado_ != kSmLibre) {
      sm_limpios_seguidos_ = 0;
    }
    sm_estado_ = kSmTrasMundo;
    sm_destino_ciclo_ = &destino;
    sm_mundo_ciclo_ = base;
    sm_ciclo_limpio_ = true;
    sm_ciclo_borrado_ = false;
    sm_mundo_reescrito_ = false;
    sm_encendido_ = SombraMinimoEncendido();
    return;
  }
  if (sm_estado_ != kSmCoches || &destino != sm_destino_ciclo_) {
    if (sm_estado_ != kSmLibre) {
      sm_limpios_seguidos_ = 0;
    }
    sm_estado_ = kSmLibre;
    return;
  }
  sm_estado_ = kSmLibre;
  ++sm_ciclos_;
  if (sm_ciclo_borrado_) {
    if (base != sm_coches_) {
      SombraMinimoDiferencia("la resolucion de los coches va a otra textura", base);
      sm_coches_ = base;
    }
    sm_virtual_ = true;
    sm_virtual_valida_ = !sm_mundo_reescrito_;
    if (sm_mundo_reescrito_) {
      SombraMinimoDiferencia("la textura del mundo se volvio a escribir dentro del ciclo", sm_mundo_);
    }
    ++sm_ciclos_aplicados_;
    return;
  }
  if (base != sm_coches_ || sm_mundo_ciclo_ != sm_mundo_ || sm_destino_ciclo_ != sm_destino_) {
    if (sm_fase_ == kSmAplicando) {
      SombraMinimoNoApto("aparece un ciclo de mapa de sombras distinto del aprendido (el dato es su textura)", base);
      return;
    }
    sm_coches_ = base;
    sm_mundo_ = sm_mundo_ciclo_;
    sm_destino_ = sm_destino_ciclo_;
    sm_limpios_seguidos_ = 0;
  }
  sm_limpios_seguidos_ = (sm_ciclo_limpio_ && !sm_mundo_reescrito_) ? sm_limpios_seguidos_ + 1 : 0;
  if (sm_fase_ == kSmMirando && sm_encendido_ && sm_limpios_seguidos_ >= kSombraMinimoCiclos) {
    sm_fase_ = kSmAplicando;
    REXLOG_INFO("[deko3d] sombra por minimo: APLICANDO tras {} ciclos limpios seguidos (mundo en {:08X}, coches en "
                "{:08X}): el pase de los coches se dibuja sobre el destino borrado y no se copia",
                sm_limpios_seguidos_, sm_mundo_, sm_coches_);
  }
}

// From RestaurarContenido: the target lost its content in a swap and is about to be drawn on. While applying, the car
// pass gets a clear to 1.0 (done when the pass binds it) instead of the world copied back.
bool RenderDk::SombraMinimoEnVezDeRestaurar(ImagenDk& destino) {
  if (&destino != sm_destino_ciclo_ || sm_estado_ != kSmTrasMundo) {
    if (&destino == sm_destino_ciclo_ && sm_estado_ == kSmCoches) {
      SombraMinimoFalloCiclo("el destino de los coches se restaura otra vez", destino.resuelta_base, false);
    }
    return false;
  }
  if (destino.resuelta_base != sm_mundo_ciclo_) {
    SombraMinimoFalloCiclo("el destino se restaura desde otra textura", destino.resuelta_base, false);
    return false;
  }
  sm_estado_ = kSmCoches;
  if (sm_fase_ != kSmAplicando || !sm_encendido_ || &destino != sm_destino_ || sm_mundo_ciclo_ != sm_mundo_ ||
      !sm_coches_) {
    return false;
  }
  destino.borrado_z = 1.0f;
  destino.borrado_estencil = 0;
  destino.borrado_pendiente = true;  // EmpezarPase clears it right after binding it
  borrado_previo_.erase(destino.serie);  // not the game's clear: its next one cannot be skipped
  sm_ciclo_borrado_ = true;
  return true;
}

void RenderDk::SombraMinimoCopiaDesde(const ImagenDk& origen) {
  if (&origen == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
    SombraMinimoFalloCiclo("se copia del destino del mapa de sombras a mitad del ciclo", 0, false);
  }
}

void RenderDk::SombraMinimoAntesDeBorrar(const ImagenDk& destino) {
  if (&destino == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
    sm_limpios_seguidos_ = 0;
    sm_estado_ = kSmLibre;
  }
}

void RenderDk::SombraMinimoResueltaEscrita(uint32_t base) {
  if (sm_virtual_) {
    if (base == sm_coches_) {
      sm_virtual_ = false;
    } else if (base == sm_mundo_) {
      sm_virtual_valida_ = false;
    }
  }
  if (sm_estado_ != kSmLibre && base == sm_mundo_ciclo_) {
    sm_mundo_reescrito_ = true;
  }
}

bool RenderDk::PaseDeCochesSombra(const ImagenDk* profundidad, bool solo_profundidad) {
  if (!profundidad || profundidad != sm_destino_ciclo_ || sm_estado_ != kSmCoches) {
    return false;
  }
  if (!solo_profundidad) {
    SombraMinimoFalloCiclo("el pase de los coches tiene destino de color", 0, true);
  }
  return true;
}

void RenderDk::DibujoDeCochesSombra(bool exacto, uint32_t control_z) {
  ++sm_dibujos_coches_;
  if (!exacto) {
    ++sm_dibujos_no_exactos_;
    SombraMinimoFalloCiclo("un dibujo de los coches no deja el minimo (el dato es su control de Z)", control_z, true);
  }
}

uint32_t RenderDk::DireccionSombraCoches() const {
  return (sm_virtual_ || (sm_encendido_ && sm_fase_ != kSmApagada)) ? sm_coches_ : 0;
}

const ImagenDk* RenderDk::CompaneraSombraCoches(bool capaz, uint32_t ps) {
  if (sm_virtual_) {
    const auto it = resueltas_.find(sm_mundo_);
    const bool mundo = it != resueltas_.end() && it->second.escrita;
    if (!sm_virtual_valida_ || !capaz || !mundo) {
      ++sm_lecturas_incapaces_;
      SombraMinimoDiferencia(!sm_virtual_valida_ ? "se muestrea la textura de los coches solos y la del mundo ya es de "
                                                   "otro ciclo (el dato es el pixel shader)"
                             : !capaz ? "un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches solos "
                                        "(el dato es su numero)"
                                      : "no esta la textura del mundo (el dato es el pixel shader)",
                             ps);
      return nullptr;
    }
    ++sm_lecturas_minimo_;
    return &it->second.imagen;
  }
  if (sm_estado_ != kSmLibre) {
    ++sm_lecturas_a_destiempo_;
    SombraMinimoNoApto("la textura de los coches se muestrea entre las dos resoluciones (el dato es el pixel shader)",
                       ps);
  }
  if (!capaz) {
    ++sm_lecturas_incapaces_;
    SombraMinimoNoApto("un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches (el dato es su numero)",
                       ps);
    return nullptr;
  }
  const auto it = resueltas_.find(sm_coches_);
  if (!sm_encendido_ || sm_fase_ == kSmApagada || it == resueltas_.end() || !it->second.escrita) {
    ++sm_lecturas_normales_;
    return nullptr;
  }
  ++sm_lecturas_si_misma_;  // the minimum with itself: the variants with the bit already exist when applying starts
  return &it->second.imagen;
}

// ---- Read-back of small resolved textures (the game's exposure)

void RenderDk::LeerResuelta(const RegistrosCopia& reg, const ImagenDk& imagen, int32_t x0, int32_t y0, uint32_t dx,
                            uint32_t dy, uint32_t ancho, uint32_t alto) {
  const int32_t maximo = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
  // RGBA8, and RGBA16F: Halo 3 measures its exposure in 16_16_16_16_FLOAT copies that its CPU reads back. Before
  // these were read back, the CPU read nothing (black scene) or, with 8-bit targets, an 8-bit layout where it
  // expected 8 bytes of half floats (a white scene).
  const bool flotante = imagen.formato == DkImageFormat_RGBA16_Float;
  if (maximo <= 0 || uint64_t(ancho) * alto > uint64_t(maximo) ||
      (imagen.formato != DkImageFormat_RGBA8_Unorm && !flotante)) {
    return;
  }
  const uint32_t bytes_texel = flotante ? 8 : 4;
  LecturasRanura& l = lecturas_[trabajo_ % kRanuras];
  const uint32_t bytes = ancho * alto * bytes_texel;
  if (l.usados + bytes > kBytesLecturaRanura) {
    if (lecturas_sin_sitio_++ == 0) {
      REXLOG_WARN("[deko3d] lecturas de vuelta: no caben mas en la ranura ({} KB); el juego vera la anterior",
                  kBytesLecturaRanura >> 10);
    }
    return;
  }
  const uint32_t offset = uint32_t(trabajo_ % kRanuras) * kBytesLecturaRanura + l.usados;
  l.usados += (bytes + 255) & ~255u;
  dkMemBlockFlushCpuCache(lectura_bloque_, offset, bytes);  // nothing of it stays in the CPU's cache
  const DkImageView vista = VistaCompleta(imagen.imagen);
  const DkImageRect rect = {dx, dy, 0, ancho, alto, 1};
  const DkCopyBuf destino = {lectura_gpu_ + offset, ancho * bytes_texel, ancho * bytes_texel * alto};
  // Into an image of its own on the 3D engine now (what the resolve wrote may be written again in this work), to the
  // buffer when the work closes (CerrarTrabajo), with the work's other read-backs.
  LecturaAplazada aplazada{};
  if (CrearImagen(aplazada.imagen, DkImageType_2D, imagen.formato, ancho, alto, 1, 1,
                  DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine, "lectura de vuelta") &&
      Copiar3D(imagen, dx, dy, ancho, alto, aplazada.imagen, 0, 0)) {
    aplazada.destino = destino;
    aplazada.ancho = ancho;
    aplazada.alto = alto;
    lecturas_aplazadas_.push_back(aplazada);
  } else {
    if (aplazada.imagen.viva) {
      DestruirImagen(aplazada.imagen);
    }
    // A transfer like the resolve: the copy engine runs them in order, and CerrarTrabajo waits for it before the fence.
    AntesDeTransferir();
    dkCmdBufCopyImageToBuffer(nucleo_.comandos(), &vista, &rect, &destino, 0);
  }
  l.pendientes.push_back({offset, reg.rb_copy_dest_base, x0, y0, ancho, alto, reg.rb_copy_dest_pitch & 0x3FFF,
                          (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info, bytes_texel});
  if (lecturas_hechas_++ == 0) {
    REXLOG_INFO("[deko3d] lectura de vuelta de texturas resueltas de hasta {} texels (la primera: {:08X}, {}x{})",
                maximo, reg.rb_copy_dest_base, ancho, alto);
  }
}

// IEEE half to float, for the diagnostics.
static float MedioAFlotante(uint16_t h) {
  const uint32_t signo = uint32_t(h >> 15) << 31, exponente = (h >> 10) & 0x1F, mantisa = h & 0x3FF;
  if (exponente == 0) {
    return (signo ? -1.0f : 1.0f) * std::ldexp(float(mantisa), -24);
  }
  const uint32_t bits = exponente == 31 ? signo | 0x7F800000u | (mantisa << 13)
                                        : signo | ((exponente + 112) << 23) | (mantisa << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Loaded with its fetch constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the
// resolved texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it. As the Vulkan path.
void RenderDk::EscribirLecturas(uint32_t ranura) {
  LecturasRanura& l = lecturas_[ranura];
  if (l.pendientes.empty()) {
    l.usados = 0;
    return;
  }
  uint8_t* const fisica = memoria_->TranslatePhysical(0);
  const auto& tabla = TablaMosaico2DTexel4();
  for (const LecturaDk& p : l.pendientes) {
    dkMemBlockFlushCpuCache(lectura_bloque_, p.offset, p.ancho * p.alto * p.bytes_texel);  // the GPU's, not the cache
    const uint8_t* const datos = lectura_cpu_ + p.offset;
    const bool intercambio = (p.info >> 24) & 0x1;
    const uint32_t orden_copia = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
    const auto orden = orden_copia <= 3 ? static_cast<xenos::Endian>(orden_copia) : xenos::Endian::k8in32;
    if (p.bytes_texel == 8) {
      // 16_16_16_16_FLOAT: R, G, B, A as 16-bit halves in the GPU's order (the host image's), red and blue swapped
      // with copy_dest_swap, then the copy's byte order on each 32-bit word, as the 8-bit path does with its marked
      // bytes; 8-byte texels tiled in 32x32 blocks.
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      for (uint32_t j = 0; j < p.alto; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.alto_destino && ty >= p.alto_destino) {
          break;
        }
        for (uint32_t i = 0; i < p.ancho; ++i) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t direccion =
              base + uint64_t(uint32_t(DesplazamientoMosaico2D(int32_t(tx), int32_t(ty), p.pitch, 3)));
          if (direccion + 8 > 0x20000000) {
            continue;
          }
          uint16_t h[4];
          std::memcpy(h, datos + (size_t(j) * p.ancho + i) * 8, 8);
          if (intercambio) {
            std::swap(h[0], h[2]);
          }
          // Halo 3's exposure: the 1-row float resolves its CPU reads back. Its first texel now and then.
          if (p.alto_destino == 1 && i == 0 && fotograma_ >= siguiente_aviso_exposicion_) {
            siguiente_aviso_exposicion_ = fotograma_ + 300;
            std::fprintf(stderr, "[exposicion] f%llu %08X %ux%u: %g %g %g %g\n",
                         static_cast<unsigned long long>(fotograma_), p.base, p.ancho, p.alto, MedioAFlotante(h[0]),
                         MedioAFlotante(h[1]), MedioAFlotante(h[2]), MedioAFlotante(h[3]));
            escribir_vista_exposicion_ = uint32_t(direccion);  // compared through the CPU's views once written
          }
          const uint32_t w[2] = {xenos::GpuSwap(uint32_t(h[0]) | (uint32_t(h[1]) << 16), orden),
                                 xenos::GpuSwap(uint32_t(h[2]) | (uint32_t(h[3]) << 16), orden)};
          std::memcpy(fisica + direccion, w, 8);
        }
      }
      // Diagnostic: the CPU reads physical memory through the 0xA/0xC/0xE views; if they do not alias the physical
      // base the GPU writes, the game never sees what the GPU wrote (its exposure loop runs to an extreme).
      if (escribir_vista_exposicion_) {
        const uint32_t d = escribir_vista_exposicion_;
        escribir_vista_exposicion_ = 0;
        const uint8_t* vistas[4] = {memoria_->TranslatePhysical(d), memoria_->TranslateVirtual(0xA0000000u | d),
                                    memoria_->TranslateVirtual(0xC0000000u | d),
                                    memoria_->TranslateVirtual(0xE0000000u | d)};
        uint64_t v[4];
        for (int k = 0; k < 4; ++k) std::memcpy(&v[k], vistas[k], 8);
        std::fprintf(stderr, "[vistas] %08X fisica %016llX A %016llX C %016llX E %016llX: %s\n", d,
                     (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                     (unsigned long long)v[3], (v[0] == v[1] && v[0] == v[2] && v[0] == v[3]) ? "iguales" : "DISTINTAS");
      }
      ++lecturas_escritas_;
      continue;
    }
    // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap), computed once with marked bytes.
    const uint32_t permutacion = xenos::GpuSwap(intercambio ? uint32_t(0x03000102) : uint32_t(0x03020100), orden);
    const uint8_t b0 = uint8_t(permutacion), b1 = uint8_t(permutacion >> 8), b2 = uint8_t(permutacion >> 16),
                  b3 = uint8_t(permutacion >> 24);
    const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
    const uint64_t baldosas_por_fila = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
    for (uint32_t j = 0; j < p.alto; ++j) {
      const uint32_t ty = uint32_t(p.y0) + j;
      if (p.alto_destino && ty >= p.alto_destino) {
        break;
      }
      const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
      const uint16_t* const local = tabla.data() + size_t(ty & 31) * 32;
      const uint8_t* s = datos + size_t(j) * p.ancho * 4;
      for (uint32_t i = 0; i < p.ancho; ++i, s += 4) {
        const uint32_t tx = uint32_t(p.x0) + i;
        if (tx >= p.pitch) {
          break;
        }
        const uint64_t direccion = fila + (uint64_t(tx >> 5) << 12) + local[tx & 31];
        if (direccion + 4 > 0x20000000) {
          continue;
        }
        const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
        uint8_t* const d = fisica + direccion;
        d[0] = t0;
        d[1] = t1;
        d[2] = t2;
        d[3] = t3;
      }
    }
    ++lecturas_escritas_;
  }
  l.pendientes.clear();
  l.usados = 0;
}

void RenderDk::EscribirLecturasTerminadas() {
  // Oldest work first: a later copy to the same address must be the one that stays.
  std::array<uint32_t, kRanuras> orden;
  for (uint32_t ranura = 0; ranura < kRanuras; ++ranura) {
    orden[ranura] = ranura;
  }
  std::sort(orden.begin(), orden.end(),
            [this](uint32_t a, uint32_t b) { return lecturas_[a].trabajo < lecturas_[b].trabajo; });
  for (const uint32_t ranura : orden) {
    LecturasRanura& l = lecturas_[ranura];
    if (l.pendientes.empty()) {
      continue;
    }
    // The open work's are not submitted yet: its slot's fence is still the previous use's.
    if ((nucleo_.TrabajoAbierto() && l.trabajo == trabajo_) || !nucleo_.TrabajoTerminado(l.trabajo)) {
      break;  // a newer one must not be written before this one
    }
    EscribirLecturas(ranura);
  }
}

// ---- Output

void RenderDk::RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) {
  rampa_juego_ = rampa;
  RecalcularRampa();
}

// The output table: the game's ramp followed by the menu's post-processing, exactly as the Vulkan path computes it
// (nfsmw_nativo_destinos.cpp, RecalcularRampa, where the order is explained).
void RenderDk::RecalcularRampa() {
  const rex::x360::PostProcess p =
      version_posproceso_ == UINT64_MAX ? rex::x360::PostProcess{} : hooks_.Posproceso();
  graduacion_ = p.activo && (p.saturacion != 1.0f || p.vibracion != 0.0f || p.vineta > 0.0f || p.lineas > 0.0f);
  const float tinte[3] = {p.tinte_r, p.tinte_g, p.tinte_b};
  const float inversa_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      float x = float(rampa_juego_[i][c] & 0x3FF) / 1023.0f;
      if (p.activo) {
        x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
        x += p.brillo;
        x = (x - 0.5f) * p.contraste + 0.5f;
        x = x + (x * tinte[c] - x) * p.tinte;
        if (!graduacion_) {
          x = std::clamp(std::pow(std::max(x, 0.0001f), inversa_gamma), 0.0f, 1.0f);
        }
      }
      rampa_valores_[i * 4 + c] = x;
    }
    rampa_valores_[i * 4 + 3] = 1.0f;
  }
  const size_t extra = 256 * 4;
  rampa_valores_[extra] = p.saturacion;
  rampa_valores_[extra + 1] = p.vibracion;
  rampa_valores_[extra + 2] = inversa_gamma;
  rampa_valores_[extra + 3] = 0.0f;
  rampa_valores_[extra + 4] = p.vineta;
  rampa_valores_[extra + 5] = p.lineas;
  rampa_valores_[extra + 6] = 0.0f;
  rampa_valores_[extra + 7] = 0.0f;
  rampa_subir_ = true;
}

bool RenderDk::CrearShadersSalida() {
  const deko::Compilador::Resultado vs = compilador_.Compilar(true, kVsSalida);
  vs_salida_valido_ = vs.ok && nucleo_.CargarShader(vs_salida_, vs.dksh, "salida (vertices)");
  if (!vs_salida_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar el vertex shader de la salida: {}", vs.log);
  }
  const deko::Compilador::Resultado color = compilador_.Compilar(false, kFsCopiaColor);
  fs_copia_color_valido_ = color.ok && nucleo_.CargarShader(fs_copia_color_, color.dksh, "copia de color");
  const deko::Compilador::Resultado z = compilador_.Compilar(false, kFsCopiaProfundidad);
  fs_copia_profundidad_valido_ = z.ok && nucleo_.CargarShader(fs_copia_profundidad_, z.dksh, "copia de profundidad");
  if (!fs_copia_color_valido_ || !fs_copia_profundidad_valido_) {
    REXLOG_ERROR("[deko3d] no se pudieron compilar las copias en el motor 3D ({} / {}): se copia con el motor de copias",
                 color.log, z.log);
  }
  const deko::Compilador::Resultado estencil = compilador_.Compilar(false, kFsEstencil);
  fs_estencil_valido_ = estencil.ok && nucleo_.CargarShader(fs_estencil_, estencil.dksh, "estencil desde color");
  if (!fs_estencil_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la restauracion del estencil: {}", estencil.log);
  }
  {
    const deko::Compilador::Resultado c = compilador_.Compilar(false, kFsMsColor);
    const deko::Compilador::Resultado z = compilador_.Compilar(false, kFsMsZ);
    const deko::Compilador::Resultado dc = compilador_.Compilar(false, kFsDesmsColor);
    const deko::Compilador::Resultado dz = compilador_.Compilar(false, kFsDesmsZ);
    fs_ms_validos_ = c.ok && z.ok && dc.ok && dz.ok && nucleo_.CargarShader(fs_ms_color_, c.dksh, "4x: empaquetar color") &&
                     nucleo_.CargarShader(fs_ms_z_, z.dksh, "4x: empaquetar profundidad") &&
                     nucleo_.CargarShader(fs_desms_color_, dc.dksh, "4x: desempaquetar color") &&
                     nucleo_.CargarShader(fs_desms_z_, dz.dksh, "4x: desempaquetar profundidad");
    if (!fs_ms_validos_) {
      REXLOG_WARN("[deko3d] 4x real no disponible ({} / {} / {} / {}): los pases 4x van en el espacio de muestras", c.log,
                  z.log, dc.log, dz.log);
    }
  }
  const deko::Compilador::Resultado exportado = compilador_.Compilar(false, kFsEstencilExportado);
  fs_estencil_exportado_valido_ =
      exportado.ok && nucleo_.CargarShader(fs_estencil_exportado_, exportado.dksh, "estencil exportado desde color");
  if (!fs_estencil_exportado_valido_) {
    REXLOG_WARN("[deko3d] sin estencil exportado ({}): la restauracion va bit a bit", exportado.log);
  }
  return vs_salida_valido_ && ShaderRampa(kRampaExacta) != nullptr;
}

const DkShader* RenderDk::ShaderRampa(uint32_t variante) {
  variante &= 7;
  if (rampas_[variante]) {
    return rampas_[variante].get();
  }
  if (rampas_intentadas_[variante]) {
    return nullptr;
  }
  rampas_intentadas_[variante] = true;
  std::string glsl = "#version 460\n";
  glsl += (variante & kRampaExacta) ? "#define EXACTA 1\n" : "#define EXACTA 0\n";
  glsl += (variante & kRampaGraduacion) ? "#define GRADUACION 1\n" : "#define GRADUACION 0\n";
  glsl += (variante & kRampaFxaa) ? "#define FXAA 1\n" : "#define FXAA 0\n";
  glsl += rampa_gamma_ ? "#define RAMPA 1\n" : "#define RAMPA 0\n";
  glsl += kFsRampa;
  const uint64_t inicio = deko::AhoraNs();
  const deko::Compilador::Resultado r = compilador_.Compilar(false, glsl);
  auto shader = std::make_unique<DkShader>();
  if (!r.ok || !nucleo_.CargarShader(*shader, r.dksh, "salida (rampa)")) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la variante {} del pixel shader de la salida: {}", variante, r.log);
    return nullptr;
  }
  REXLOG_INFO("[deko3d] salida: variante {} (exacta {}, graduacion {}, FXAA {}) compilada en {:.1f} ms", variante,
              (variante & kRampaExacta) != 0, (variante & kRampaGraduacion) != 0, (variante & kRampaFxaa) != 0,
              double(deko::AhoraNs() - inicio) / 1e6);
  rampas_[variante] = std::move(shader);
  return rampas_[variante].get();
}

bool RenderDk::PintarSalida(DkImage& salida, uint32_t ancho_salida, uint32_t alto_salida, const ImagenDk& origen,
                            uint32_t ancho, uint32_t alto) {
  AsegurarTrabajo();
  TerminarPase();
  AntesDe3D();
  MarcarGpu(kGpuOtros);  // the output pass
  // The menu's post-processing, if it changed (an atomic counter; the cvars are only read then).
  const uint64_t version = hooks_.VersionPosproceso();
  if (version != version_posproceso_) {
    version_posproceso_ = version;
    RecalcularRampa();
  }
  const bool exacta = ancho_salida == ancho && alto_salida == alto;
  const bool fxaa = hooks_.Fxaa();
  if (fxaa != fxaa_anotado_) {
    fxaa_anotado_ = fxaa;
    REXLOG_INFO("[deko3d] antialiasing en la salida: {}", fxaa ? "FXAA" : "ninguno");
  }
  const DkShader* fs = ShaderRampa((exacta ? kRampaExacta : 0) | (graduacion_ ? kRampaGraduacion : 0) |
                                   (fxaa ? kRampaFxaa : 0));
  if (!fs) {
    fs = ShaderRampa(kRampaExacta);
  }
  if (!fs || !vs_salida_valido_) {
    return false;
  }
  // The ramp's texture, created once and uploaded in the pre-list when it changes (the game's ramp, the menu).
  if (!rampa_tabla_.viva &&
      !CrearImagen(rampa_tabla_, DkImageType_2D, DkImageFormat_RGBA32_Float, 256, 1, 1, 1, 0, "tabla de la rampa")) {
    return false;
  }
  if (rampa_subir_) {
    const deko::Subida datos = Reservar(256 * 16, 256);
    if (!datos) {
      return false;
    }
    std::memcpy(datos.cpu, rampa_valores_.data(), 256 * 16);
    DkImageView vista_tabla;
    dkImageViewDefaults(&vista_tabla, &rampa_tabla_.imagen);
    const DkCopyBuf desde = {datos.gpu, 256 * 16, 256 * 16};
    const DkImageRect rect = {0, 0, 0, 256, 1, 1};
    dkCmdBufCopyBufferToImage(nucleo_.ComandosPrevios(), &desde, &vista_tabla, &rect, 0);
    rampa_subir_ = false;
  }
  const deko::Subida ubo = Reservar(kBytesUboRampa, DK_UNIFORM_BUF_ALIGNMENT);
  if (!ubo) {
    return false;
  }
  const int32_t desplazamiento[4] = {0, 0, 0, 0};
  const float inverso[4] = {1.0f / float(std::max(ancho, 1u)), 1.0f / float(std::max(alto, 1u)), 0.0f, 0.0f};
  std::memcpy(ubo.cpu, rampa_valores_.data() + 256 * 4, 8 * sizeof(float));  // mezcla, efectos
  std::memcpy(ubo.cpu + 32, desplazamiento, sizeof(desplazamiento));
  std::memcpy(ubo.cpu + 48, inverso, sizeof(inverso));
  const uint32_t ranura = VistaDe(origen, 0x688 /* RGBA */, 0x688);
  const uint32_t ranura_tabla = VistaDe(rampa_tabla_, 0x688, 0x688);
  DkCmdBuf cmd = nucleo_.comandos();
  // The presenter may still be sampling this image from its previous paint (rex/ui/deko3d/presentador.h).
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
  const DkImageView vista = VistaCompleta(salida);
  dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  const DkViewport vp = {0.0f, 0.0f, float(ancho_salida), float(alto_salida), 0.0f, 1.0f};
  const DkScissor tijera = {0, 0, ancho_salida, alto_salida};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_, fs};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, kBytesUboRampa);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(ranura, 0));
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle(ranura_tabla, 0));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  OlvidarEstado();
  return true;
}

bool RenderDk::Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho, uint32_t alto) {
  dibujos_ultimo_fotograma_ = dibujados_ - dibujados_al_swap_;
  dibujados_al_swap_ = dibujados_;
  RexSwitchPerfCount(0);  // the profiler's "juego N fps"
  const uint64_t ahora = deko::AhoraNs();
  if (ultimo_swap_ns_) {
    const double ms = double(ahora - ultimo_swap_ns_) / 1e6;
    const uint32_t cubeta = ms < 15.0    ? 0
                            : ms < 18.0  ? 1
                            : ms < 25.0  ? 2
                            : ms < 30.0  ? 3
                            : ms < 36.0  ? 4
                            : ms < 50.0  ? 5
                            : ms < 60.0  ? 6
                            : ms < 75.0  ? 7
                            : ms < 100.0 ? 8
                            : ms < 150.0 ? 9
                                         : 10;
    ++cubetas_swap_[cubeta];
    peor_swap_ms_ = std::max(peor_swap_ms_, ms);
    hooks_.Latido(ms);
  }
  ultimo_swap_ns_ = ahora;
  RecogerCompilaciones();
  AsegurarTrabajo();
  bool pintado = false;
  bool llamada = false;
  const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
  ultimo_frontal_ = base;  // nfsmw_nativo_frontal_perezoso: the next resolve here may go to the Swap directly
  // The front buffer is still in its render target (and the texture was not written): painted from there.
  bool desde_destino = frontal_.pendiente && frontal_.base == base;
  const ImagenDk* origen = desde_destino ? frontal_.destino : TexturaResuelta(base);
  if (!origen && !desde_destino) {
    // Fallback: the Swap's frontbuffer address matched no resolved texture. Some titles (e.g. Halo 3) present
    // from an address the Swap's fetch constant does not point at, so the exact lookup finds nothing and the
    // screen stays black even though a frame was resolved. Show the most-recently written resolved frame
    // instead. Titles whose frontbuffer base matches a resolve (NFS) find origen above and never reach this.
    uint32_t mejor_version = 0;
    for (auto& par : resueltas_) {
      if (par.second.escrita && par.second.imagen.version >= mejor_version) {
        mejor_version = par.second.imagen.version;
        origen = &par.second.imagen;
      }
    }
  }
  // deko_ver_resueltas: in turn every 4 s, the front buffer and each listed resolved texture (one step of the frame).
  {
    static const std::vector<uint32_t> lista = [] {
      std::vector<uint32_t> v;
      const std::string texto = REXCVAR_GET(deko_ver_resueltas);
      for (size_t i = 0; i < texto.size();) {
        size_t fin = texto.find(',', i);
        if (fin == std::string::npos) fin = texto.size();
        if (fin > i) v.push_back(uint32_t(std::strtoul(texto.substr(i, fin - i).c_str(), nullptr, 16)) & 0x1FFFFFFF);
        i = fin + 1;
      }
      return v;
    }();
    if (!lista.empty()) {
      static uint64_t paso_anterior = ~0ull;
      const uint64_t paso = (ahora / 4000000000ull) % (lista.size() + 1);
      const ImagenDk* vista = paso ? TexturaResuelta(lista[paso - 1]) : nullptr;
      if (paso != paso_anterior) {
        paso_anterior = paso;
        std::fprintf(stderr, "[ver] fotograma %llu: %s %08X%s\n", (unsigned long long)fotograma_,
                     paso ? "resuelta" : "frontal", paso ? lista[paso - 1] : base,
                     paso && !vista ? " (no escrita: se ve el frontal)" : "");
      }
      if (vista) {
        origen = vista;
        desde_destino = false;
      }
    }
  }
  const uint32_t ancho_origen = desde_destino ? frontal_.ancho : (origen ? origen->ancho : 0);
  const uint32_t alto_origen = desde_destino ? frontal_.alto : (origen ? origen->alto : 0);
  // DIAG (picture / "2 frames"): to stderr so it survives a crash in rex_stderr.log. Logs the swap's
  // frontbuffer, the chosen source, and every resolved texture, so we can see which of the two is the
  // real scene vs the empty/white one. First swaps only. No behavior change.
  {
    static int diag_pres = 0;
    // The first swaps, then one in 300 (in game the front buffer may not be the menu's).
    if (diag_pres < 48 || (fotograma_ % 300) == 0) {
      ++diag_pres;
      std::fprintf(stderr,
                   "[present] fotograma %llu swap base=%08X formato %u %ux%u want=%ux%u desde_destino=%d origen=%s %ux%u "
                   "| resueltas=%zu:",
                   (unsigned long long)fotograma_, base, swap.dword[1] & 0x3F, (swap.dword[2] & 0x1FFF) + 1,
                   ((swap.dword[2] >> 13) & 0x1FFF) + 1, ancho, alto, int(desde_destino),
                   origen ? (desde_destino ? "frontal" : "resuelta") : "NINGUNA", ancho_origen, alto_origen,
                   resueltas_.size());
      for (const auto& par : resueltas_) {
        std::fprintf(stderr, " [%08X %ux%u v%llu %c%s]", par.first, par.second.imagen.ancho,
                     par.second.imagen.alto, (unsigned long long)par.second.imagen.version,
                     par.second.escrita ? 'W' : '.', (origen == &par.second.imagen) ? "<=" : "");
      }
      std::fprintf(stderr,
                   " ultimo_frontal=%08X rechazados=%llu copias3d=%llu llevados=%llu bandas=%llu baldosas=%llu "
                   "borrados_rapidos=%llu causas:",
                   ultimo_frontal_, (unsigned long long)rechazados_, (unsigned long long)copias_3d_,
                   (unsigned long long)contenidos_llevados_, (unsigned long long)bandas_resueltas_,
                   (unsigned long long)baldosas_traidas_, (unsigned long long)borrados_rapidos_);
      for (const auto& c : causas_) {
        std::fprintf(stderr, " c%u=%llu", c.first, (unsigned long long)c.second);
      }
      std::fprintf(stderr, "\n");
    }
  }
  if (!origen) {
    Rechazar(14, "Swap sin textura resuelta");
  } else if (presentador) {
    const uint32_t w = std::min(ancho ? ancho : ancho_origen, ancho_origen);
    const uint32_t h = std::min(alto ? alto : alto_origen, alto_origen);
    presentador->RefreshGuestOutput(w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& c) {
      llamada = true;
      auto& contexto = static_cast<ContextoSalidaDeko3d&>(c);
      contexto.SetIs8bpc(true);
      const ImagenDk* fuente = origen;
      if (desde_destino) {
        // From the target only texel for texel at the same size (it is taller than the front buffer); otherwise, or
        // with FXAA, the copy is made now and painted as always.
        if (hooks_.Fxaa() || contexto.ancho() != w || contexto.alto() != h) {
          MaterializarFrontal();
          fuente = TexturaResuelta(base);
        } else {
          frontal_.pendiente = false;
          frontal_obsoleta_ = base;
        }
      }
      pintado = fuente && PintarSalida(contexto.imagen(), contexto.ancho(), contexto.alto(), *fuente, w, h);
      CerrarTrabajo();  // submitted before returning, as RefreshGuestOutput requires
      return pintado;
    });
  }
  if (!llamada) {
    CerrarTrabajo();
  }
  if (volcando_captura_) {
    std::fprintf(stderr, "[dibujos de la captura %d] fotograma %llu, ultimos %zu dibujos:\n", volcando_captura_,
                 static_cast<unsigned long long>(fotograma_), volcado_dibujos_.size());
    for (const std::string& linea : volcado_dibujos_) {
      std::fprintf(stderr, "%s\n", linea.c_str());
    }
    std::fflush(stderr);
    volcado_dibujos_.clear();
    volcando_captura_ = 0;
  }
  if (const int32_t pedida = g_volcar_dibujos_captura.exchange(0, std::memory_order_relaxed)) {
    volcando_captura_ = pedida;
  }
  // Frames in flight (deko_fotogramas_en_vuelo): the GPU has finished the frame before this one when this one is
  // closed, so the game records at most one frame ahead of the one the GPU draws. With three work slots it ran two
  // or three ahead: no faster when the GPU is the limit, only that many frames later to the screen (at 8 fps, a
  // third of a second from the stick to the picture).
  static const int32_t en_vuelo = REXCVAR_GET(deko_fotogramas_en_vuelo);
  if (en_vuelo > 0) {
    fin_fotogramas_[fotograma_ % 4] = trabajo_;
    if (fotograma_ >= uint64_t(en_vuelo)) {
      nucleo_.EsperarTrabajo(fin_fotogramas_[(fotograma_ - uint64_t(en_vuelo)) % 4], trabajo_);
    }
  }
  // The memory exports of the works the GPU has done: in guest memory before the game's next frame reads them.
  for (uint32_t i = 0; i < kRanuras; ++i) {
    const ExportRanura& er = export_ranuras_[i];
    if (!er.regiones.empty() && er.trabajo != trabajo_ && nucleo_.TrabajoTerminado(er.trabajo)) {
      AplicarExportaciones(i);
    }
  }
  ++fotograma_;
  geometria_.NuevoFotograma(fotograma_);
  PerfilAlCambiarDeFotograma();
  if (pintado) {
    ++presentados_;
  }
  EscribirLecturasTerminadas();
  CerrarFotogramaDeLaGuardiaDelCielo();
  SoltarTexturasSiHaceFalta();
  GuardarCacheSiHaceFalta(false);
  InformePeriodico();
  return pintado;
}

// ---- Real 4x MSAA (rex/x360/deko/render.h)

namespace {
// The sample locations of a 4x pass: the 2x2 grid of the sample-space image, sample i at ((i & 1) + 0.5, (i >> 1) +
// 0.5) / 2 of the pixel, so its coverage and depth are what drawing in sample space gave.
DkMultisampleState EstadoMuestras(bool cuatro) {
  DkMultisampleState m;
  dkMultisampleStateDefaults(&m);
  if (cuatro) {
    m.mode = DkMsMode_4x;
    m.rasterizerMode = DkMsMode_4x;
    const DkSampleLocation sitios[4] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
    dkMultisampleStateSetLocations(&m, sitios, 4);
  }
  return m;
}
}  // namespace

uint32_t RenderDk::VistaEstencil(const ImagenDk& imagen) {
  const uint64_t clave = (imagen.serie << 24) ^ 0xE57ull ^ (UINT64_C(1) << 62);
  if (const auto it = vistas_.find(clave); it != vistas_.end()) {
    return it->second;
  }
  const uint32_t ranura = ReservarDescriptorImagen();
  if (!ranura) {
    return 0;
  }
  DkImageView v;
  dkImageViewDefaults(&v, &imagen.imagen);
  v.dsSource = DkDsSource_Stencil;
  EscribirDescriptorImagen(ranura, v);
  vistas_.emplace(clave, ranura);
  vistas_por_serie_[imagen.serie].push_back(clave);
  return ranura;
}

ImagenDk* RenderDk::CompaneraMs(const ImagenDk& muestras) {
  const uint32_t ancho = muestras.ancho / 2, alto = muestras.alto / 2;
  auto it = ms_de_.find(muestras.serie);
  if (it != ms_de_.end()) {
    if (it->second.viva && it->second.ancho == ancho && it->second.alto == alto &&
        it->second.formato == muestras.formato) {
      return &it->second;
    }
    ImagenDk vieja = it->second;
    ms_de_.erase(it);
    DestruirImagen(vieja);
  }
  ImagenDk m;
  uint32_t flags = DkImageFlags_UsageRender;
  if (muestras.profundidad ? REXCVAR_GET(nfsmw_dk_compresion_profundidad) : REXCVAR_GET(nfsmw_dk_compresion_color)) {
    flags |= DkImageFlags_HwCompression;
  }
  if (!CrearImagen(m, DkImageType_2DMS, muestras.formato, ancho, alto, 1, 1, flags, "companera 4x", DkMsMode_4x)) {
    return nullptr;
  }
  REXLOG_INFO("[deko3d] companera 4x de {}x{} ({})", ancho, alto, muestras.profundidad ? "profundidad" : "color");
  const_cast<ImagenDk&>(muestras).s_nueva = true;  // a new companion holds nothing yet
  return &ms_de_.emplace(muestras.serie, m).first->second;
}

// A rectangle from (0, 0) over what is bound, with `fs` reading texture slots `ranura` (and `ranura2`, the stencil),
// on a 4x target (destino_ms) or a sample-space one. Depth: written with its stencil (the replace op takes the
// exported reference), color untouched.
void RenderDk::DibujarRectanguloMs(bool destino_ms, bool profundidad, const DkShader& fs, uint32_t ranura,
                                   uint32_t ranura2, uint32_t ancho, uint32_t filas) {
  DkCmdBuf cmd = nucleo_.comandos();
  const DkViewport vp = {0.0f, 0.0f, float(ancho), float(filas), 0.0f, 1.0f};
  const DkScissor tijera = {0, 0, ancho, filas};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);
  const DkMultisampleState muestras = EstadoMuestras(destino_ms);
  dkCmdBufBindMultisampleState(cmd, &muestras);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  if (profundidad) {
    cws.masks = 0;
  }
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);  // stencil ops replace, compare always
  ds.depthTestEnable = profundidad;
  ds.depthWriteEnable = profundidad;
  ds.depthCompareOp = DkCompareOp_Always;
  ds.stencilTestEnable = profundidad;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  if (profundidad) {
    dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, 0x00, 0xFF);
  }
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_, &fs};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  const DkResHandle texturas[2] = {dkMakeTextureHandle(ranura, 0), dkMakeTextureHandle(ranura2, 0)};
  dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, texturas, profundidad ? 2 : 1);
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  invalidar_zcull_ |= profundidad;
  rop_sin_barrera_ = true;
  OlvidarEstado();
}

void RenderDk::EmpaquetarMs(ImagenDk& muestras, ImagenDk& ms, uint32_t filas) {
  AsegurarTrabajo();
  AntesDe3D();
  DkCmdBuf cmd = nucleo_.comandos();
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);  // what was drawn or copied there, to read
  ++barreras_fragmentos_;
  DkImageView vista;
  dkImageViewDefaults(&vista, &ms.imagen);
  if (muestras.profundidad) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  }
  const uint32_t ranura = VistaDe(muestras, 0x688, muestras.profundidad ? 0 : 0x688);
  const uint32_t ranura2 = muestras.profundidad ? VistaEstencil(muestras) : 0;
  DibujarRectanguloMs(true, muestras.profundidad, muestras.profundidad ? fs_ms_z_ : fs_ms_color_, ranura, ranura2,
                      ms.ancho, std::min(ms.alto, (filas + 1) / 2));
  ++ms_empaquetados_;
}

void RenderDk::DesempaquetarMs(ImagenDk& muestras, ImagenDk& ms, uint32_t filas) {
  AsegurarTrabajo();
  AntesDe3D();
  DkCmdBuf cmd = nucleo_.comandos();
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);  // the pass's draws, to read
  ++barreras_fragmentos_;
  DkImageView vista;
  dkImageViewDefaults(&vista, &muestras.imagen);
  if (muestras.profundidad) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  }
  const uint32_t ranura = VistaDe(ms, 0x688, ms.profundidad ? 0 : 0x688);
  const uint32_t ranura2 = ms.profundidad ? VistaEstencil(ms) : 0;
  DibujarRectanguloMs(false, muestras.profundidad, muestras.profundidad ? fs_desms_z_ : fs_desms_color_, ranura,
                      ranura2, muestras.ancho, std::min(muestras.alto, filas));
  ++ms_desempaquetados_;
}

void RenderDk::CerrarPaseMs() {
  if (!ms_activo_) {
    return;
  }
  ms_activo_ = false;
  // The companions keep what was drawn (ms_nueva): unpacked only when something else needs the sample-space image
  // (LeerMuestras), so 4x passes in a row (Halo 3's depth clear then its G-buffer, tile after tile) hand nothing back.
  for (ImagenDk* imagen : imagenes_pase_) {
    if (imagen && imagen->ms_ligada) {
      imagen->ms_ligada = false;
    }
  }
  const DkMultisampleState uno = EstadoMuestras(false);
  dkCmdBufBindMultisampleState(nucleo_.comandos(), &uno);
  g_.pase_valido = false;  // the next draw binds its targets again (and packs them if it is 4x)
  OlvidarEstado();
}

void RenderDk::LeerMuestras(ImagenDk& imagen) {
  if (!imagen.ms_nueva) {
    return;
  }
  CerrarPaseMs();  // it may still be bound
  imagen.ms_nueva = false;
  if (ImagenDk* ms = CompaneraMs(imagen)) {
    if (imagen.ms_filas) {
      DesempaquetarMs(imagen, *ms, imagen.ms_filas);
    }
  }
  imagen.ms_filas = 0;
}

void RenderDk::EscribirMuestras(ImagenDk& imagen) {
  LeerMuestras(imagen);
  imagen.s_nueva = true;
}

// ---- Memory export (rex/x360/deko/render.h)

namespace {
// Bytes per element of a memory export format (xeno_export_elemento stores these), 0 if it does not.
uint32_t BytesElementoExport(uint32_t formato) {
  switch (formato) {
    case 6: case 7: case 25: case 31: case 36:
      return 4;
    case 26: case 32: case 37:
      return 8;
    case 38:
      return 16;
    default:
      return 0;
  }
}
}  // namespace

void RenderDk::ReservarExportacion(const uint32_t* r, uint32_t vertices, uint32_t& dword, uint32_t& capacidad) {
  dword = capacidad = 0;
  if (!export_bloque_ || !vertices) {
    return;
  }
  // The streams: the float constants holding a stream constant's signature (Xenia's xe_gpu_memexport_stream_t; dword
  // 0: base in dwords and 01 on top, 1: 0x4B000000, 2: format etc. under 0x4B0, 3: element count under 0x96).
  RegionExport region;
  for (uint32_t c = 0; c < 256 && region.n_flujos < kFlujosExport; ++c) {
    const uint32_t* k = r + kRegConstantesVs + c * 4;
    if ((k[0] >> 30) != 1 || k[1] != 0x4B000000u || (k[2] >> 20) != 0x4B0 || (k[3] >> 23) != 0x96) {
      continue;
    }
    const uint32_t base = (k[0] & 0x3FFFFFFFu) << 2;
    const uint64_t bytes = uint64_t(k[3] & 0x7FFFFFu) * BytesElementoExport((k[2] >> 8) & 0x3F);
    if (bytes && bytes <= (16u << 20) && base + bytes <= kMemoriaFisica) {
      region.flujos[region.n_flujos++] = {base, uint32_t(bytes)};
    }
  }
  if (!region.n_flujos) {
    return;  // nowhere it could write
  }
  ExportRanura& er = export_ranuras_[trabajo_ % kRanuras];
  // Each vertex may store up to 5 elements of up to 4 dwords per alloc; 2 dwords a record, 2 of header.
  const uint32_t libres = er.usados + 2 < kDwordsExportRanura ? (kDwordsExportRanura - er.usados - 2) / 2 : 0;
  region.capacidad = std::min<uint32_t>(libres, std::min<uint64_t>(uint64_t(vertices) * 40, 1u << 18));
  if (!region.capacidad) {
    ++export_llenos_;
    return;
  }
  region.dword = er.usados;
  static constexpr uint32_t kCero[2] = {0, 0};
  dkCmdBufPushData(nucleo_.comandos(),
                   export_gpu_ + (uint64_t(trabajo_ % kRanuras) * kDwordsExportRanura + region.dword) * 4, kCero,
                   sizeof(kCero));
  er.usados += 2 + region.capacidad * 2;
  dword = region.dword;
  capacidad = region.capacidad;
  er.regiones.push_back(region);
  ++export_dibujos_;
  dkCmdBufBindStorageBuffer(nucleo_.comandos(), DkStage_Vertex, 15,
                            export_gpu_ + uint64_t(trabajo_ % kRanuras) * kDwordsExportRanura * 4,
                            kDwordsExportRanura * 4);
}

void RenderDk::SincronizarExportaciones(uint32_t inicio, uint32_t fin) {
  for (;;) {
    // The oldest slot of an earlier frame with records over [inicio, fin).
    int32_t elegida = -1;
    for (uint32_t i = 0; i < kRanuras; ++i) {
      const ExportRanura& er = export_ranuras_[i];
      if (er.regiones.empty() || er.fotograma >= fotograma_ || er.trabajo == trabajo_ ||
          (elegida >= 0 && er.trabajo >= export_ranuras_[elegida].trabajo)) {
        continue;
      }
      for (const RegionExport& region : er.regiones) {
        for (uint32_t f = 0; f < region.n_flujos; ++f) {
          if (inicio < region.flujos[f].first + region.flujos[f].second && region.flujos[f].first < fin) {
            elegida = int32_t(i);
          }
        }
      }
    }
    if (elegida < 0) {
      return;
    }
    nucleo_.EsperarTrabajo(export_ranuras_[elegida].trabajo, trabajo_);
    AplicarExportaciones(uint32_t(elegida));
    ++export_esperas_;
  }
}

void RenderDk::AplicarExportaciones(uint32_t ranura) {
  ExportRanura& er = export_ranuras_[ranura];
  if (er.regiones.empty()) {
    er.usados = 0;
    return;
  }
  const uint32_t* base = export_cpu_ + ranura * kDwordsExportRanura;
  dkMemBlockFlushCpuCache(export_bloque_, ranura * kDwordsExportRanura * 4, er.usados * 4);  // the GPU's, not the cache
  // Older than a texture's longest interval: no check is still waiting for them.
  std::erase_if(exportados_, [&](const RangoExportado& e) { return e.fotograma + 64 < fotograma_; });
  bool escrito = false;
  for (const RegionExport& region : er.regiones) {
    const uint32_t* d = base + region.dword;
    const uint32_t n = std::min(d[0], region.capacidad);
    export_registros_ += d[0];
    if (n) {
      escrito = true;
      for (uint32_t f = 0; f < region.n_flujos; ++f) {
        exportados_.push_back(
            {region.flujos[f].first, region.flujos[f].first + region.flujos[f].second, fotograma_, export_serie_ + 1});
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t direccion = d[2 + i * 2], valor = d[3 + i * 2];
      bool dentro = false;
      for (uint32_t f = 0; f < region.n_flujos && !dentro; ++f) {
        dentro = direccion >= region.flujos[f].first && direccion + 4 <= region.flujos[f].first + region.flujos[f].second;
      }
      if (!dentro) {
        ++export_fuera_;
        continue;
      }
      std::memcpy(memoria_->TranslatePhysical(direccion), &valor, 4);  // already in guest byte order
      ++export_escritos_;
    }
  }
  er.regiones.clear();
  er.usados = 0;
  geometria_.Olvidar();  // guest memory changed under the ranges it trusts this epoch
  if (escrito) {
    ++export_serie_;
    ++generacion_texturas_;  // the draws' per-fetch texture cache asks again (PrepararTextura rechecks over them)
  }
}

// ---- The DKSH cache file and the report

void RenderDk::GuardarCacheSiHaceFalta(bool ya) {
  const uint64_t ahora = deko::AhoraNs();
  if (!cache_sucia_ || !cache_.HayNuevas() || (!ya && ahora - cache_guardada_ns_ < 3'000'000'000ull)) {
    return;
  }
  cache_sucia_ = false;
  cache_guardada_ns_ = ahora;
  // Only the shaders added since the last time, appended to the journal (the whole file was 18 MB with Halo 3's, every
  // 20 s: a hitch here and a long SD write while the game streams). The start merges it into the file.
  std::error_code error;
  const bool nuevo = !std::filesystem::exists(RutaDiario(), error);
  auto datos = std::make_shared<std::vector<uint8_t>>(cache_.SerializarNuevas(nuevo));
  const size_t entradas = cache_.size();
  auto escribir = [datos, entradas, nuevo] {
    std::error_code e;
    std::filesystem::create_directories(RutaCache().parent_path(), e);
    std::ofstream f(RutaDiario(), std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(datos->data()), std::streamsize(datos->size()));
    if (!f) {
      REXLOG_WARN("[deko3d] no se pudo escribir {}", RutaDiario().string());
      return;
    }
    REXLOG_INFO("[deko3d] cache de DKSH: {} KB al diario{} ({} entradas en total)", datos->size() >> 10,
                nuevo ? " nuevo" : "", entradas);
  };
  // One write at a time, on a thread that is joined, never detached: libnx has no pthread_detach, and
  // std::thread::detach() throws (std::system_error, "Function not implemented") and ends the process.
  if (guardando_.joinable()) {
    guardando_.join();  // the previous one (or the start's merge), 3 s or more ago
  }
  if (ya) {
    escribir();
  } else {
    guardando_ = std::thread(escribir);
  }
}

void RenderDk::InformePeriodico() {
  const uint64_t ahora = deko::AhoraNs();
  auto tomar = [&] {
    informe_ns_ = ahora;
    informe_dibujos_ = dibujados_;
    informe_fotogramas_ = fotograma_;
    informe_copias_ = copias_;
    informe_borrados_ = borrados_ + borrados_profundidad_;
    informe_barreras_ = barreras_;
    informe_creadas_ = texturas_creadas_;
    informe_subidas_ = texturas_subidas_;
    informe_bytes_subidos_ = bytes_subidos_;
    informe_ns_grabando_ = ns_grabando_;
    informe_lecturas_ = lecturas_escritas_;
    informe_gpu_ns_ = gpu_ns_;
    informe_gpu_trabajos_ = gpu_trabajos_;
    informe_espera_ns_ = nucleo_.NsEsperandoValla();
    informe_saltados_ = borrados_saltados_;
    informe_z_temprana_ = dibujos_z_temprana_;
    informe_categorias_ = gpu_categorias_ns_;
    informe_sub_borrados_ = gpu_sub_borrados_ns_;
    informe_export_dibujos_ = export_dibujos_;
    informe_export_escritos_ = export_escritos_;
    informe_export_fuera_ = export_fuera_;
    informe_etapas_ = ns_etapas_;
    informe_dedupe_bytes_ = dedupe_.bytes_ahorrados();
    informe_intercambios_ = intercambios_;
    informe_restauraciones_ = restauraciones_;
    informe_cubos_ = cubos_refrescados_;
    informe_cielos_ = cielo_aplazados_;
    informe_copias_3d_ = copias_3d_;
    informe_barreras_fragmentos_ = barreras_fragmentos_;
    informe_frontales_ = frontales_ahorrados_;
    informe_bandas_ = bandas_completadas_;
    informe_registros_empujados_ = registros_empujados_;
    informe_empujes_ = empujes_;
    informe_geometria_ = geometria_.estadisticas();
    informe_bytes_huella_ = bytes_huella_;
    informe_bytes_ahorrados_muestra_ = bytes_ahorrados_muestra_;
    informe_muestras_aciertos_ = muestras_aciertos_;
    informe_huellas_aplazadas_ = huellas_aplazadas_;
    informe_texturas_reusadas_ = texturas_reusadas_;
  };
  if (!informe_ns_) {
    tomar();
    return;
  }
  if (ahora - informe_ns_ < 10'000'000'000ull) {
    return;
  }
  // Everything per frame or per interval, except the totals marked as such.
  const double segundos = double(ahora - informe_ns_) / 1e9;
  const double fotogramas = std::max(1.0, double(fotograma_ - informe_fotogramas_));
  const uint64_t dibujos = dibujados_ - informe_dibujos_;
  const deko::EstadisticasNucleo n = nucleo_.TomarEstadisticas();
  X360_INFORME(
      "[deko3d] ultimos {:.0f} s: {:.1f} fotogramas/s, {:.0f} dibujos por fotograma ({} rechazados en total); GPU "
      "{:.2f} ms por fotograma ({} trabajos medidos); por fotograma: {:.1f} copias, {:.1f} borrados, {:.1f} barreras de transferencia, {:.2f} MB de subida (vertices, "
      "indices, constantes, texturas); grabar un dibujo {:.2f} us en RenderDk (sin esperas a la GPU); texturas: {} vivas ({} MB), {} creadas "
      "y {} subidas ({} MB) en el intervalo; lecturas de vuelta al invitado {}; {} trabajos, {} cambios de ranura por "
      "subida llena en total, comandos max {} KB; imagenes {} MB en {} trozos; shaders: {} de la cache, {} compilados "
      "en el acto, {} en segundo plano (en total)",
      segundos, fotogramas / segundos, double(dibujos) / fotogramas, rechazados_,
      double(gpu_ns_ - informe_gpu_ns_) / 1e6 / fotogramas, gpu_trabajos_ - informe_gpu_trabajos_,
      double(copias_ - informe_copias_) / fotogramas,
      double(borrados_ + borrados_profundidad_ - informe_borrados_) / fotogramas,
      double(barreras_ - informe_barreras_) / fotogramas,
      n.trabajos ? double(n.bytes_subida) / fotogramas / 1048576.0 : 0.0,
      dibujos ? double(ns_grabando_ - informe_ns_grabando_) / double(dibujos) / 1000.0 : 0.0, texturas_.size(),
      bytes_texturas_ >> 20, texturas_creadas_ - informe_creadas_, texturas_subidas_ - informe_subidas_,
      (bytes_subidos_ - informe_bytes_subidos_) >> 20, lecturas_escritas_ - informe_lecturas_, n.trabajos,
      cambios_ranura_, n.bytes_comandos_max >> 10, n.bytes_imagenes >> 20, n.trozos_imagenes, aciertos_cache_,
      compilados_en_el_acto_, compilados_en_fondo_);
  const auto ms = [&](uint32_t c) { return double(gpu_categorias_ns_[c] - informe_categorias_[c]) / 1e6 / fotogramas; };
  X360_INFORME(
      "[deko3d] por fotograma: GPU sombras {:.2f} ms, escena {:.2f}, reflejo {:.2f}, 320 {:.2f}, menores {:.2f}, copias "
      "{:.2f}, borrados {:.2f}, resto {:.2f}, escena sin profundidad {:.2f}, GPU parada entre trabajos {:.2f}; el anillo "
      "espero a la GPU {:.2f} ms; borrados saltados {:.1f}; dibujos con Z temprana {:.0f}",
      ms(kGpuSombras), ms(kGpuEscena), ms(kGpuReflejo), ms(kGpu320), ms(kGpuMenores), ms(kGpuCopias), ms(kGpuBorrados),
      ms(kGpuOtros), ms(kGpuEscenaSinProfundidad), ms(kGpuHuecoEntreTrabajos),
      double(nucleo_.NsEsperandoValla() - informe_espera_ns_) / 1e6 / fotogramas,
      double(borrados_saltados_ - informe_saltados_) / fotogramas,
      double(dibujos_z_temprana_ - informe_z_temprana_) / fotogramas);
  const auto ms_borrado = [&](uint32_t k) {
    return double(gpu_sub_borrados_ns_[k] - informe_sub_borrados_[k]) / 1e6 / fotogramas;
  };
  X360_INFORME(
      "[deko3d] por fotograma, borrados por tipo: al empezar un pase {:.2f} ms, rapidos de D3D {:.2f}, estencil "
      "restaurado {:.2f}, destinos nuevos {:.2f}, pendientes {:.2f}, bandas {:.2f}",
      ms_borrado(0), ms_borrado(1), ms_borrado(2), ms_borrado(3), ms_borrado(4), ms_borrado(5));
  X360_INFORME("[deko3d] 4x real: {} pases, {} empaquetados, {} desempaquetados en total", ms_pases_, ms_empaquetados_,
               ms_desempaquetados_);
  X360_INFORME("[deko3d] exportacion a memoria por fotograma: {:.1f} dibujos, {:.0f} dwords escritos, {:.0f} fuera de "
               "sus flujos; {} registros en total, {} veces sin sitio, {} esperas",
               double(export_dibujos_ - informe_export_dibujos_) / fotogramas,
               double(export_escritos_ - informe_export_escritos_) / fotogramas,
               double(export_fuera_ - informe_export_fuera_) / fotogramas, export_registros_, export_llenos_,
               export_esperas_);
  const auto us = [&](uint32_t e) { return dibujos ? double(ns_etapas_[e] - informe_etapas_[e]) / double(dibujos) / 1e3 : 0.0; };
  static constexpr const char* kFasesSm[3] = {"mirando", "APLICANDO", "apagada"};
  X360_INFORME(
      "[deko3d] por fotograma: {:.2f} intercambios en vez de copias, {:.2f} restauraciones, {:.2f} cubos con caras "
      "resueltas, {:.2f} cielos aplazados (guardia {}), {:.2f} MB de vertices sin copiar (repetidos en el trabajo); "
      "sombra por minimo: fase {}, {} ciclos ({} limpios seguidos, {} aplicados), {} diferencias; por dibujo en el "
      "anillo: preparar {:.2f} us, texturas {:.2f}, estado {:.2f}, vertices {:.2f}, indices {:.2f}, constantes y dibujo "
      "{:.2f}",
      double(intercambios_ - informe_intercambios_) / fotogramas,
      double(restauraciones_ - informe_restauraciones_) / fotogramas,
      double(cubos_refrescados_ - informe_cubos_) / fotogramas, double(cielo_aplazados_ - informe_cielos_) / fotogramas,
      cielo_guardia_ == kCieloAplazando ? "superada" : cielo_guardia_ == kCieloDescartado ? "descartada" : "mirando",
      double(dedupe_.bytes_ahorrados() - informe_dedupe_bytes_) / 1048576.0 / fotogramas, kFasesSm[sm_fase_ % 3],
      sm_ciclos_, sm_limpios_seguidos_, sm_ciclos_aplicados_, sm_diferencias_, us(0), us(1), us(2), us(3), us(4), us(5));
  X360_INFORME(
      "[deko3d] por fotograma: {:.1f} copias en el motor 3D, {:.1f} barreras de fragmentos, {:.2f} frontales pintados "
      "desde su destino sin copiar ({} copiados en total, {}), {:.2f} bandas borradas; constantes por diferencias: "
      "{:.1f} registros y {:.1f} empujes por dibujo",
      double(copias_3d_ - informe_copias_3d_) / fotogramas,
      double(barreras_fragmentos_ - informe_barreras_fragmentos_) / fotogramas,
      double(frontales_ahorrados_ - informe_frontales_) / fotogramas, frontales_copiados_,
      frontal_apagado_ ? "APAGADO por una lectura tardia" : "activo", double(bandas_completadas_ - informe_bandas_) / fotogramas,
      dibujos ? double(registros_empujados_ - informe_registros_empujados_) / double(dibujos) : 0.0,
      dibujos ? double(empujes_ - informe_empujes_) / double(dibujos) : 0.0);
  X360_INFORME(
      "[deko3d] comprobacion de texturas: por fotograma {:.2f} MB leidos para sus huellas, {:.2f} MB ahorrados por "
      "muestreo ({:.1f} recomprobaciones solo con la muestra), {:.1f} aplazadas por el presupuesto; muestreo {} ({} "
      "con las dos huellas, {} muestras distintas en total); {:.0f} % de los dibujos reusan las texturas del anterior",
      double(bytes_huella_ - informe_bytes_huella_) / 1048576.0 / fotogramas,
      double(bytes_ahorrados_muestra_ - informe_bytes_ahorrados_muestra_) / 1048576.0 / fotogramas,
      double(muestras_aciertos_ - informe_muestras_aciertos_) / fotogramas,
      double(huellas_aplazadas_ - informe_huellas_aplazadas_) / fotogramas,
      muestreo_huellas_ > 0 ? "activo" : muestreo_huellas_ == 0 ? "APAGADO" : "sin empezar", muestras_comprobadas_,
      muestras_distintas_, dibujos ? 100.0 * double(texturas_reusadas_ - informe_texturas_reusadas_) / double(dibujos) : 0.0);
  if (geometria_.activo()) {
    const deko::AlmacenGeometria::Estadisticas& g = geometria_.estadisticas();
    const deko::AlmacenGeometria::Estadisticas& a = informe_geometria_;
    const auto mb = [&](uint64_t ahora_b, uint64_t antes_b) { return double(ahora_b - antes_b) / 1048576.0 / fotogramas; };
    X360_INFORME(
        "[deko3d] geometria entre fotogramas: por fotograma {:.2f} MB sin copiar ({:.0f} de {:.0f} consultas), {:.2f} MB "
        "con huella, {:.3f} MB guardados ({:.1f} rangos), {:.1f} cambios, {:.1f} al anillo sin huella (cambiantes); "
        "{} MB de {} usados, {} rangos conocidos; {} desalojos, {} sin sitio, {} colisiones (en el intervalo); "
        "muestreo {}: {:.0f} comprobaciones por muestra por fotograma, {} desacuerdos en total",
        mb(g.bytes_aciertos, a.bytes_aciertos), double(g.aciertos - a.aciertos) / fotogramas,
        double(g.consultas - a.consultas) / fotogramas, mb(g.bytes_huella, a.bytes_huella),
        mb(g.bytes_admitidos, a.bytes_admitidos), double(g.admitidas - a.admitidas) / fotogramas,
        double(g.cambios - a.cambios) / fotogramas, double(g.volatiles - a.volatiles) / fotogramas,
        geometria_.bytes_usados() >> 20, geometria_.bytes_total() >> 20, geometria_.entradas(),
        g.desalojos - a.desalojos, g.sin_sitio - a.sin_sitio, g.colisiones - a.colisiones,
        geometria_.muestreo() ? (g.muestreos > 20000 ? "activo" : "con la guardia") : "APAGADO",
        double(g.muestreos - a.muestreos) / fotogramas, g.desacuerdos);
  }
  tomar();
}

// ---- The GPU profile by draw (nfsmw_dk_perfil_gpu_s)

void RenderDk::PerfilAlCambiarDeFotograma() {
  if (!perfil_bloque_) {
    return;
  }
  if (perfilando_) {
    // The profiled frame ended: its works are read once the GPU has done them.
    perfilando_ = false;
    perfil_pendiente_ = !perfil_marcas_.empty();
  }
  if (perfil_pendiente_) {
    // Its last work is submitted (the frame closed it); its fence then is its own. Not while it is still the one
    // being recorded.
    if ((nucleo_.TrabajoAbierto() && perfil_trabajo_ == trabajo_) || !nucleo_.TrabajoTerminado(perfil_trabajo_)) {
      return;
    }
    InformeDelPerfil();
    perfil_pendiente_ = false;
    perfil_marcas_.clear();
  }
  const uint64_t ahora = deko::AhoraNs();
  if (ahora - perfil_ns_ >= uint64_t(REXCVAR_GET(nfsmw_dk_perfil_gpu_s)) * 1'000'000'000ull) {
    perfil_ns_ = ahora;
    perfilando_ = true;
    perfil_fotograma_ = fotograma_;
    perfil_marcas_.clear();
  }
}

void RenderDk::InformeDelPerfil() {
  dkMemBlockFlushCpuCache(perfil_bloque_, 0, uint32_t(perfil_marcas_.size()) * 32);
  static constexpr const char* kCategorias[kGpuCategorias] = {"otros",   "sombras", "escena",  "reflejo",
                                                              "320",     "menores", "copias",  "borrados",
                                                              "escena sin Z", "hueco"};
  struct Suma {
    double ns = 0;
    uint32_t dibujos = 0;
    uint64_t vertices = 0;
  };
  struct Pase {
    Suma suma;
    uint32_t categoria = 0, ancho = 0, alto = 0, orden = 0;
    uint64_t color = 0, profundidad = 0;
  };
  std::unordered_map<uint64_t, Pase> pases;
  std::unordered_map<const DkShader*, Suma> por_ps, por_vs;
  std::array<Suma, kGpuCategorias> por_categoria{};
  double total = 0;
  for (size_t i = 0; i < perfil_marcas_.size(); ++i) {
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, perfil_cpu_ + i * 32 + 8, sizeof(t0));
    std::memcpy(&t1, perfil_cpu_ + i * 32 + 24, sizeof(t1));
    const double ns = t1 >= t0 ? double(t1 - t0) * deko::kNsPorUnidadGpu : 0.0;
    const MarcaPerfil& m = perfil_marcas_[i];
    if (m.repeticion) {
      continue;  // the repeat test's extra copy: not part of the frame
    }
    total += ns;
    auto sumar = [&](Suma& s) {
      s.ns += ns;
      ++s.dibujos;
      s.vertices += m.vertices;
    };
    auto [pase, nuevo] = pases.try_emplace(m.pase);
    if (nuevo) {
      pase->second.categoria = m.categoria;
      pase->second.ancho = m.ancho;
      pase->second.alto = m.alto;
      pase->second.color = m.color;
      pase->second.profundidad = m.profundidad;
      pase->second.orden = uint32_t(pases.size());
    }
    sumar(pase->second.suma);
    sumar(por_ps[m.ps]);
    sumar(por_vs[m.vs]);
    sumar(por_categoria[std::min<uint32_t>(m.categoria, kGpuCategorias - 1)]);
  }
  std::string categorias;
  for (uint32_t c = 0; c < kGpuCategorias; ++c) {
    if (por_categoria[c].dibujos) {
      categorias += fmt::format("{}{} {:.2f} ms en {} dibujos", categorias.empty() ? "" : ", ", kCategorias[c],
                                por_categoria[c].ns / 1e6, por_categoria[c].dibujos);
    }
  }
  REXLOG_INFO("[deko3d] perfil GPU del fotograma {}: {} dibujos medidos uno a uno, {:.2f} ms dentro de los dibujos "
              "({} pases); por categoria: {}",
              perfil_fotograma_, perfil_marcas_.size(), total / 1e6, pases.size(), categorias);
  // The passes that cost most, in frame order within the top.
  std::vector<const Pase*> orden_pases;
  for (const auto& [serie, pase] : pases) {
    orden_pases.push_back(&pase);
  }
  std::sort(orden_pases.begin(), orden_pases.end(), [](const Pase* a, const Pase* b) { return a->suma.ns > b->suma.ns; });
  for (size_t i = 0; i < std::min<size_t>(orden_pases.size(), 12); ++i) {
    const Pase& p = *orden_pases[i];
    REXLOG_INFO("[deko3d] perfil GPU: pase {} del fotograma ({}, {}x{}, color {:016X}, Z {:016X}): {:.2f} ms, {} "
                "dibujos, {} vertices o indices",
                p.orden, kCategorias[std::min<uint32_t>(p.categoria, kGpuCategorias - 1)], p.ancho, p.alto, p.color,
                p.profundidad, p.suma.ns / 1e6, p.suma.dibujos, p.suma.vertices);
  }
  // The most expensive single draws, with what decides their cost (pixels, textures). The extra copies of the repeat
  // test follow their draw in the marks: listed with it, not on their own.
  auto ms_de = [&](size_t i) {
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, perfil_cpu_ + i * 32 + 8, sizeof(t0));
    std::memcpy(&t1, perfil_cpu_ + i * 32 + 24, sizeof(t1));
    return t1 >= t0 ? double(t1 - t0) * deko::kNsPorUnidadGpu / 1e6 : 0.0;
  };
  std::vector<std::pair<double, size_t>> dibujos;
  for (size_t i = 0; i < perfil_marcas_.size(); ++i) {
    if (!perfil_marcas_[i].repeticion) {
      dibujos.emplace_back(ms_de(i), i);
    }
  }
  std::sort(dibujos.begin(), dibujos.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  static constexpr const char* kVeces[5] = {"", "otra vez igual", "con el sampler 0", "con texturas vacias de 1x1",
                                            "escribiendo solo el destino 0"};
  for (size_t k = 0; k < std::min<size_t>(dibujos.size(), 8); ++k) {
    const size_t i = dibujos[k].second;
    const MarcaPerfil& m = perfil_marcas_[i];
    const auto it = datos_shaders_.find(m.ps);
    std::string pruebas;
    for (size_t j = i + 1; j < perfil_marcas_.size() && perfil_marcas_[j].repeticion; ++j) {
      pruebas += fmt::format("; {} {:.2f} ms", kVeces[std::min<uint32_t>(perfil_marcas_[j].repeticion, 4)], ms_de(j));
    }
    REXLOG_INFO("[deko3d] perfil GPU: dibujo {} ({:.2f} ms): PS {:016X} (n{}), pase {} {}x{}, viewport {:.0f}x{:.0f}, "
                "tijera {}x{}, mascaras {:04X}, RB_DEPTHCONTROL {:08X}, {} vertices o indices; {} texturas, la primera "
                "{}x{} formato {} sampler {}{}",
                i, dibujos[k].first, it != datos_shaders_.end() ? it->second.huella : 0,
                it != datos_shaders_.end() ? it->second.numero : 0,
                kCategorias[std::min<uint32_t>(m.categoria, kGpuCategorias - 1)], m.ancho, m.alto, m.vp_ancho,
                m.vp_alto, m.tijera_ancho, m.tijera_alto, m.mascaras, m.profundidad_control, m.vertices, m.texturas,
                m.tex_ancho, m.tex_alto, m.tex_formato, m.tex_sampler, pruebas);
  }
  auto shaders = [&](const std::unordered_map<const DkShader*, Suma>& mapa, const char* que, size_t cuantos) {
    std::vector<std::pair<const DkShader*, Suma>> lista(mapa.begin(), mapa.end());
    std::sort(lista.begin(), lista.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    for (size_t i = 0; i < std::min(lista.size(), cuantos); ++i) {
      const auto& [shader, suma] = lista[i];
      const auto it = datos_shaders_.find(shader);
      const DatosShaderDk d = it != datos_shaders_.end() ? it->second : DatosShaderDk{};
      REXLOG_INFO("[deko3d] perfil GPU: {} {:016X} (n{}, espec {:08X}, variante {:X}): {:.2f} ms en {} dibujos ({:.1f} "
                  "us por dibujo, {} vertices o indices); uam: {} registros, {} bytes de codigo, scratch {}",
                  que, d.huella, d.numero, d.espec, d.variante, suma.ns / 1e6, suma.dibujos,
                  suma.dibujos ? suma.ns / 1e3 / suma.dibujos : 0.0, suma.vertices, d.registros, d.bytes_codigo,
                  d.scratch);
    }
  };
  shaders(por_ps, "PS", 15);
  shaders(por_vs, "VS", 6);
}

// ---- Statistics for the system's report

void RenderDk::EstadisticasOclusion(uint64_t valores[5]) const { std::fill(valores, valores + 5, 0); }

EstadisticasDibujos RenderDk::EstadisticasDeDibujos() const {
  EstadisticasDibujos e;
  e.dibujados = dibujados_;
  e.rechazados = rechazados_;
  e.texturas = texturas_.size();
  e.subidas_textura = texturas_subidas_;
  e.megas_texturas = bytes_texturas_ >> 20;
  for (const auto& [causa, veces] : causas_) {
    e.causas.emplace_back(causa, veces);
  }
  std::sort(e.causas.begin(), e.causas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return e;
}

void RenderDk::Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados, uint64_t& rechazos) const {
  copias = copias_;
  borrados = borrados_;
  presentados = presentados_;
  rechazos = rechazados_;
}

void RenderDk::TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const {
  nanosegundos = gpu_ns_;
  trabajos = gpu_trabajos_;
}
void RenderDk::EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const { veces = nanosegundos = 0; }
void RenderDk::CosteGrabar(uint64_t coste[6]) const { std::fill(coste, coste + 6, 0); }
void RenderDk::EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                                    std::array<uint64_t, kGpuCategorias>& vertices,
                                    std::array<uint64_t, kGpuCategorias>& primitivas) const {
  fragmentos.fill(0);
  vertices.fill(0);
  primitivas.fill(0);
}
void RenderDk::CopiasPorTamano(std::array<uint64_t, 4>& copias, std::array<uint64_t, 4>& pixeles) const {
  copias.fill(0);
  pixeles.fill(0);
}
void RenderDk::EstadisticasPorShader(std::vector<uint64_t>& fragmentos, std::vector<uint64_t>& dibujos,
                                     uint64_t& fotogramas) const {
  fragmentos.clear();
  dibujos.clear();
  fotogramas = 0;
}
void RenderDk::IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                                    double& peor_ms) const {
  cubetas = cubetas_swap_;
  solapes = 0;
  peor_ms = peor_swap_ms_;
}
void RenderDk::CostePresentar(uint64_t coste[12]) const { std::fill(coste, coste + 12, 0); }

std::unique_ptr<DestinosNativos> CrearDestinosDeko3d(deko::ProveedorDeko3d* proveedor, rex::memory::Memory* memoria,
                                                    rex::x360::RenderHooks& hooks) {
  if (!proveedor) {
    return nullptr;
  }
  return std::make_unique<RenderDk>(*proveedor, memoria, hooks);
}

}  // namespace rex::x360
