# `--bridge-only` Removal — Phased Diff Plan (design-only; execute later)

Scope: worktree `D:\BambuBridge\identical-win`. Keep **invisible-GUI** mode
(`BAMBU_BRIDGE_INVISIBLE_GUI=1`, full GUI via `install_gui_worker`) working.

> ⚠️ `src/slic3r/GUI/BridgeBootstrap.cpp` is being edited concurrently for
> invisible-GUI. Line numbers below are a snapshot — **re-anchor on the
> function/comment-banner names at execution time**, not line numbers.
> Gate after every phase: build `BambuStudio_app_gui` + invisible-GUI filament/
> drying control test result unchanged from baseline.

## Reference table (symbol → location → action)

| Symbol / construct | Location | Action | Notes |
|---|---|---|---|
| `--bridge-only` CLI block (is_bridge_multi, arg scan, parse_cli_args, cert/CA probe, g_bridge_only set, GUI_Run) | BambuStudio.cpp:1485-1701 | REMOVE | entire `#if BAMBU_BRIDGE` dispatch + globals |
| g_bridge_only / g_bridge_only_cfg defs | BambuStudio.cpp:1494-1504 | REMOVE | |
| includes BridgeAppCliArgs/BridgeLauncher/BridgeOnlyFlag | BambuStudio.cpp:1485-1492 | REMOVE | |
| BridgeOnlyFlag.hpp | whole file | REMOVE | |
| g_bridge_only branch in on_init_inner | GUI_App.cpp:2953-2956 | REMOVE | the `return run_headless(this)` |
| BRIDGE_SKIP_GUI_CTOR = is_bridge_only() | GUI_App.cpp:1445-1471 | MODIFY | collapse to `false` |
| run_headless fwd-decl + friend | GUI_App.hpp:81,403 | REMOVE | |
| g_bridge_only branch in GUI_Run | GUI_Init.cpp:13-15,48-69 | REMOVE | |
| run_headless() body + doc | BridgeBootstrap.cpp:269-982; hpp:59-79 | REMOVE | pure --bridge-only |
| is_bridge_only() | BridgeBootstrap.cpp:146-153; hpp:36-41 | REMOVE | only consumers removed |
| note_inbound/wait_for_inbound/g_inbound_* | BridgeBootstrap.cpp:182-209 | REMOVE | orphaned |
| install_gui_worker `if (g_bridge_only) return;` | BridgeBootstrap.cpp:996-1001 | MODIFY | drop guard, KEEP fn |
| g_bridge_only_cfg.only_dev_ids reads | BridgeBootstrap.cpp:1579,1874 | MODIFY | source from BAMBU_BRIDGE_TARGET_DEV |
| install_networking_callbacks bridge-only branch | BridgeBootstrap.cpp:2093-2260 | MODIFY | collapse to `app->init_networking_callbacks()` |
| shutdown_hooks `if (g_bridge_only) _Exit(0)` | BridgeBootstrap.cpp:2316-2320 | MODIFY | drop block, KEEP fn |
| BridgeOnlyConsoleApp.cpp/.hpp | whole files | REMOVE | never instantiated |
| install_print_dispatcher_resolver / install_mtls_resolver | BridgeBootstrap.cpp:83-144 | KEEP | shared by install_gui_worker |
| register_app_factory + wxIMPLEMENT scaffolding | BridgeBootstrap.cpp:211-267 | KEEP | IMPLEMENT_APP replacement |
| CloudSession/CloudDeviceList/region_for_bridge includes | BridgeBootstrap.cpp:29-30 | KEEP | shared |
| BridgeAppCliArgs.cpp/.hpp | bambu_bridge/headless/ | REMOVE | |
| BridgeLauncher.cpp/.hpp | bambu_bridge/headless/ | REMOVE | |
| bambu-studio-bridge console exe target | src/CMakeLists.txt:230-245 | KEEP + RENAME → `bambu-studio-console` | DECISION A: drop --bridge-only assoc; launch with BAMBU_BRIDGE_INVISIBLE_GUI=1 for redirectable logs |
| BridgeOnly* from libslic3r_gui sources | src/slic3r/CMakeLists.txt:715-717 | MODIFY | drop 3 lines |
| BridgeAppCliArgs/Launcher from bambu_bridge sources | src/bambu_bridge/CMakeLists.txt:140-143 | REMOVE | 4 lines |
| BridgeAppCliArgsTest target + .cpp | tests/bridge/CMakeLists.txt:49-60 | REMOVE | depends on deleted parser |
| host_drives_inventory false-branch | bambu_bridge/headless/BridgeApp.{cpp,hpp} | REMOVE (Option B) | DECISION B: bridge_cli retired → BridgeApp single-mode |
| bridge_cli.cpp + target | bambu_bridge/cli/, CMakeLists.txt:219-235 | REMOVE (Option B) | DECISION B |
| comment-only mentions | NetworkAgentPluginAdapter.*, BridgeStorageBackend.cpp:60-64, VirtualTunnelServer.*, docs | OPTIONAL | Phase 6 sweep |

## Phases (each independently buildable)

