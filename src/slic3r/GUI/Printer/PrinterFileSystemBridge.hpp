// Bambu Bridge — PrinterFileSystem static-lib dispatch hook.
//
// PrinterFileSystem.cpp owns `StaticBambuLib::get()`, the single point
// where the proprietary libBambuSource function pointers are resolved
// for every consumer in the slicer (PrinterFileSystem itself,
// wxMediaCtrl3, the gstbambusrc plugin, ...). On bridge builds those
// pointers also need to dispatch virtual tunnels (minted by our own
// `Bambu_Create` for `bambu:///virtual/...` URLs) into
// `bambu_virtual_client::virtual_tunnel::*` instead of libBambuSource.
//
// All that wrap logic used to live inline in PrinterFileSystem.cpp
// (≈110 lines of 11 lambda definitions plus their `static auto real_*`
// captures). It has been moved here so the PrinterFileSystem.cpp diff
// against upstream bambulab/BambuStudio shrinks to a one-line hook.
//
// Behaviour-preserving: every lambda installed by
// `install_static_bambu_lib_dispatch` matches the inline version it
// replaced byte-for-byte. The captured `real_*` originals live in
// file-scope statics inside the .cpp (same lifetime as the old
// `static auto` locals).

#ifndef SLIC3R_GUI_PRINTER_FILE_SYSTEM_BRIDGE_HPP
#define SLIC3R_GUI_PRINTER_FILE_SYSTEM_BRIDGE_HPP

#if defined(BAMBU_BRIDGE)

// Match PrinterFileSystem.h: BambuTunnel.h's `BambuLib` struct (the
// table of function pointers we mutate here) only exists when
// BAMBU_DYNAMIC is defined before inclusion.
#ifndef BAMBU_DYNAMIC
#define BAMBU_DYNAMIC
#endif
#include "BambuTunnel.h"

namespace Slic3r {
namespace bridge {

// Replace the `Bambu_*` function pointers on `lib` with dispatcher
// lambdas that route virtual tunnels to `virtual_tunnel::*` and fall
// through to the originals for real tunnels. Captures `lib`'s current
// pointers as the "real" set in file-scope statics, so the dispatchers
// can always reach the originals.
//
// Idempotent contract mirrors the caller's: `StaticBambuLib::get()`
// only invokes this once, after the libBambuSource pointers are first
// resolved. The recorded `real_*` captures persist for the lifetime of
// the process.
//
// Fake_Bambu_Create_fp is the `Fake_Bambu_Create` symbol from
// PrinterFileSystem.cpp's StaticBambuLib (used as the fallback when the
// real `Bambu_Create` is null and the URL is not virtual). Passed in
// rather than re-declared so the bridge TU does not depend on
// StaticBambuLib's TU-private layout.
using BambuCreateFn = int (*)(Bambu_Tunnel*, char const*);
void install_static_bambu_lib_dispatch(BambuLib&     lib,
                                       BambuCreateFn fake_bambu_create);

} // namespace bridge
} // namespace Slic3r

#endif // BAMBU_BRIDGE

#endif // SLIC3R_GUI_PRINTER_FILE_SYSTEM_BRIDGE_HPP
