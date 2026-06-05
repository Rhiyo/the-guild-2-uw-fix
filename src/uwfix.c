/*
 * steambridge_proxy.c — Guild II Renaissance ultrawide HUD fix
 * Proxies SteamBridge.dll -> SteamBridge_orig.dll
 *
 * Deploy:
 *   1. Rename SteamBridge.dll -> SteamBridge_orig.dll  (in game root)
 *   2. Copy this build output (SteamBridge.dll) to game root
 *
 * No WINEDLLOVERRIDES needed — SteamBridge.dll is a local game DLL,
 * Proton/Wine loads the local one automatically. All SB_* exports are
 * forwarded via the .def file; DllMain just starts our thread.
 *
 * ----------------------------------------------------------------
 * v16 — render-view X fix, resolution-correct at draw time.
 *
 *   cl_CharactersPanel positions its 3D head via the rect stored at
 *   [obj+0xcc] (path B), recomputed every layout pass by the store at
 *   0x004ae085 (`mov [esi+0xcc], edx`). We detour the store site (0x004ae082)
 *   into a code cave that right-anchors the X.
 *
 *   v15 baked offset = GetSystemMetrics(desktop) - 1024 once at startup. That
 *   broke whenever the in-game render resolution != desktop (windowed or a
 *   non-native fullscreen res): the head used the wrong width while the Lua
 *   bottom-right fix (which reads the game's own ScreenWidth) stayed correct.
 *
 *   v16: the cave reads the ENGINE canvas width ([*0xB69950 + 0xDC]) at draw
 *   time and adds (canvasWidth - 1024) to the X. Same coordinate space as the
 *   value being adjusted, so it is correct at every resolution AND adapts if
 *   the resolution changes. Self-disabling at 1024-wide (offset 0), so it is
 *   safe to install unconditionally (no ultrawide gate, no 4:3 side effects).
 *
 * v14 — DIAGNOSTIC ONLY (read-only, no patching).
 *
 *   Goal: find out exactly how cl_CharactersPanel positions its 3D
 *   character render view(s), which Lua/ABS_X cannot move.
 *
 *   From static RE of GuildII.exe (base 0x00400000):
 *     - cl_CharactersPanel vtable = 0x00A27B4C  (object[0] == this VA)
 *     - The panel is a GUI node: its container rect X lives at [obj+0x64].
 *     - slot38 (layout, 0x4af540) positions the render view each frame via
 *       SetViewport, X taken from EITHER a literal 123 (path A) OR the
 *       stored rect [obj+0xcc..+0xd8] (path B, filled by 0x4ae060).
 *
 *   This build walks committed heap memory, finds every object whose first
 *   dword equals the (relocation-adjusted) cl_CharactersPanel vtable, and
 *   dumps the telling fields:
 *       +0x58 content ptr, +0x5c child-container ptr,
 *       +0x64 container X (did the Lua ABS_X write reach it?),
 *       +0xc8..+0xd8 render rect (c8,X,Y,W,H),
 *       +0x184/+0x188/+0x18c/+0x190 per-character array state,
 *   plus a raw dword window. Nothing is written — purely observational.
 * ----------------------------------------------------------------
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
   GuildII.exe constants (addresses assume preferred base 0x00400000;
   we add the runtime relocation delta below).
   ================================================================ */
#ifndef DLL_VERSION                    /* CI stamps this from the release tag */
#define DLL_VERSION           "dev"
#endif
#define PREFERRED_BASE        0x00400000u
#define CP_VTABLE_VA          0x00A27B4Cu   /* cl_CharactersPanel vtable */
#define BP_VTABLE_VA          0x00A274C4u   /* cl_ButtonPanel vtable (bottom-right) */

/* Canvas (the engine's render-space width lives here, NOT the desktop size).
   Singleton getter 0x5967e0 reads this global; GetWidth 0x594920 = [canvas+0xDC]. */
#define CANVAS_GLOBAL_VA      0x00B69950u
#define CANVAS_WIDTH_OFF      0xDCu

#define BP_GATE_W             162           /* ButtonPanelRight ABS_WIDTH */
#define BP_GATE_H             210           /* ButtonPanelRight ABS_HEIGHT*/

/* Object field offsets (relative — relocation-independent). */
#define OFF_CONTENT           0x58u
#define OFF_CHILDCONT         0x5Cu
#define OFF_CONTAINER_X       0x64u   /* GUI-node rect X (ABS_X target)     */
#define OFF_CONTAINER_Y       0x68u
#define OFF_RECT_C8           0xC8u
#define OFF_RECT_X            0xCCu   /* render-view rect X (path B)        */
#define OFF_RECT_Y            0xD0u
#define OFF_RECT_W            0xD4u
#define OFF_RECT_H            0xD8u
#define OFF_ARR_CNT           0x184u
#define OFF_ARR_IDX           0x188u
#define OFF_ARR_DIRTY         0x18Cu
#define OFF_ARR_KEY           0x190u

