/*
 * Fable Mod Loader -- the native frontend SDK.
 *
 * This is not the overlay. `fable_ui.h` draws over the finished back buffer;
 * this edits the game's own frontend so a mod's menu row and page are built by
 * Fable, out of Fable's own widgets, in Fable's own style. There is nothing to
 * draw and nothing to hit-test: the game does it.
 *
 * How it works, in one paragraph. A frontend screen builds its children at
 * construction from a list of definition ids in its own definition, and every
 * definition passes through the component factory one instruction before its
 * component is built. The `frontend` mod sits in that window and edits
 * definitions: appending an id to a list adds a row, and overwriting a CText's
 * text key changes what a row says. All of it is established in the crossover
 * repo's docs/ui-system.md, sections 12-16.
 *
 * The consequences of doing it that way are the awkward parts of this API, and
 * they are real limits rather than policy:
 *
 *   - **Everything is decided before the screen is built.** There is no hot
 *     reload: a label is baked into the component at construction. Register
 *     during your mod's start(); changes made later apply the next time the
 *     player opens the screen.
 *
 *   - **Labels are written over a stock definition's text buffer**, so a label
 *     cannot be longer than the one it replaces. `max_label` reports the real
 *     limit for a row, and anything longer is truncated rather than refused.
 *     (Any text works, though -- the game draws a text key it cannot resolve,
 *     so a label needs no entry in the language bank.)
 *
 *   - **The page has a small, fixed capacity.** Each row needs a distinct
 *     button definition to appropriate, and only so many exist. `page_capacity`
 *     is the number, not a guess.
 *
 *   - **A row cannot run your code when clicked.** A button's action is a stock
 *     action id and nothing has been found that dispatches to a mod. The API
 *     has no on_click for that reason rather than because it was forgotten.
 *     What a row CAN do is open a screen, and screens are editable too -- so
 *     `set_row_detail` gives each row a page of its own, filled in ahead of
 *     time. That covers showing something; it does not cover running something.
 *
 * Resolve it by name, exactly like any other mod interface:
 *
 *     FableFrontend *fe = host->get_mod_interface(FABLE_FRONTEND_MOD_NAME);
 *     if (fe && FABLE_FRONTEND_HAS(fe, add_page_row))
 *         fe->add_page_row("My Mod  1.0");
 */

#ifndef FABLE_FRONTEND_H
#define FABLE_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FABLE_FRONTEND_MOD_NAME    "frontend"
#define FABLE_FRONTEND_ABI_VERSION 1

/* Why the frontend is not available, if it is not. */
typedef enum {
    FABLE_FRONTEND_OK            = 0,
    FABLE_FRONTEND_NO_HOOK       = 1,  /* the factory window could not be taken */
    FABLE_FRONTEND_WRONG_BUILD   = 2,  /* the definitions are not what we expect */
    FABLE_FRONTEND_NOT_READY     = 3   /* asked before the mod started */
} FableFrontendStatus;

typedef struct FableFrontend {
    uint32_t size;                  /* sizeof(FableFrontend) as built */
    uint32_t abi_version;           /* FABLE_FRONTEND_ABI_VERSION */

    /* FABLE_FRONTEND_OK once the hook is installed and the id table has been
     * checked against the running game. Everything below is a no-op until it
     * is, so a mod may register unconditionally and simply get nothing. */
    int (*status)(void);

    /* --- the mod page --- */

    /* Add a row. Returns the row's index, or -1 if the page is full.
     * `label` is copied immediately; it may be freed on return. */
    int (*add_page_row)(const char *label);

    /* Relabel a row already added. Takes effect the next time the page is
     * built -- leave the page and come back. */
    int (*set_page_row)(int row, const char *label);

    /* The page's heading. */
    int (*set_page_title)(const char *title);

    /* How many rows exist to be appropriated. Not policy: one stock button
     * definition is spent per row. */
    int (*page_capacity)(void);

    /* The longest label this row can show, in characters, because the text is
     * written over the buffer of the key it replaces. Pass -1 for the title. */
    int (*max_label)(int row);

    /* --- the main menu --- */

    /* What the mod loader's own row on the main menu says. Defaults to
     * "Mods". */
    int (*set_menu_label)(const char *label);

    /*
     * What the player sees after pressing a row.
     *
     * A row cannot call your code -- a button's action is a stock action id and
     * nothing dispatches to a mod. But an action opens a *screen*, and screens
     * are as editable as rows: each row carries a different stock action, so
     * the screen the game builds identifies the row that was pressed. That is
     * what this fills in.
     *
     * `heading` is the page's title; the three lines are its body. Each is
     * written over a different stock text buffer, so they truncate
     * independently -- roughly 24 characters a line. Pass NULL to leave one
     * unchanged.
     */
    int (*set_row_detail)(int row, const char *heading,
                          const char *line0, const char *line1,
                          const char *line2);
} FableFrontend;

/* True if this frontend build is new enough to have `field`. The struct only
 * ever grows. */
#define FABLE_FRONTEND_HAS(fe, field) \
    ((fe)->size >= offsetof(FableFrontend, field) + sizeof((fe)->field))

#ifdef __cplusplus
}
#endif

#endif /* FABLE_FRONTEND_H */
