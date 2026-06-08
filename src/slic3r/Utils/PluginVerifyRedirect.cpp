#include "PluginVerifyRedirect.hpp"

#ifdef _WIN32

#include <windows.h>
#include <wincrypt.h>
#include <intrin.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MinHook.h"

namespace {

// Resolved at install(): our own (forked, unsigned) BambuStudio.dll path, and
// the genuine official BambuStudio.dll we redirect the plugin's checks to.
wchar_t g_our_dll[MAX_PATH]   = {0};
wchar_t g_genuine_w[MAX_PATH] = {0};
char    g_genuine_a[MAX_PATH] = {0};

typedef DWORD (WINAPI *fn_gmfw)(HMODULE, LPWSTR, DWORD);
typedef DWORD (WINAPI *fn_gmfa)(HMODULE, LPSTR, DWORD);
typedef BOOL  (WINAPI *fn_cqo)(DWORD, const void*, DWORD, DWORD, DWORD,
                               DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);

fn_gmfw o_gmfw = nullptr;
fn_gmfa o_gmfa = nullptr;
fn_cqo  o_cqo  = nullptr;

// Is the return address inside the proprietary network plugin? (resolved lazily,
// since the plugin loads after we install.) Always uses the trampoline to avoid
// re-entering our own hook.
bool caller_is_plugin(void* ra)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ra, &h) || !h || !o_gmfw)
        return false;
    wchar_t p[MAX_PATH] = {0};
    o_gmfw(h, p, MAX_PATH);
    return wcsstr(p, L"bambu_networking") != nullptr || wcsstr(p, L"BambuSource") != nullptr;
}

bool is_our_dll_w(const wchar_t* p) { return p && g_our_dll[0] && _wcsicmp(p, g_our_dll) == 0; }
bool is_our_dll_a(const char* p)
{
    if (!p || !g_our_dll[0]) return false;
    wchar_t w[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH);
    return _wcsicmp(w, g_our_dll) == 0;
}

DWORD WINAPI hk_gmfw(HMODULE hMod, LPWSTR fn, DWORD sz)
{
    void* ra = _ReturnAddress();
    DWORD r = o_gmfw(hMod, fn, sz);
    if (fn && r && is_our_dll_w(fn) && caller_is_plugin(ra)) {
        size_t n = wcslen(g_genuine_w);
        if (sz > n) { wcscpy_s(fn, sz, g_genuine_w); return (DWORD)n; }
    }
    return r;
}

DWORD WINAPI hk_gmfa(HMODULE hMod, LPSTR fn, DWORD sz)
{
    void* ra = _ReturnAddress();
    DWORD r = o_gmfa(hMod, fn, sz);
    if (fn && r && is_our_dll_a(fn) && caller_is_plugin(ra)) {
        size_t n = strlen(g_genuine_a);
        if (sz > n) { strcpy_s(fn, sz, g_genuine_a); return (DWORD)n; }
    }
    return r;
}

BOOL WINAPI hk_cqo(DWORD ot, const void* obj, DWORD a, DWORD b, DWORD c,
                   DWORD* d, DWORD* e, DWORD* f, HCERTSTORE* st, HCRYPTMSG* msg, const void** ctx)
{
    const void* use = obj;
    if (ot == CERT_QUERY_OBJECT_FILE && obj &&
        is_our_dll_w((const wchar_t*)obj) && caller_is_plugin(_ReturnAddress()))
        use = g_genuine_w;
    return o_cqo(ot, use, a, b, c, d, e, f, st, msg, ctx);
}

} // namespace

namespace Slic3r {

void install_plugin_verify_redirect()
{
    static bool done = false;
    if (done) return;
    done = true;

    // Always on. The bridge's forked (unsigned) BambuStudio.dll always needs
    // this redirect for the plugin to accept the studio as genuine (works in
    // both the invisible-GUI bridge and a normal full GUI). If the genuine DLL
    // isn't on disk the install no-ops below regardless.
    const char* gp = std::getenv("BAMBU_BRIDGE_GENUINE_DLL");
    std::string gpath = (gp && *gp) ? gp : "C:\\Program Files\\Bambu Studio\\BambuStudio.dll";
    MultiByteToWideChar(CP_UTF8, 0, gpath.c_str(), -1, g_genuine_w, MAX_PATH);
    strncpy_s(g_genuine_a, sizeof(g_genuine_a), gpath.c_str(), _TRUNCATE);
    if (GetFileAttributesW(g_genuine_w) == INVALID_FILE_ATTRIBUTES) return;  // genuine DLL not present

    // our own module's on-disk path
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install_plugin_verify_redirect, &self);
    GetModuleFileNameW(self, g_our_dll, MAX_PATH);

    if (MH_Initialize() != MH_OK) return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE c32 = GetModuleHandleW(L"crypt32.dll");
    if (!c32) c32 = LoadLibraryW(L"crypt32.dll");
    if (k32) {
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameW"), (void*)hk_gmfw, (void**)&o_gmfw);
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameA"), (void*)hk_gmfa, (void**)&o_gmfa);
    }
    if (c32)
        MH_CreateHook((void*)GetProcAddress(c32, "CryptQueryObject"), (void*)hk_cqo, (void**)&o_cqo);
    MH_EnableHook(MH_ALL_HOOKS);
}

} // namespace Slic3r

#else  // !_WIN32

namespace Slic3r { void install_plugin_verify_redirect() {} }

#endif