/* Render-view X store site (path B) and the bytes we expect there:
     0x004ae082: 8B 50 08              mov edx,[eax+8]
     0x004ae085: 89 96 CC 00 00 00     mov [esi+0xcc],edx     (X store)
     0x004ae08b: (resume)              mov ecx,[eax+0xc] ...               */
#define HEAD_STORE_VA         0x004AE082u
#define HEAD_RESUME_VA        0x004AE08Bu
#define DESIGN_WIDTH          1024

#define MAX_RUNTIME_MS        (20u * 60u * 1000u)   /* 20 minutes */
#define POLL_MS               2000u
#define MAX_DUMPS_PER_PASS    4
#define HEARTBEAT_PASSES      15                    /* ~30s with POLL 2s   */

static uint32_t g_cp_vtable = CP_VTABLE_VA;   /* adjusted for relocation   */
static uint32_t g_bp_vtable = BP_VTABLE_VA;   /* adjusted for relocation   */
static intptr_t g_delta     = 0;              /* runtime relocation delta  */
static volatile uint32_t g_bp_total = 0;   /* slot56 calls seen by the cave */
static volatile uint32_t g_bp_match = 0;   /* calls that matched W=162,H=210 */
static volatile uint32_t g_bp_node  = 0;   /* esi (Container node) at a match */
static volatile int32_t  g_bp_absx  = 0x7FFFFFFF;  /* captured ABS_X arg     */
static volatile int32_t  g_bp_absy  = 0x7FFFFFFF;  /* captured ABS_Y arg     */

/* uw_fix.ini settings (defaults = everything on, quiet-ish). */
static int g_do_head = 1;   /* [UltrawideFix] CharacterPanel   */
static int g_do_bp   = 1;   /* [UltrawideFix] ButtonPanelRight  */
static int g_logging = 1;   /* [UltrawideFix] Logging           */
static int g_verbose = 0;   /* [UltrawideFix] Verbose (heavy dumps) */

/* Resolved at runtime by scanning the loaded exe (.text) for byte signatures,
   so the fix is exe-version-independent (addresses move between builds). */
static uint32_t g_canvas_global = 0;     /* &canvas-object-ptr global */
static uint8_t *g_text_base = NULL;
static size_t   g_text_size = 0;
static uintptr_t g_image_base = 0;       /* whole mapped module (for string scans) */
static uint32_t  g_image_size = 0;
static uint32_t g_setvalueint = 0;       /* resolved SetValueInt addr (0 = unresolved) */

/* ================================================================
   Logging
   ================================================================ */
void log_line(const char *s) {   /* non-static: shared with d3d9_exports.c */
    if (!g_logging) return;
    HANDLE h = CreateFileA("ultrawide_fix.log", GENERIC_WRITE,
                           FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w; WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
    CloseHandle(h);
}

static void log_fmt(const char *fmt, ...) {
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    log_line(buf);
}

/* ================================================================
   Memory helpers (read-only; our own process)
   ================================================================ */
static BOOL mem_ok(const void *p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return ((SIZE_T)mbi.BaseAddress + mbi.RegionSize - (SIZE_T)p) >= n;
}

static uint32_t rd32(const uint8_t *obj, uint32_t off) {
    const uint8_t *p = obj + off;
    if (!mem_ok(p, 4)) return 0xBADD0000u;
    return *(const uint32_t*)p;
}

/* ----------------------------------------------------------------
   Runtime signature scanning over the loaded exe's .text section, so
   patch sites don't depend on a specific build's addresses.
   ---------------------------------------------------------------- */
static void find_text_section(uintptr_t base) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_image_base = base;                                  /* whole mapped module */
    g_image_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(s[i].Name, ".text", 5) == 0) {
            g_text_base = (uint8_t*)(base + s[i].VirtualAddress);
            g_text_size = s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize
                                                : s[i].SizeOfRawData;
            return;
        }
    }
}

/* Find the nth (0-based) occurrence of sig in .text; NULL if not found. */
static uint8_t *scan_text(const uint8_t *sig, size_t len, int nth) {
    if (!g_text_base) return NULL;
    int seen = 0;
    for (size_t i = 0; i + len <= g_text_size; i++) {
        if (memcmp(g_text_base + i, sig, len) == 0) {
            if (seen++ == nth) return g_text_base + i;
        }
    }
    return NULL;
}

/* Canvas-object-ptr global: singleton getter does
   A1 <glob> 85 C0 75 <rel8> 68 28 01 00 00  (mov eax,[glob];test;jne;push 0x128). */
static uint32_t find_canvas_global(void) {
    if (!g_text_base) return 0;
    static const uint8_t tail[] = { 0x85,0xC0,0x75 };
    for (size_t i = 5; i + 9 <= g_text_size; i++) {
        if (g_text_base[i-5] != 0xA1) continue;
        if (memcmp(g_text_base + i, tail, 3) != 0) continue;
        const uint8_t *p = g_text_base + i + 4;          /* after 85 C0 75 rel8 */
        if (p[0]==0x68 && p[1]==0x28 && p[2]==0x01 && p[3]==0x00 && p[4]==0x00) {
            uint32_t glob; memcpy(&glob, g_text_base + (i-4), 4);
            return glob;
        }
    }
    return 0;
}

