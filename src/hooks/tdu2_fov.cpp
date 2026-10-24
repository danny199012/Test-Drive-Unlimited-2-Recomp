// tdu2 - Field of view.
//
// Linked into the game DLL (tdu2_TestDrive2_DLL), not the exe: generated
// functions are weak aliases of their __imp__ originals, and a strong
// definition only replaces them within the same linked image.
//
// The render camera is the camera object whose pointer lives at 0x890945C4
// (0x8907EC10 in practice). Each frame, the active camera controller copies
// its own camera into it with sub_881D2D48(dst, src) - see sub_88E32628 - and
// the renderer builds the projection and culling from the render camera's
// FOV (+704, radians). Widening that FOV right after the copy therefore widens
// everything drawn, for every camera (chase, in-car, cutscenes), and as the
// render camera is refreshed from its controller every frame the change never
// accumulates or leaks back into the game's own camera logic.
//
// The widening is a ratio of half-angle tangents rather than an added angle,
// so narrow cinematic shots and wide chase shots keep their proportions - the
// approach King Kong Recompiled uses for its FOV slider.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(tdu2_fov_boost, 0, "TDU2",
                     "Extra vertical field of view in degrees for the default chase camera "
                     "(45 degrees); other cameras widen in proportion. 0 = the game's own FOV")
    .range(0, 40);

namespace {

constexpr uint32_t kRenderCameraPointer = 0x890945C4;
constexpr uint32_t kCameraFovOffset = 704;

// Vertical FOV of the default chase camera (0.7854 rad), which the boost is
// expressed against. The in-car camera is 60 degrees.
constexpr double kReferenceFovDegrees = 45.0;
constexpr double kPi = 3.14159265358979323846;

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}

float LoadF32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(LoadU32(base, address));
}

void StoreF32(uint8_t* base, uint32_t address, float value) {
  const uint32_t swapped = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &swapped, sizeof(swapped));
}

// Camera pointers can be stale while scenes change; reading an unmapped guest
// page from here is a host access violation, so check before touching it.
bool IsGuestReadWrite(uint32_t address, uint32_t length) {
  if (address == 0) {
    return false;
  }
  auto* heap = REX_KERNEL_MEMORY()->LookupHeap(address);
  return heap != nullptr && heap->QueryRangeAccess(address, address + length - 1) !=
                                rex::memory::PageAccess::kNoAccess;
}

double LensFactor(int boost_degrees) {
  const double reference = kReferenceFovDegrees * kPi / 180.0;
  const double boosted = (kReferenceFovDegrees + boost_degrees) * kPi / 180.0;
  return std::tan(boosted * 0.5) / std::tan(reference * 0.5);
}

}  // namespace

REX_EXTERN(__imp__sub_881D2D48);

REX_HOOK_RAW(sub_881D2D48) {
  const uint32_t destination = ctx.r3.u32;
  __imp__sub_881D2D48(ctx, base);
  if (destination == 0 || destination != LoadU32(base, kRenderCameraPointer) ||
      !IsGuestReadWrite(destination + kCameraFovOffset, 4)) {
    return;
  }
  const uint32_t fov_address = destination + kCameraFovOffset;
  const float fov = LoadF32(base, fov_address);
  const int boost = REXCVAR_GET(tdu2_fov_boost);
  // Radians in practice; leave anything implausible alone.
  if (boost <= 0 || !(fov > 0.01f && fov < 3.0f)) {
    return;
  }
  const double widened =
      std::min(2.0 * std::atan(std::tan(fov * 0.5) * LensFactor(boost)), 2.8);
  StoreF32(base, fov_address, float(widened));
}
