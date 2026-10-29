// rex::deko3d (from nfsmw-nx): the DKSH cache file. See rex/ui/deko3d/cache_dksh.h.

#include <rex/ui/deko3d/cache_dksh.h>

#include <rex/ui/deko3d/glsl.h>

#include <cstring>
// The same xxHash setup as nfsmw_shader_library.cpp (reads through memcpy: see the note there).
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace rex::deko3d {
namespace {

constexpr uint32_t kMagia = 0x4B44464Eu;  // "NFDK"
constexpr uint32_t kVersion = 2;
constexpr size_t kCabecera = 32;
constexpr size_t kEntrada = 24;
constexpr uint32_t kMaxEntradas = 1u << 16;
constexpr uint32_t kMaxDksh = 1u << 20;
constexpr uint32_t kMagiaDiario = 0x4A44464Eu;  // "NFDJ"
constexpr uint32_t kVersionDiario = 3;
constexpr size_t kCabeceraDiario = 24;  // magic, version, compiler id, stamp, 0
constexpr uint32_t kSoloUso = 0xFFFFFFFFu;  // a journal record with no DKSH: the entry was used (MarcarUso)
uint32_t g_sal = 0;

uint32_t U32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t U64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
void Poner(std::vector<uint8_t>& d, size_t at, const void* v, size_t n) { std::memcpy(d.data() + at, v, n); }

uint64_t HuellaId() {
  const std::string id = IdCompilador();
  return XXH3_64bits(id.data(), id.size());
}

}  // namespace

std::string IdCompilador() {
  return "uam 1.1.0 + SwitchWakerHD 2d147aa (patches 1-7); SPIRV-Cross vulkan-sdk-1.4.363.0; GLSL revision " +
         std::to_string(kRevisionGlsl);
}

void FijarSalCache(uint32_t sal) { g_sal = sal; }

uint64_t HuellaSpirv(std::span<const uint32_t> espirv) { return XXH3_64bits(espirv.data(), espirv.size_bytes()); }

bool CacheDksh::Cargar(std::span<const uint8_t> archivo, std::string& motivo) {
  entradas_.clear();
  sales_.clear();
  if (archivo.size() < kCabecera || U32(archivo.data()) != kMagia) {
    motivo = "no es una cache de DKSH";
    return false;
  }
  if (U32(archivo.data() + 4) != kVersion) {
    motivo = "otra version del formato";
    return false;
  }
  if (U64(archivo.data() + 8) != HuellaId()) {
    motivo = "hecha por otro compilador (se espera: " + IdCompilador() + ")";
    return false;
  }
  const uint32_t n = U32(archivo.data() + 16);
  if (n > kMaxEntradas || archivo.size() < kCabecera + size_t(n) * kEntrada ||
      XXH3_64bits(archivo.data() + kCabecera, archivo.size() - kCabecera) != U64(archivo.data() + 24)) {
    motivo = "danada";
    return false;
  }
  size_t datos = kCabecera + size_t(n) * kEntrada;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = archivo.data() + kCabecera + size_t(i) * kEntrada;
    const uint32_t bytes = U32(e + 16);
    if (bytes > kMaxDksh || datos + bytes > archivo.size()) {
      entradas_.clear();
      motivo = "danada";
      return false;
    }
    const Clave clave{U64(e), U32(e + 8), U32(e + 12)};
    entradas_[clave].assign(archivo.data() + datos, archivo.data() + datos + bytes);
    sales_[clave] = U32(e + 20);
    datos = (datos + bytes + 3) & ~size_t(3);
  }
  return true;
}

std::vector<uint8_t> CacheDksh::Serializar() const {
  // Entries made under another stamp are left out (FijarSalCache).
  auto guardar = [this](const Clave& clave) {
    if (!g_sal) {
      return true;
    }
    const auto it = sales_.find(clave);
    return it != sales_.end() && it->second == g_sal;
  };
  size_t total = kCabecera, n_guardadas = 0;
  for (const auto& [clave, dksh] : entradas_) {
    if (guardar(clave)) {
      total += kEntrada;
      ++n_guardadas;
    }
  }
  for (const auto& [clave, dksh] : entradas_) {
    if (guardar(clave)) {
      total = (total + dksh.size() + 3) & ~size_t(3);
    }
  }
  std::vector<uint8_t> d(total, 0);
  const uint32_t cabecera[2] = {kMagia, kVersion};
  Poner(d, 0, cabecera, 8);
  const uint64_t id = HuellaId();
  Poner(d, 8, &id, 8);
  const uint32_t n = uint32_t(n_guardadas);
  Poner(d, 16, &n, 4);
  size_t e = kCabecera;
  size_t datos = kCabecera + n_guardadas * kEntrada;
  for (const auto& [clave, dksh] : entradas_) {
    if (!guardar(clave)) {
      continue;
    }
    const uint32_t bytes = uint32_t(dksh.size());
    const auto sal = sales_.find(clave);
    const uint32_t sello = sal != sales_.end() ? sal->second : 0u;
    Poner(d, e, &clave.huella, 8);
    Poner(d, e + 8, &clave.especializacion, 4);
    Poner(d, e + 12, &clave.variante, 4);
    Poner(d, e + 16, &bytes, 4);
    Poner(d, e + 20, &sello, 4);
    e += kEntrada;
    if (bytes) {
      Poner(d, datos, dksh.data(), bytes);
    }
    datos = (datos + bytes + 3) & ~size_t(3);
  }
  const uint64_t integridad = XXH3_64bits(d.data() + kCabecera, d.size() - kCabecera);
  Poner(d, 24, &integridad, 8);
  return d;
}