/* The exact value the head cave uses: *(*(g_canvas_global) + 0xDC).
   Returns negative sentinels if the global/canvas isn't ready. */
static int32_t canvas_width(void) {
    const uint8_t *pg = (const uint8_t*)(uintptr_t)g_canvas_global;
    if (!mem_ok(pg, 4)) return -1;
    uint32_t canvas = *(const uint32_t*)pg;
    if (canvas == 0) return -2;
    const uint8_t *pw = (const uint8_t*)(canvas + CANVAS_WIDTH_OFF);
    if (!mem_ok(pw, 4)) return -3;
    return (int32_t)(*(const uint32_t*)pw);
}

/* Dump canvas object fields so we can identify the LOGICAL (ScreenWidth)
   width vs the physical [+0xDC], plus the scale factor near [+0xF8]. */
static void dump_canvas(void) {
    const uint8_t *pg = (const uint8_t*)(uintptr_t)g_canvas_global;
    if (!mem_ok(pg, 4)) { log_line("[uw] canvas: global unreadable\n"); return; }
    uint32_t c = *(const uint32_t*)pg;
    if (c == 0 || !mem_ok((const void*)(uintptr_t)c, 0x110)) {
        log_fmt("[uw] canvas ptr=%08X not ready\n", c); return;
    }
    char line[512]; line[0] = 0;
    for (uint32_t off = 0xD0; off <= 0x108; off += 4) {
        uint32_t iv = *(const uint32_t*)(uintptr_t)(c + off);
        float fv; memcpy(&fv, &iv, 4);
        char t[48];
        snprintf(t, sizeof t, "%02X=%d(%.3f) ", off, (int32_t)iv, (double)fv);
        strncat(line, t, sizeof(line) - strlen(line) - 1);
    }
    log_fmt("[uw] canvas @%08X  %s\n", c, line);
}

static int32_t canvas_height(void) {
    const uint8_t *pg = (const uint8_t*)(uintptr_t)g_canvas_global;
    if (!mem_ok(pg, 4)) return -1;
    uint32_t c = *(const uint32_t*)pg;
    if (c == 0 || !mem_ok((const void*)(uintptr_t)(c + 0xE0), 4)) return -1;
    return (int32_t)(*(const uint32_t*)(uintptr_t)(c + 0xE0));
}

/* THE fix for the bottom-right panel: right/bottom-anchor the RESOLVED node
   rect (the cave only captured the node). The resolver published the panel's
   rect into several identical [.,.,X,Y,162,210] blocks (stride 0x18, first at
   node+0x64). We rewrite X = canvasW-W and Y = canvasH-H in EVERY block whose
   W/H == 162/210 (so the panel AND its clip/bounds rects all move together,
   else the panel would be clipped by a stale bounds rect). This writes the
   post-resolution rect, which the renderer uses verbatim -- bypassing the
   ~1365px virtual-canvas clip that culls a large ABS_X at resolve time. Y ends
   up == its current value (the engine already lands it at physH-H), so the Y
   write is idempotent. Guarded against implausible coordinates. */
/* Engine's SetValueInt(this=node, key, value) -- the exact setter the working
   Lua used (`child:SetValueInt("ABS_X", sw-w)`). __thiscall, args on stack,
   callee-cleaned (ret 8). We model __thiscall as fastcall with a dummy edx:
   fastcall puts arg0 in ecx (=this), arg1 in edx (ignored by the callee),
   and pushes the rest -- giving exactly ecx=node, [esp]=key, [esp+4]=value. */
#define SETVALUEINT_VA 0x00794FF0u      /* fallback addr (preferred base) only  */
typedef int (__attribute__((fastcall)) *SetValIntFn)(uint32_t node, uint32_t edx,
                                                     const char *key, int value);

/* SetValueInt's prologue. NOT unique on its own (the SetValue<T> family shares
   it), so it's only used to VALIDATE a resolved address before we ever call it
   -- a wrong/changed build fails the check and we skip safely instead of
   jumping into arbitrary code. */
static const uint8_t SVI_PROLOG[10] =
    { 0x83,0xEC,0x20, 0x8B,0x44,0x24,0x24, 0x56, 0x8B,0xF1 };

static int svi_prologue_ok(uint32_t addr) {
    return addr && mem_ok((void*)(uintptr_t)addr, sizeof SVI_PROLOG) &&
           memcmp((void*)(uintptr_t)addr, SVI_PROLOG, sizeof SVI_PROLOG) == 0;
}

