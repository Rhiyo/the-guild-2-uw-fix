/*
 * d3d9_proxy.c -- exports for the d3d9.dll proxy build (ReShade-style).
 *
 * Compiled with uwfix.c (DllMain + ultrawide patch logic).
 *
 * Why d3d9: this is a D3D9 game, and Proton already loads d3d9 as NATIVE
 * (DXVK/wined3d), so an app-local d3d9.dll in the game folder is picked up
 * with NO WINEDLLOVERRIDES and NO renamed files -- exactly how ReShade drops
 * in. d3d9 also has no other dependents in the process (only the game renders),
 * unlike winmm (dsound/fmod) or dbghelp (sentry.dll) which crashed when stubbed.
 *
 * The game statically imports only the no-op D3DPERF_* profiling funcs, and
 * loads Direct3DCreate9 dynamically. We stub D3DPERF_* and FORWARD
 * Direct3DCreate9 / Direct3DCreate9Ex to the real system d3d9 so rendering
 * works untouched.
 */

#include <windows.h>

extern void log_line(const char *s);    /* from uwfix.c */
extern void bp_game_flush(void);        /* from uwfix.c -- game-thread one-shot */

/* ---- Game-thread pump via D3D9 Present hook -------------------------------
 * The bottom-right ButtonPanelRight fix calls the engine's SetValueInt, which
 * mutates UI state and can relayout -- it MUST run on the game (render) thread,
 * not our worker thread, or it can race the renderer and crash intermittently.
 * IDirect3DDevice9::Present is called once per frame on the game thread at a
 * clean frame boundary (after layout + render), so we vtable-hook it and let
 * bp_game_flush() apply the fix there, exactly once. This is fully independent
 * of the head/CharacterPanel fix.
 *
 * We reach the device by vtable-hooking IDirect3D9::CreateDevice (index 16) on
 * the object we hand back from Direct3DCreate9; in that hook we vtable-hook the
 * returned device's Present (index 17). Standard COM (__stdcall, explicit this).
 */
typedef long (WINAPI *PFN_Present)(void *self, const void *src, const void *dst,
                                   void *hwnd, const void *dirty);
typedef long (WINAPI *PFN_CreateDevice)(void *self, unsigned adapter, int devtype,
                                        void *hwnd, unsigned long flags,
                                        void *pp, void **ppDevice);

static PFN_Present      real_Present      = NULL;
static PFN_CreateDevice real_CreateDevice = NULL;

/* Patch one vtable slot, keeping the page executable. One-shot per `saved`. */
static void patch_slot(void *iface, int idx, void *hook, void **saved) {
    if (iface == NULL || *saved != NULL) return;
    void **vtbl = *(void ***)iface;
    DWORD old;
    if (!VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return;
    *saved = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}

static long WINAPI my_Present(void *self, const void *a, const void *b,
                              void *c, const void *d) {
    bp_game_flush();                          /* one-shot, game thread, between frames */
    return real_Present(self, a, b, c, d);
}

static long WINAPI my_CreateDevice(void *self, unsigned adapter, int devtype,
                                   void *hwnd, unsigned long flags,
                                   void *pp, void **ppDevice) {
    long hr = real_CreateDevice(self, adapter, devtype, hwnd, flags, pp, ppDevice);
    if (hr >= 0 && ppDevice && *ppDevice && real_Present == NULL) {
        patch_slot(*ppDevice, 17, (void *)my_Present, (void **)&real_Present);
        log_line(real_Present ? "[uw] d3d9: Present hook installed (game-thread pump)\n"
                              : "[uw] d3d9: Present hook FAILED\n");
    }
    return hr;
}

/* Hook CreateDevice (index 16) on the IDirect3D9 / IDirect3D9Ex we return. */
static void hook_d3d9_object(void *obj) {
    if (obj == NULL) return;
    patch_slot(obj, 16, (void *)my_CreateDevice, (void **)&real_CreateDevice);
    log_line("[uw] d3d9: CreateDevice hook installed\n");
}

typedef void* (WINAPI *pDirect3DCreate9)(UINT);
typedef long  (WINAPI *pDirect3DCreate9Ex)(UINT, void**);

static pDirect3DCreate9   real_Create9   = NULL;
static pDirect3DCreate9Ex real_Create9Ex = NULL;
static int real_loaded = 0;

/* Load the REAL d3d9 from system32, avoiding ourselves. LOAD_LIBRARY_SEARCH_
   SYSTEM32 forces the system copy over our app-local one. */
static void load_real_d3d9(void) {
    if (real_loaded) return;
    real_loaded = 1;
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 12) { log_line("[uw] d3d9: GetSystemDirectory failed\n"); return; }
    lstrcatW(sys, L"\\d3d9.dll");
    HMODULE self = GetModuleHandleW(L"d3d9.dll");           /* us */
    HMODULE real = LoadLibraryExW(sys, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (real == NULL) real = LoadLibraryW(sys);             /* fallback */
    if (real == NULL) { log_line("[uw] d3d9: could not load real d3d9\n"); return; }
    if (real == self) { log_line("[uw] d3d9: real load returned SELF (circular) -- forwarding broken\n"); return; }
    real_Create9   = (pDirect3DCreate9)  (void*)GetProcAddress(real, "Direct3DCreate9");
    real_Create9Ex = (pDirect3DCreate9Ex)(void*)GetProcAddress(real, "Direct3DCreate9Ex");
    log_line(real_Create9 ? "[uw] d3d9: real Direct3DCreate9 resolved\n"
                          : "[uw] d3d9: real Direct3DCreate9 NOT found\n");
}

__declspec(dllexport) void* WINAPI Direct3DCreate9(UINT sdk) {
    if (!real_loaded) load_real_d3d9();
    void *d3d = real_Create9 ? real_Create9(sdk) : NULL;
    hook_d3d9_object(d3d);              /* install CreateDevice -> Present pump */
    return d3d;
}
__declspec(dllexport) long WINAPI Direct3DCreate9Ex(UINT sdk, void **ppD3D) {
    if (!real_loaded) load_real_d3d9();
    if (real_Create9Ex) {
        long hr = real_Create9Ex(sdk, ppD3D);
        if (hr >= 0 && ppD3D) hook_d3d9_object(*ppD3D);
        return hr;
    }
    if (ppD3D) *ppD3D = NULL;
    return (long)0x8007000EL;   /* E_OUTOFMEMORY-ish: signal failure */
}

/* D3DPERF_* are profiling no-ops; stubbing them is exactly what a release
   D3D runtime does. Signatures match d3d9.h for correct __stdcall cleanup. */
__declspec(dllexport) int   WINAPI D3DPERF_BeginEvent(DWORD col, const wchar_t *n) { (void)col;(void)n; return -1; }
__declspec(dllexport) int   WINAPI D3DPERF_EndEvent(void)                          { return -1; }
__declspec(dllexport) void  WINAPI D3DPERF_SetMarker(DWORD col, const wchar_t *n)  { (void)col;(void)n; }
__declspec(dllexport) void  WINAPI D3DPERF_SetRegion(DWORD col, const wchar_t *n)  { (void)col;(void)n; }
__declspec(dllexport) BOOL  WINAPI D3DPERF_QueryRepeatFrame(void)                  { return FALSE; }
__declspec(dllexport) void  WINAPI D3DPERF_SetOptions(DWORD opts)                  { (void)opts; }
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void)                         { return 0; }