1. **Sever entry point** — GUI_App.cpp:2953 (drop run_headless branch); GUI_App.cpp:1445-1471 (BRIDGE_SKIP_GUI_CTOR → false); GUI_Init.cpp:13-15,48-69 (drop include + headless branch); BambuStudio.cpp:1485-1701 (drop includes, globals, whole CLI dispatch block). After this g_bridge_only is always-false (headless branch dead but compiles).
2. **Delete run_headless body + headless-only helpers** in BridgeBootstrap.cpp: drop `#include BridgeOnlyFlag.hpp` (17), is_bridge_only (146-153), note_inbound block (171-209), run_headless (269-982); in install_gui_worker drop `if(g_bridge_only)return` (998-1001); replace the two `g_bridge_only_cfg.only_dev_ids` reads (1579,1874) with a `BAMBU_BRIDGE_TARGET_DEV`-sourced local; collapse install_networking_callbacks (2093-2260) to just `app->init_networking_callbacks();` (this removes the last note_inbound caller); drop shutdown_hooks `_Exit` block (2316-2320). hpp: remove is_bridge_only + run_headless decls. GUI_App.hpp: remove run_headless fwd-decl(81)+friend(403).
3. **Remove console *app*; RENAME console *exe*** — delete BridgeOnlyConsoleApp.cpp/.hpp + BridgeOnlyFlag.hpp; src/slic3r/CMakeLists.txt:715-717 drop 3 BridgeOnly* lines. **DECISION A — KEEP the `bambu-studio-bridge` exe target but rename `OUTPUT_NAME "bambu-studio-console"`**, drop its `add_dependencies`/comment ties to --bridge-only (it's just a console-subsystem wrapper of the same DLL; run it with `BAMBU_BRIDGE_INVISIBLE_GUI=1`, no `--bridge-only`). Leave the VcpkgEnabled=false on both targets as-is (no hoist needed since the target stays). (mingw_compat_stubs.c — keep.)
4. **Remove BridgeAppCliArgs + BridgeLauncher** — delete the 4 files; src/bambu_bridge/CMakeLists.txt:140-143 drop 4 lines; tests/bridge/CMakeLists.txt:49-60 remove BridgeAppCliArgsTest target + delete its .cpp.
5. **DECISION B — retire bridge_cli + single-mode BridgeApp** — delete src/bambu_bridge/cli/bridge_cli.cpp + the `bridge_cli` target (src/bambu_bridge/CMakeLists.txt:219-235); drop the `host_drives_inventory` member (BridgeApp.hpp:254-257), the self-owned-plugin construction block (BridgeApp.cpp:450-465) and the m_plugin-gated CloudInventory (484-486), hard-wiring host-driven (invisible-GUI) mode; update BridgeAppLifecycleTest.cpp:102 (it set host_drives_inventory=false). Do NOT delete BambuNetworkingPluginHandle (still used by CloudInventory + uplinks + bridge tests). Land this as its OWN commit AFTER phases 1-4 are green — it touches the shared lib + tests.
6. **Orphan/comment/docs sweep** — reword comments in BridgeStorageBackend.cpp:60-64, NetworkAgentPluginAdapter.*, VirtualTunnelServer.*; update/delete docs + e2e scripts referencing `--bridge-only`/`bambu-studio-bridge`. NOTE: the PowerShell supervisor scripts (relaunch-*.ps1, bridge-multi-*.ps1) break once the flag+console exe are gone — rewrite them against the invisible-GUI launch (the invisible-GUI agent's deliverable).

## Key concrete diffs (excerpts)

GUI_App.cpp ~2953:
```
-#if defined(BAMBU_BRIDGE)
-    if (::Slic3r::GUI::BridgeBootstrap::is_bridge_only())
-        return ::Slic3r::GUI::BridgeBootstrap::run_headless(this);
-#endif
```
BridgeBootstrap.cpp install_networking_callbacks:
```
 void install_networking_callbacks(GUI_App* app) {
-    if (!g_bridge_only) { app->init_networking_callbacks(); return; }
-    // ...~160 lines bridge-only minimal callbacks...
+    app->init_networking_callbacks();
 }
```
only_dev_ids replacement (both sites, inside install_gui_worker — KEEP):
```
-    const auto& owned = Slic3r::GUI::g_bridge_only_cfg.only_dev_ids;
+    std::vector<std::string> owned;
+    if (const char* td = std::getenv("BAMBU_BRIDGE_TARGET_DEV"); td && *td) {
+        std::string v = td; auto c = v.find(',');
+        owned.push_back(c==std::string::npos ? v : v.substr(0,c));
+    }
```

## Decisions (RESOLVED 2026-06-04)

A. **Console exe → KEEP + RENAME `bambu-studio-console`.** Don't remove the target; rename `OUTPUT_NAME` and sever its --bridge-only ties. It's a console-subsystem wrapper of the same DLL, kept for redirectable stdout (the GUI exe uses CONOUT$, not redirectable). Launch with `BAMBU_BRIDGE_INVISIBLE_GUI=1`. Folded into Phase 3.

B. **bridge_cli → RETIRE (Option B).** Delete bridge_cli + its target and simplify `BridgeApp` to single (host-driven/invisible-GUI) mode. Folded into Phase 5 (now a real phase, landed as its own commit after 1-4 are green). `BambuNetworkingPluginHandle` stays (CloudInventory/uplinks/tests use it).

## Open questions
1. Console exe keep-vs-remove (Decision A).
2. bridge_cli retirement (Decision B).
3. BridgeBootstrap.cpp line drift — re-anchor on: `is_bridge_only` banner (~146), `g_inbound_mu` block (~182), `run_headless` banner (~269) through `m_initialized=true; return true;` (~982), `install_gui_worker` (~996), `install_networking_callbacks` (~2093), `shutdown_hooks` (~2277). The two only_dev_ids reads are in the `[bridge-refresh]` and `[bridge-gui] PROBE` blocks.
4. `m_initialized` is set in run_headless(982); normal path sets it at GUI_App.cpp:3508 — invisible-GUI loses nothing.