/* Resolve SetValueInt robustly so it tracks across game builds instead of
   trusting a hardcoded address. Anchor on the "SetValueInt" string -> its Lua
   registration in .text (`68 <wrapper> 68 <strVA> 56`) -> the wrapper's final
   `call <real>` right before its `pop edi/ebp/ebx` epilogue (E8 rel32 5F 5D 5B).
   Validate the prologue; fall back to the hardcoded address (also validated).
   Returns 0 if nothing validates -> the bottom-right fix then skips safely. */
static uint32_t find_setvalueint(void) {
    uint32_t cand = 0;
    /* 1. locate the "SetValueInt\0" string anywhere in the mapped module. */
    static const char KEY[] = "SetValueInt";
    uint32_t strva = 0;
    if (g_image_base) {
        for (uint32_t i = 0; i + sizeof KEY <= g_image_size; i++) {
            if (memcmp((uint8_t*)(g_image_base + i), KEY, sizeof KEY) == 0) {
                strva = (uint32_t)(g_image_base + i); break;
            }
        }
    }
    /* 2. find the registration push pair, read the wrapper function ptr. */
    if (strva && g_text_base) {
        uintptr_t tlo = (uintptr_t)g_text_base, thi = tlo + g_text_size;
        for (size_t i = 0; i + 11 <= g_text_size; i++) {
            uint8_t *p = g_text_base + i;
            if (p[0] != 0x68 || p[5] != 0x68 || p[10] != 0x56) continue;
            uint32_t s2; memcpy(&s2, p + 6, 4);
            if (s2 != strva) continue;
            uint32_t wrapper; memcpy(&wrapper, p + 1, 4);
            if (wrapper < tlo || wrapper >= thi) continue;
            /* 3. scan the wrapper for `E8 rel32 5F 5D 5B` -> the real call. */
            uint8_t *w = (uint8_t*)(uintptr_t)wrapper;
            for (size_t j = 0; j < 0x300 && (uintptr_t)(w + j + 9) <= thi; j++) {
                if (w[j]==0xE8 && w[j+5]==0x5F && w[j+6]==0x5D && w[j+7]==0x5B) {
                    int32_t rel; memcpy(&rel, w + j + 1, 4);
                    cand = (uint32_t)(uintptr_t)(w + j + 5) + (uint32_t)rel;
                    break;
                }
            }
            if (cand) break;
        }
    }
    if (svi_prologue_ok(cand)) return cand;
    uint32_t fb = (uint32_t)(SETVALUEINT_VA + g_delta);   /* validated fallback */
    if (svi_prologue_ok(fb)) return fb;
    return 0;
}

/* THE fix: replicate the proven Lua. cl_ButtonPanel honors ABS_X via the
   property system, so setting ABS_X = screenW - W (and ABS_Y = screenH - H)
   moves it to the physical right edge -- no virtual-canvas clip, because this
   goes through the property setter + relayout, not the resolver args or a raw
   rect poke (both of which failed/disappeared).

   Thread model: SetValueInt mutates UI state + can trigger relayout, so it MUST
   run on the game thread to avoid racing the renderer. It is applied from
   bp_game_flush(), called once per frame by the d3d9 Present hook (between
   frames, game thread) -- so it lands the first frame the panel exists, with no
   poll delay. Independent of the head/CharacterPanel fix. If no Present pump
   exists (e.g. a non-d3d9 build) the worker applies it after a grace period. */
static volatile int g_bp_done = 0;      /* SetValueInt applied */

static void bp_set_now(const char *who) {
    if (g_bp_done || g_bp_node == 0 || g_setvalueint == 0) return;
    int32_t cw = canvas_width(), ch = canvas_height();
    if (cw <= 1400 || ch <= 0) return;              /* wait for ultrawide canvas */
    uint8_t *nd = (uint8_t*)(uintptr_t)g_bp_node;
    if (!mem_ok(nd, 0x80)) return;
    int nx = cw - BP_GATE_W, ny = ch - BP_GATE_H;
    g_bp_done = 1;                       /* set first: avoid a double-fire race  */
    SetValIntFn SetValueInt = (SetValIntFn)(uintptr_t)g_setvalueint;
    SetValueInt(g_bp_node, 0, "ABS_X", nx);
    SetValueInt(g_bp_node, 0, "ABS_Y", ny);
    log_fmt("[uw] bp SetValueInt ABS_X=%d ABS_Y=%d on node %08X via %08X (%s)\n",
            nx, ny, g_bp_node, g_setvalueint, who);
}

/* Called every frame from the d3d9 Present hook -> game thread, between frames.
   The race-free path, decoupled from CharacterPanel; applies as soon as the
   node is captured and the canvas is ultrawide. */
void bp_game_flush(void) { bp_set_now("game-thread"); }

#define BP_PUMP_GRACE 3                  /* worker polls before the fallback fires */
static void bp_apply(void) {
    if (g_bp_done || g_bp_node == 0) return;
    static int grace = 0;                /* give the Present pump time to land */
    if (++grace >= BP_PUMP_GRACE) bp_set_now("worker-fallback");
}

