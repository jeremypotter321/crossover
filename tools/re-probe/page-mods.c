/*
 * page-mods: a Mods entry on the main menu that opens a page of our own.
 *
 * Everything here uses only mechanisms already proved:
 *
 *   section 13   a row exists          - child vector of the list definition
 *   section 15   which label it uses   - child id swap
 *   section 16   what the label says   - overwrite the CText key in place;
 *                                        an unresolved key is drawn as-is
 *   confirmed    what it does          - CFrontEndButton CUIDef +0xC4/+0xE4
 *
 * The page is the Options screen (211), rebuilt into ours. page-survey showed
 * what it is made of:
 *
 *     SCREEN 211
 *       219  LIST    -> [347, 350, 273, 344]   the four option rows
 *       338  TEXT    "TEXT_GUI_MENU_OPTIONS"   the page title
 *       577  -> 563  -> 569 TEXT "TEXT_GUI_BACK"   the Back bar
 *
 * The Back bar is part of the screen already, so a page with a working Back
 * button costs nothing: leave 577 alone. What changes is the title, and the
 * list of rows.
 *
 * Row action: the single row is given 297, which re-opens this same page. That
 * is deliberate -- it is a proved, harmless action, so the row can be clicked
 * without leaving the page or reaching game code we have not tested. A real mod
 * would put its own action here once one exists.
 *
 * Every edit lands in the factory window at 0x0041D249, before the component is
 * built, because the values are baked in at construction and there is no hot
 * reload. The handler copies bytes and calls nothing: a Win32 call from inside
 * the game's own dispatch deadlocks it.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "page-mods.log"

#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_KEY         0x54
#define DEF_ACTION      0xC4
#define DEF_ACTION2     0xE4
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74
#define DEF_CHILD_CAP   0x78

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

/* --- the main-menu row --- */
#define QUIT_BUTTON   251        /* becomes our Mods row      */
#define QUIT_CTEXT    254        /* its label                 */
#define ACTION_OPEN_PAGE 297     /* opens screen 211          */

/* --- the page --- */
#define PAGE_TITLE    338        /* CText, the screen's title */
#define PAGE_LIST     219        /* LIST of rows              */
#define PAGE_ROW      347        /* the row we keep           */
#define PAGE_ROW_MID  348        /* its CChangingStateComponent */

static const WCHAR MENU_LABEL[] = L"Mods";
static const WCHAR PAGE_TITLE_TEXT[] = L"Mods";
static const WCHAR ROW_LABEL[] = L"No mods loaded";

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

/* Reserved up front -- the handler allocates nothing. */
static DWORD g_rows[4];

static volatile DWORD g_row_text_id;        /* learned from PAGE_ROW_MID */
static volatile LONG  g_did_menu_text, g_did_menu_action;
static volatile LONG  g_did_title, g_did_list, g_did_row_text, g_did_row_action;
static volatile DWORD g_old_action, g_list_n;

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

/* Overwrite a CText definition's key in place. Wide, and never longer than
 * what is already there, so nothing grows and nothing is allocated. */
static int set_key(DWORD def, const WCHAR *text, SIZE_T bytes)
{
    DWORD strobj = *(DWORD *)(uintptr_t)(def + DEF_KEY);
    DWORD chars;

    if (!PLAUSIBLE(strobj)) return 0;
    chars = *(DWORD *)(uintptr_t)strobj;
    if (!PLAUSIBLE(chars)) return 0;
    memcpy((void *)(uintptr_t)chars, text, bytes);
    return 1;
}

static void edit(DWORD def)
{
    DWORD id = *(DWORD *)(uintptr_t)(def + DEF_ID);

    /* --- the main menu row --- */
    if (id == QUIT_CTEXT && !g_did_menu_text)
        g_did_menu_text = set_key(def, MENU_LABEL, sizeof MENU_LABEL);

    if (id == QUIT_BUTTON && !g_did_menu_action) {
        g_old_action = *(DWORD *)(uintptr_t)(def + DEF_ACTION);
        *(DWORD *)(uintptr_t)(def + DEF_ACTION)  = ACTION_OPEN_PAGE;
        *(DWORD *)(uintptr_t)(def + DEF_ACTION2) = ACTION_OPEN_PAGE;
        g_did_menu_action = 1;
    }

    /* --- the page --- */
    if (id == PAGE_TITLE && !g_did_title)
        g_did_title = set_key(def, PAGE_TITLE_TEXT, sizeof PAGE_TITLE_TEXT);

    if (id == PAGE_LIST && !g_did_list) {
        DWORD b = *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN);
        DWORD e = *(DWORD *)(uintptr_t)(def + DEF_CHILD_END);
        if (PLAUSIBLE(b) && e > b) {
            g_list_n = (e - b) / 4;
            g_rows[0] = PAGE_ROW;                     /* one row, ours */
            *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN) = (DWORD)(uintptr_t)g_rows;
            *(DWORD *)(uintptr_t)(def + DEF_CHILD_END)   = (DWORD)(uintptr_t)(g_rows + 1);
            *(DWORD *)(uintptr_t)(def + DEF_CHILD_CAP)   = (DWORD)(uintptr_t)(g_rows + 1);
            g_did_list = 1;
        }
    }

    /* The row's label id is not known up front, so it is learned: a parent is
     * dispatched before its children, so seeing PAGE_ROW_MID gives us the id
     * of its CText in time to catch it. */
    if (id == PAGE_ROW_MID && !g_row_text_id) {
        DWORD b = *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN);
        DWORD e = *(DWORD *)(uintptr_t)(def + DEF_CHILD_END);
        if (PLAUSIBLE(b) && e > b) g_row_text_id = *(DWORD *)(uintptr_t)b;
    }

    if (g_row_text_id && id == g_row_text_id && !g_did_row_text &&
        *(DWORD *)(uintptr_t)(def + DEF_TYPE) == 6)
        g_did_row_text = set_key(def, ROW_LABEL, sizeof ROW_LABEL);

    if (id == PAGE_ROW && !g_did_row_action) {
        *(DWORD *)(uintptr_t)(def + DEF_ACTION)  = ACTION_OPEN_PAGE;
        *(DWORD *)(uintptr_t)(def + DEF_ACTION2) = ACTION_OPEN_PAGE;
        g_did_row_action = 1;
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
    LONG s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== page-mods: a Mods row, and a page of our own behind it ===");
    plog("menu row: button %d -> action %d, label \"Mods\"",
         QUIT_BUTTON, ACTION_OPEN_PAGE);
    plog("page: screen 211 retitled, list %d cut to one row, Back left alone",
         PAGE_LIST);

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");
    plog("");

    for (t = 0; t < 200000; t++) {
        if (g_did_menu_text   && !s1) { s1 = 1; plog("  [menu]  label -> \"Mods\""); }
        if (g_did_menu_action && !s2) { s2 = 1; plog("  [menu]  action %lu -> %d",
                                                    g_old_action, ACTION_OPEN_PAGE); }
        if (g_did_title       && !s3) { s3 = 1; plog("  [page]  title -> \"Mods\""); }
        if (g_did_list        && !s4) { s4 = 1; plog("  [page]  list %d: %lu rows -> 1",
                                                    PAGE_LIST, g_list_n); }
        if (g_row_text_id     && !s5) { s5 = 1; plog("  [page]  row label is CText %lu",
                                                    g_row_text_id); }
        if (g_did_row_text    && !s6) { s6 = 1; plog("  [page]  row label -> "
                                                    "\"No mods loaded\""); }
        if ((t & 0x1F) == 0) dismiss_dialogs();
        Sleep(20);
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
