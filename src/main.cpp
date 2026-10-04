// tdu2 - ReXGlue Recompiled Project

#include "generated/default/tdu2_init.h"

#include "tdu2_app.h"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN / NOMINMAX already come from the SDK's own compile
// definitions, so redefining them here would just warn.
#include <windows.h>

#include <cstdlib>
#include <exception>
#include <new>

// rexruntime raises abort() when the guest hits something it cannot service
// (e.g. a call to an unregistered function address). The MSVC debug CRT turns
// that into a MODAL "Debug Error! abort() has been called" dialog, which blocks
// the process forever on an unattended run and hides the real cause. Route it
// to the log instead and take the process down, so a run always terminates and
// leaves a diagnosable log behind.
namespace {

void InstallCrashHandlers() {
  // Keep OS-level fault dialogs suppressed.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

  // Tell the CRT not to pop its abort dialog: _WRITE_ABORT_MSG goes to stderr,
  // _CALL_REPORTFAULT is the behaviour that raises the interactive box.
  _set_abort_behavior(_WRITE_ABORT_MSG, _CALL_REPORTFAULT);

  std::set_terminate([]() {
    std::abort();
  });
}

// A class type, because __attribute__((init_priority)) only applies to
// file-scope objects of class type. Runs before any other static initialiser
// in the image, so the handlers are in place before the guest can fault.
__attribute__((init_priority(101))) struct CrashHandlerInstaller {
  CrashHandlerInstaller() { InstallCrashHandlers(); }
} g_crash_handler_installer;

}  // namespace
#endif

REX_DEFINE_APP(tdu2, Tdu2App::Create)