/* ================================================================
   Dump one candidate cl_CharactersPanel instance.
   ================================================================ */
static void dump_object(const uint8_t *obj) {
    int32_t cw = canvas_width();
    log_fmt("[uw] CP @%p  vtbl=%08X  canvasW=%d  expect head X[+cc]~%d\n",
            (const void*)obj, rd32(obj, 0), cw, (cw > 0 ? cw - 150 : cw));
    log_fmt("[uw]   content[+58]=%08X childCont[+5C]=%08X\n",
            rd32(obj, OFF_CONTENT), rd32(obj, OFF_CHILDCONT));
    log_fmt("[uw]   containerX[+64]=%d  containerY[+68]=%d\n",
            (int32_t)rd32(obj, OFF_CONTAINER_X),
            (int32_t)rd32(obj, OFF_CONTAINER_Y));
    log_fmt("[uw]   renderRect c8=%d X[+cc]=%d Y[+d0]=%d W[+d4]=%d H[+d8]=%d\n",
            (int32_t)rd32(obj, OFF_RECT_C8),
            (int32_t)rd32(obj, OFF_RECT_X),
            (int32_t)rd32(obj, OFF_RECT_Y),
            (int32_t)rd32(obj, OFF_RECT_W),
            (int32_t)rd32(obj, OFF_RECT_H));
    log_fmt("[uw]   arr cnt[+184]=%d idx[+188]=%d dirty[+18c]=%d key[+190]=%08X\n",
            (int32_t)rd32(obj, OFF_ARR_CNT),
            (int32_t)rd32(obj, OFF_ARR_IDX),
            (int32_t)rd32(obj, OFF_ARR_DIRTY),
            rd32(obj, OFF_ARR_KEY));

    /* raw window 0x60..0xE0 so we can spot the X anywhere in the rect block */
    char line[400]; line[0] = 0;
    for (uint32_t off = 0x60; off <= 0xE0; off += 4) {
        char t[24];
        snprintf(t, sizeof t, "%02X:%d ", off, (int32_t)rd32(obj, off));
        strncat(line, t, sizeof(line) - strlen(line) - 1);
    }
    log_fmt("[uw]   raw %s\n", line);
}

/* Dump a cl_ButtonPanel instance (generic node layout: rect at +0x64..+0x70).
   We expect ButtonPanelRight to read X~862,W~162 and ButtonPanelLeft X~0. */
static void dump_bp(const uint8_t *obj) {
    log_fmt("[uw] BP @%p  rect X[+64]=%d Y[+68]=%d W[+6c]=%d H[+70]=%d\n",
            (const void*)obj,
            (int32_t)rd32(obj, 0x64), (int32_t)rd32(obj, 0x68),
            (int32_t)rd32(obj, 0x6c), (int32_t)rd32(obj, 0x70));
    char line[400]; line[0] = 0;
    for (uint32_t off = 0x58; off <= 0x90; off += 4) {
        char t[24];
        snprintf(t, sizeof t, "%02X:%d ", off, (int32_t)rd32(obj, off));
        strncat(line, t, sizeof(line) - strlen(line) - 1);
    }
    log_fmt("[uw]   raw %s\n", line);
}

/* ================================================================
   Scan committed RW heap for cl_CharactersPanel instances.
   ================================================================ */