const std::vector<uint8_t>* CacheDksh::Buscar(const Clave& clave) const {
  const auto it = entradas_.find(clave);
  return it == entradas_.end() ? nullptr : &it->second;
}

void CacheDksh::Anadir(const Clave& clave, std::vector<uint8_t> dksh) {
  auto [it, nueva] = entradas_.insert_or_assign(clave, std::move(dksh));
  sales_[clave] = g_sal;
  if (nueva) {
    nuevas_.push_back(clave);
  }
}

bool CacheDksh::MarcarUso(const Clave& clave) {
  if (!g_sal) {
    return false;
  }
  uint32_t& sal = sales_[clave];
  if (sal == g_sal) {
    return false;
  }
  sal = g_sal;
  usos_.push_back(clave);
  return true;
}

std::vector<uint8_t> CacheDksh::SerializarNuevas(bool cabecera) {
  std::vector<uint8_t> d;
  if (cabecera) {
    d.resize(kCabeceraDiario, 0);
    const uint32_t c[2] = {kMagiaDiario, kVersionDiario};
    Poner(d, 0, c, 8);
    const uint64_t id = HuellaId();
    Poner(d, 8, &id, 8);
    Poner(d, 16, &g_sal, 4);
  }
  for (const Clave& clave : usos_) {
    const size_t at = d.size();
    d.resize(at + kEntrada, 0);
    Poner(d, at, &clave.huella, 8);
    Poner(d, at + 8, &clave.especializacion, 4);
    Poner(d, at + 12, &clave.variante, 4);
    Poner(d, at + 16, &kSoloUso, 4);
  }
  usos_.clear();
  for (const Clave& clave : nuevas_) {
    const auto it = entradas_.find(clave);
    if (it == entradas_.end()) {
      continue;
    }
    const std::vector<uint8_t>& dksh = it->second;
    const size_t at = d.size();
    d.resize(at + kEntrada + ((dksh.size() + 3) & ~size_t(3)), 0);
    const uint32_t bytes = uint32_t(dksh.size());
    const uint32_t control = uint32_t(XXH3_64bits(dksh.data(), dksh.size()));
    Poner(d, at, &clave.huella, 8);
    Poner(d, at + 8, &clave.especializacion, 4);
    Poner(d, at + 12, &clave.variante, 4);
    Poner(d, at + 16, &bytes, 4);
    Poner(d, at + 20, &control, 4);
    if (bytes) {
      Poner(d, at + kEntrada, dksh.data(), bytes);
    }
  }
  nuevas_.clear();
  return d;
}

int CacheDksh::CargarDiario(std::span<const uint8_t> diario) {
  if (diario.size() < kCabeceraDiario || U32(diario.data()) != kMagiaDiario ||
      U32(diario.data() + 4) != kVersionDiario || U64(diario.data() + 8) != HuellaId()) {
    return -1;
  }
  const uint32_t sal = U32(diario.data() + 16);
  int anadidas = 0;
  size_t at = kCabeceraDiario;
  while (at + kEntrada <= diario.size()) {
    const uint8_t* e = diario.data() + at;
    const uint32_t bytes = U32(e + 16);
    if (bytes == kSoloUso) {
      const Clave clave{U64(e), U32(e + 8), U32(e + 12)};
      if (entradas_.count(clave)) {
        sales_[clave] = sal;
      }
      ++anadidas;
      at += kEntrada;
      continue;
    }
    if (bytes > kMaxDksh || at + kEntrada + bytes > diario.size() ||
        uint32_t(XXH3_64bits(e + kEntrada, bytes)) != U32(e + 20)) {
      break;
    }
    const Clave clave{U64(e), U32(e + 8), U32(e + 12)};
    entradas_[clave].assign(e + kEntrada, e + kEntrada + bytes);
    sales_[clave] = sal;
    ++anadidas;
    at += kEntrada + ((bytes + 3) & ~size_t(3));
  }
  return anadidas;
}

}  // namespace rex::deko3d
