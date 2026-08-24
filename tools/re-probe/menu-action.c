/*
 * menu-action: find what decides a menu row's action.
 *
 * Three of the four pieces of an entry of our own are proved: a row can be
 * added (section 13), pointed at a label (15), and given arbitrary text (16).
 * What a row *does* is the last one, and it is the only one that cannot be read
 * off the screen -- it needs a click.
 *
 * The candidate is the only mirrored per-button pair left in a
 * CFrontEndButton's definition:
 *
 *     +0xC4 / +0xE4    Continue 66   Change 16   Options 297
 *                      Credits  67   About 321   Quit    314
 *
 * It is already eliminated as the text: writing Options' 297 over Quit's 314
 * left the row reading "Quit". So if it is anything, it is the action.
 *
 * This run does two independent edits to the last row, both before construction:
 *
 *   the CText key      -> "Mods"     so the row is unmistakably the edited one
 *   the button +0xC4/E4 -> 297       Options' value
 *
 * Then one click on "Mods" separates the cases:
 *
 *   opens Options            -> +0xC4 is the action, and a row's behaviour is
 *                               selectable by data alone
 *   asks "are you sure you
 *     want to quit?"         -> +0xC4 is inert; the action comes from somewhere
 *                               else, most likely the button's name, and will
 *                               need a hook rather than a field
 *
 * Handler rule, learned the hard way in section 16: copy bytes, call nothing.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-action.log"

#define DEF_ID    0x20
#define DEF_TYPE  0x3C
#define DEF_KEY   0x54
#define DEF_ACT   0xC4
#define DEF_ACT2  0xE4

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

#define QUIT_BUTTON 251
#define QUIT_CTEXT  254
#define OPTIONS_ACT 297          /* what Options' button carries at +0xC4 */

static const WCHAR OURS[] = L"Mods";

static FILE *g_log;

static void plog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)

static unsigned char g_orig;
static int g_saved;

static volatile LONG g_text_done, g_act_done, g_reported;
static volatile DWORD g_text_def, g_act_def, g_act_was, g_act_was2;
static char g_key_was[128];

static int arm_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;
    if (cur == 0xCC) return 1;
    if (!g_saved) { g_orig = cur; g_saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                        PAGE_EXECUTE_READWRITE, &prot)) return 0;
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
    VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    return 1;
}

static void disarm_bp(void)
{
    DWORD prot;
    if (!g_saved) return;
    if (VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = g_orig;
        VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    }
}

static void edit(DWORD def)
{
    DWORD id;

    if (!PLAUSIBLE(def)) return;
    id = *(DWORD *)(uintptr_t)(def + DEF_ID);

    if (id == QUIT_CTEXT && !g_text_done) {
        DWORD strobj = *(DWORD *)(uintptr_t)(def + DEF_KEY);
        DWORD chars;
        int i;

        if (!PLAUSIBLE(strobj)) return;
        chars = *(DWORD *)(uintptr_t)strobj;
        if (!PLAUSIBLE(chars)) return;

        for (i = 0; i < 63; i++) {
            unsigned short w = *(unsigned short *)(uintptr_t)(chars + i * 2);
            if (!w || w < 0x20 || w > 0x7E) break;
            g_key_was[i] = (char)w;
            g_key_was[i + 1] = 0;
        }
        memcpy((void *)(uintptr_t)chars, OURS, sizeof OURS);
        g_text_def = def;
        g_text_done = 1;
        return;
    }

    if (id == QUIT_BUTTON && !g_act_done) {
        g_act_was  = *(DWORD *)(uintptr_t)(def + DEF_ACT);
        g_act_was2 = *(DWORD *)(uintptr_t)(def + DEF_ACT2);
        *(DWORD *)(uintptr_t)(def + DEF_ACT)  = OPTIONS_ACT;
        *(DWORD *)(uintptr_t)(def + DEF_ACT2) = OPTIONS_ACT;
        g_act_def = def;
        g_act_done = 1;
    }
}

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    DWORD ebx;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress != FACTORY_TYPE_READ)
        return EXCEPTION_CONTINUE_SEARCH;

    ebx = ep->ContextRecord->Ebx;
    if (!PLAUSIBLE(ebx)) {
        disarm_bp();
        ep->ContextRecord->Eip = FACTORY_TYPE_READ;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    edit(ebx);

    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void dismiss_dialogs(void)
{
    HWND hw = FindWindowA(NULL, "Fable - The Lost Chapters ");
    if (!hw) hw = FindWindowA(NULL, "Fable - The Lost Chapters");
    if (!hw) return;
    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0);
}

static DWORD WINAPI worker(LPVOID unused)
{
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-action: is CFrontEndButton +0xC4 the row's action? ===");
    plog("last row labelled \"Mods\"; its button +0xC4/+0xE4 set to Options' %d",
         OPTIONS_ACT);

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");

    /* Reported one at a time. The previous run died before it had both and
     * the single combined message meant the log said nothing at all about
     * which edit had landed -- a partial result is still evidence. */
    {
        LONG said_text = 0, said_act = 0;

        for (t = 0; t < 60000; t++) {
            if (g_text_done && !said_text) {
                said_text = 1;
                plog("  [text]   CText %d at 0x%08lX: key \"%s\" -> \"Mods\"",
                     QUIT_CTEXT, g_text_def, g_key_was);
            }
            if (g_act_done && !said_act) {
                said_act = 1;
                plog("  [action] button %d at 0x%08lX: +0xC4 %lu -> %d, "
                     "+0xE4 %lu -> %d", QUIT_BUTTON, g_act_def,
                     g_act_was, OPTIONS_ACT, g_act_was2, OPTIONS_ACT);
            }
            if (g_text_done && g_act_done && !g_reported) {
                g_reported = 1;
                disarm_bp();
                plog("");
                plog("  CLICK \"Mods\":");
                plog("    Options screen -> +0xC4 is the action");
                plog("    quit prompt    -> +0xC4 is inert, action is elsewhere");
            }
            if (!(g_text_done && g_act_done) && (t & 0x1F) == 0)
                dismiss_dialogs();
            Sleep(20);
        }
    }
    disarm_bp();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