static int scan_pass(void) {
    static uint8_t buf[65536];
    HANDLE self = GetCurrentProcess();
    uint8_t *addr = (uint8_t*)0x00010000u;
    MEMORY_BASIC_INFORMATION mbi;
    int found = 0, dumped = 0, bpdumped = 0;

    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        BOOL rw = (mbi.State == MEM_COMMIT) &&
                  (mbi.Protect == PAGE_READWRITE ||
                   mbi.Protect == PAGE_EXECUTE_READWRITE);
        if (rw) {
            uint8_t *base = (uint8_t*)mbi.BaseAddress;
            SIZE_T   sz   = mbi.RegionSize;
            for (SIZE_T off = 0; off < sz; off += sizeof(buf)) {
                SIZE_T chunk = sz - off;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                SIZE_T nread = 0;
                if (!ReadProcessMemory(self, base+off, buf, chunk, &nread) ||
                    nread < 4) continue;
                for (SIZE_T i = 0; i + 4 <= nread; i += 4) {
                    uint32_t v; memcpy(&v, buf+i, 4);
                    if (v == g_cp_vtable) {
                        found++;
                        const uint8_t *obj = base + off + i;
                        /* sanity: a real object should have a plausible
                           content ptr at +0x58 (committed, non-null). */
                        uint32_t content = rd32(obj, OFF_CONTENT);
                        if (content == 0xBADD0000u || content == 0) continue;
                        if (dumped < MAX_DUMPS_PER_PASS) {
                            dump_object(obj);
                            dumped++;
                        }
                    } else if (v == g_bp_vtable) {
                        const uint8_t *obj = base + off + i;
                        if (bpdumped < 6) { dump_bp(obj); bpdumped++; }
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

/* ================================================================
   Install the render-view X offset (code-cave hook at the store site).
   Adds `offset` to the X just before it is written to [esi+0xcc], so the
   3D head is recomputed each frame at the physical right edge.
   ================================================================ */
static const uint8_t HEAD_ORIG[9] =
    { 0x8B,0x50,0x08, 0x89,0x96,0xCC,0x00,0x00,0x00 };

static int install_head_patch(uint8_t *site, uint32_t canvasg) {
    uint8_t *resume = site + 9;   /* after mov edx,[eax+8]; mov [esi+0xcc],edx */

    if (!mem_ok(site, 9)) { log_line("[uw] head: site unreadable\n"); return 0; }
    if (memcmp(site, HEAD_ORIG, 9) != 0) {
        log_fmt("[uw] head: byte mismatch %02x %02x %02x %02x %02x %02x; abort\n",
                site[0],site[1],site[2],site[3],site[4],site[5]);
        return 0;
    }

    uint8_t *cave = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT|MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) { log_line("[uw] head: VirtualAlloc failed\n"); return 0; }

    /* Cave — right-anchor X using the live canvas width:
         8B 50 08            mov edx,[eax+8]        ; original X
         8B 0D <canvasg>     mov ecx,[canvasGlobal] ; canvas object ptr
         85 C9               test ecx,ecx
         74 0E               jz  +14  -> store      ; null canvas: leave X as-is
         8B 89 DC 00 00 00   mov ecx,[ecx+0xDC]     ; canvas width
         81 E9 00 04 00 00   sub ecx, 1024          ; offset = width - design
         01 CA               add edx, ecx
       store:
         89 96 CC 00 00 00   mov [esi+0xcc], edx
         E9 <rel32>          jmp resume
       ecx is free here (the engine reloads it at the resume point); eax/esi
       are preserved (subsequent stores read [eax+0xc/0x10/0x14]).            */
    int n = 0;
    cave[n++]=0x8B; cave[n++]=0x50; cave[n++]=0x08;                 /* mov edx,[eax+8]      */
    cave[n++]=0x8B; cave[n++]=0x0D; memcpy(cave+n,&canvasg,4); n+=4;/* mov ecx,[canvasg]    */
    cave[n++]=0x85; cave[n++]=0xC9;                                 /* test ecx,ecx         */
    cave[n++]=0x74; cave[n++]=0x0E;                                 /* jz +14 -> store      */
    cave[n++]=0x8B; cave[n++]=0x89; cave[n++]=0xDC; cave[n++]=0x00;
    cave[n++]=0x00; cave[n++]=0x00;                                 /* mov ecx,[ecx+0xDC]   */
    cave[n++]=0x81; cave[n++]=0xE9; cave[n++]=0x00; cave[n++]=0x04;
    cave[n++]=0x00; cave[n++]=0x00;                                 /* sub ecx,1024         */
    cave[n++]=0x01; cave[n++]=0xCA;                                 /* add edx,ecx          */
    cave[n++]=0x89; cave[n++]=0x96; cave[n++]=0xCC; cave[n++]=0x00;
    cave[n++]=0x00; cave[n++]=0x00;                                 /* mov [esi+0xcc],edx   */
    cave[n++]=0xE9;
    int32_t relback = (int32_t)((uintptr_t)resume - ((uintptr_t)(cave+n)+4));
    memcpy(cave+n,&relback,4); n+=4;                                /* jmp resume           */

    /* Site: E9 <rel32 to cave> + 4x NOP (fills the 9 original bytes). */
    uint8_t patch[9];
    patch[0]=0xE9;
    int32_t reljmp = (int32_t)((uintptr_t)cave - ((uintptr_t)site+5));
    memcpy(patch+1,&reljmp,4);
    patch[5]=patch[6]=patch[7]=patch[8]=0x90;

    DWORD prot;
    if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &prot)) {
        log_line("[uw] head: VirtualProtect failed\n"); return 0;
    }
    memcpy(site, patch, 9);
    VirtualProtect(site, 9, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 9);

    log_fmt("[uw] head patch OK: site=%p cave=%p(%dB) canvasGlobal=%08X (dynamic width)\n",
            (void*)site,(void*)cave,n,canvasg);
    return 1;
}

/* ================================================================
   Bottom-right ButtonPanelRight fix.
   cl_ButtonPanel has no class-specific geometry setter; its position is
   produced by the generic geometry resolver, which calls slot56 with
   (this, ABS_X, ABS_Y, ABS_WIDTH, ABS_HEIGHT) on the stack at 0x005B5B76.
   We detour that call, gate on the panel's distinctive size (W=162,H=210 --
   stable across vanilla and the modpack, unlike its position value), and
   right/bottom-anchor X/Y using the live canvas. Every other element hits a
   two-compare no-op, so the effective blast radius is just this panel.
   ================================================================ */
#define BP_HOOK_VA            0x005B5B76u   /* call [eax+0xe0] (slot56)   */
#define BP_RESUME_VA          0x005B5B7Cu
static const uint8_t BP_ORIG[6] = { 0xFF,0x90,0xE0,0x00,0x00,0x00 };

static int install_bp_patch(uint8_t *site, uint32_t canvasg) {
    uint8_t *resume = site + 6;   /* after call [eax+0xe0] */

    if (!mem_ok(site, 6)) { log_line("[uw] bp: site unreadable\n"); return 0; }
    if (memcmp(site, BP_ORIG, 6) != 0) {
        log_fmt("[uw] bp: byte mismatch %02x %02x %02x; abort\n",
                site[0], site[1], site[2]);
        return 0;
    }
    uint8_t *cave = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT|MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) { log_line("[uw] bp: VirtualAlloc failed\n"); return 0; }

    int n = 0; uint32_t imm;
    uint32_t totaddr=(uint32_t)(uintptr_t)&g_bp_total;
    uint32_t mataddr=(uint32_t)(uintptr_t)&g_bp_match;
    cave[n++]=0x52;                                              /* push edx */
    cave[n++]=0xFF;cave[n++]=0x05; memcpy(cave+n,&totaddr,4); n+=4; /* inc [g_bp_total] */
    /* cmp dword[esp+0xC],162 (ABS_WIDTH after push) */
    cave[n++]=0x81;cave[n++]=0x7C;cave[n++]=0x24;cave[n++]=0x0C;
    imm=BP_GATE_W; memcpy(cave+n,&imm,4); n+=4;
    int jne1=n; cave[n++]=0x0F;cave[n++]=0x85; n+=4;            /* jne restore */
    /* cmp dword[esp+0x10],210 (ABS_HEIGHT) */
    cave[n++]=0x81;cave[n++]=0x7C;cave[n++]=0x24;cave[n++]=0x10;
    imm=BP_GATE_H; memcpy(cave+n,&imm,4); n+=4;
    int jne2=n; cave[n++]=0x0F;cave[n++]=0x85; n+=4;            /* jne restore */
    cave[n++]=0xFF;cave[n++]=0x05; memcpy(cave+n,&mataddr,4); n+=4; /* inc [g_bp_match] */
    { uint32_t nodeaddr=(uint32_t)(uintptr_t)&g_bp_node;
      cave[n++]=0x89;cave[n++]=0x35; memcpy(cave+n,&nodeaddr,4); n+=4; } /* mov [g_bp_node],esi */
    /* CAPTURE ONLY -- do NOT rewrite the resolver args.  ABS_X is the resolver
       INPUT, expressed in the virtual GUI canvas that is CLIPPED to ~1365px
       wide; pushing ABS_X past that makes the resolver cull the rect and the
       panel vanishes (and then there's no 162x210 rect left to fix).  Instead
       we let the resolver run untouched (clean [862,1230,162,210]) and capture
       the node; the worker poll then rewrites the *resolved* node rect, which
       is rendered as-is and bypasses the virtual-canvas clip.  We only record
       ABS_X/ABS_Y here for the log. */
    { uint32_t ax=(uint32_t)(uintptr_t)&g_bp_absx, ay=(uint32_t)(uintptr_t)&g_bp_absy;
      cave[n++]=0x8B;cave[n++]=0x54;cave[n++]=0x24;cave[n++]=0x04;   /* mov edx,[esp+4] ABS_X */
      cave[n++]=0x89;cave[n++]=0x15; memcpy(cave+n,&ax,4); n+=4;     /* mov [g_bp_absx],edx   */
      cave[n++]=0x8B;cave[n++]=0x54;cave[n++]=0x24;cave[n++]=0x08;   /* mov edx,[esp+8] ABS_Y */
      cave[n++]=0x89;cave[n++]=0x15; memcpy(cave+n,&ay,4); n+=4; }   /* mov [g_bp_absy],edx   */
    /* DIAGNOSTIC: single tail, capture only -- run the original call, jmp back.
       No writes, so the panel is untouched. */
    int restore=n;
    cave[n++]=0x5A;                                             /* pop edx */
    cave[n++]=0xFF;cave[n++]=0x90; imm=0xE0; memcpy(cave+n,&imm,4); n+=4; /* call [eax+0xe0] */
    cave[n++]=0xE9; int jback=n; n+=4;                          /* jmp resume */

    int32_t r;
    r=restore-(jne1+6); memcpy(cave+jne1+2,&r,4);
    r=restore-(jne2+6); memcpy(cave+jne2+2,&r,4);
    r=(int32_t)((uintptr_t)resume-((uintptr_t)(cave+jback)+4)); memcpy(cave+jback,&r,4);
    (void)canvasg;

    uint8_t patch[6];
    patch[0]=0xE9;
    r=(int32_t)((uintptr_t)cave-((uintptr_t)site+5)); memcpy(patch+1,&r,4);
    patch[5]=0x90;
    DWORD prot;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &prot)) {
        log_line("[uw] bp: VirtualProtect failed\n"); return 0;
    }
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log_fmt("[uw] bp patch OK: site=%p cave=%p(%dB) gate W=%d,H=%d -> capture+poll-anchor\n",
            (void*)site,(void*)cave,n,BP_GATE_W,BP_GATE_H);
    return 1;
}

