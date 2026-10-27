/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/xevent.h>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <cstdio>

REXCVAR_DECLARE(bool, switch_subir_hilo_audio);  // xboxkrnl_audio.cpp

namespace rex::system {

namespace {
// Halo 3 delivered 40 of the 187 audio frames a second (72 % missing, the static): its audio callback, run on the
// audio system's worker (0x2B), waits for an event its mixing thread sets, and that thread sits at 0x3B with every
// other game thread on three full cores. 26 ms a call against a 5.33 ms frame (`[audio] 10 s`).
thread_local bool g_hilo_de_audio = false;
thread_local bool g_hilo_subido = false;
constexpr size_t kEventosAudio = 4;
std::atomic<const XObject*> g_eventos_audio[kEventosAudio] = {};
std::atomic<uint32_t> g_hilos_subidos{0};

void ComprobarProductorDeAudio(const XObject* evento) {
  if (g_hilo_subido || g_hilo_de_audio) {
    return;
  }
  bool es = false;
  for (const auto& e : g_eventos_audio) {
    es |= e.load(std::memory_order_relaxed) == evento;
  }
  if (!es) {
    return;
  }
  g_hilo_subido = true;  // asked once per thread, whatever the answer
#if REX_PLATFORM_SWITCH
  auto* hilo = XThread::GetCurrentThread();
  if (!REXCVAR_GET(switch_subir_hilo_audio) || !hilo || !hilo->is_guest_thread() || !hilo->thread()) {
    return;
  }
  // At most two: a third setter is not the mixer, and 0x3A is not time-sliced (a spinning thread there keeps a core).
  if (g_hilos_subidos.fetch_add(1, std::memory_order_relaxed) >= 2) {
    return;
  }
  hilo->thread()->set_priority(0x3A);
  std::fprintf(stderr, "[audio] el hilo %u alimenta el callback de audio: sube a la prioridad 0x3A\n",
               hilo->thread_id());
#endif
}
}  // namespace

void MarcarHiloDeAudio() { g_hilo_de_audio = true; }

void AnotarEsperaDeAudio(XObject* const* objetos, size_t n) {
  if (!g_hilo_de_audio) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    bool ya = false;
    for (const auto& e : g_eventos_audio) {
      ya |= e.load(std::memory_order_relaxed) == objetos[i];
    }
    for (size_t j = 0; j < kEventosAudio && !ya; ++j) {
      const XObject* vacio = nullptr;
      ya = g_eventos_audio[j].compare_exchange_strong(vacio, objetos[i], std::memory_order_relaxed);
    }
  }
}

XEvent::XEvent(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  if (out_type) {
    *out_type = manual_reset_ ? 0x00 : 0x01;
  }
  if (out_state) {
    // Query the live host event, not the stale guest header
    auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
    if (result == rex::thread::WaitResult::kSuccess) {
      *out_state = 1;
      // Re-signal since we consumed the signal by waiting
      event_->Set();
    } else {
      *out_state = 0;
    }
  }
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  ComprobarProductorDeAudio(this);
  event_->Set();
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  event_->Pulse();
  return 1;
}

int32_t XEvent::Reset() {
  event_->Reset();
  return 1;
}

void XEvent::Clear() {
  event_->Reset();
}

bool XEvent::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == rex::thread::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == rex::thread::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = rex::thread::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = rex::thread::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace rex::system