/* ================================================================
   Config: uw_fix.ini next to GuildII.exe.  GetPrivateProfileInt needs a
   FULL path (a bare name resolves against the Windows dir), so we build it
   from the exe location.  Missing file/keys -> defaults (everything on).
   ================================================================ */
static void load_config(void) {
    char exe[MAX_PATH] = {0}, ini[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(ini, sizeof ini, "%suw_fix.ini", exe);
    g_do_head = GetPrivateProfileIntA("UltrawideFix", "CharacterPanel",   1, ini);
    g_do_bp   = GetPrivateProfileIntA("UltrawideFix", "ButtonPanelRight", 1, ini);
    g_logging = GetPrivateProfileIntA("UltrawideFix", "Logging",          1, ini);
    g_verbose = GetPrivateProfileIntA("UltrawideFix", "Verbose",          0, ini);
}

/* ================================================================
   Worker thread
   ================================================================ */
static DWORD WINAPI fix_thread(LPVOID unused) {
    (void)unused;
    load_config();
    log_fmt("[uw] " DLL_VERSION " (CharacterPanel=%d ButtonPanelRight=%d "
            "Logging=%d Verbose=%d)\n", g_do_head, g_do_bp, g_logging, g_verbose);

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    g_delta     = (intptr_t)base - (intptr_t)PREFERRED_BASE;

    /* Locate patch sites by signature in the loaded exe (version-independent). */
    find_text_section(base);
    g_canvas_global = find_canvas_global();
    log_fmt("[uw] exe base=%p text=%p/%x canvasGlobal=%08X\n",
            (void*)base, (void*)g_text_base, (unsigned)g_text_size, g_canvas_global);
    if (!g_text_base || g_canvas_global == 0) {
        log_line("[uw] could not locate .text / canvas global; aborting patches\n");
        return 0;
    }

    static const uint8_t HEAD_SIG[9] = { 0x8B,0x50,0x08, 0x89,0x96,0xCC,0x00,0x00,0x00 };
    static const uint8_t BP_SIG[14]  = { 0x8B,0x06, 0x51,0x52,0x53,0x55, 0x8B,0xCE,
                                         0xFF,0x90,0xE0,0x00,0x00,0x00 };

    if (g_do_head) {
        uint8_t *site = scan_text(HEAD_SIG, sizeof HEAD_SIG, 0);
        if (site) { log_fmt("[uw] head site @%p\n",(void*)site); install_head_patch(site, g_canvas_global); }
        else      log_line("[uw] head: signature not found\n");
    } else log_line("[uw] CharacterPanel fix disabled by config\n");

    if (g_do_bp) {
        /* Collect ALL matches FIRST -- patching a site overwrites bytes inside
           its own signature, which would corrupt the nth-match indexing if we
           scanned and patched interleaved. */
        uint8_t *sites[16]; int nb = 0;
        for (int k = 0; k < 16; k++) {
            uint8_t *m = scan_text(BP_SIG, sizeof BP_SIG, k);
            if (!m) break;
            sites[nb++] = m;
        }
        for (int k = 0; k < nb; k++) {
            log_fmt("[uw] bp call @%p\n", (void*)(sites[k] + 8));
            install_bp_patch(sites[k] + 8, g_canvas_global);  /* +8 -> call [eax+0xe0] */
        }
        if (!nb) log_line("[uw] bp: signature not found\n");
        /* Resolve the engine setter (string-anchored, prologue-validated). If it
           can't be validated on this build, the fix is skipped rather than
           risking a call into the wrong code. */
        g_setvalueint = find_setvalueint();
        log_fmt("[uw] SetValueInt resolved @%08X%s\n", g_setvalueint,
                g_setvalueint ? "" : " (FAILED -> bottom-right fix skipped)");
    } else log_line("[uw] ButtonPanelRight fix disabled by config\n");

    /* The bottom-right fix needs a steady poll to re-assert its rect across
       relayouts, so it runs for the session.  Verbose dumps are capped. */
    int pass = 0;
    while (g_do_bp || (g_verbose && pass < 120)) {
        if (g_verbose && pass < 120) { dump_canvas(); scan_pass(); }
        if (g_do_bp) bp_apply();
        pass++;
        Sleep(POLL_MS);
    }
    return 0;
}

/* ================================================================
   DllMain
   ================================================================ */
BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hmod);
        CreateThread(NULL, 0, fix_thread, NULL, 0, NULL);
    }
    return TRUE;
}
