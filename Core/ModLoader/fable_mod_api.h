/*
 * Fable Mod Loader -- the interface between the loader and a mod.
 *
 * Deliberately a plain C ABI. Mods are separate DLLs that may be built with a
 * different compiler to the loader (the multiplayer mod, for instance, is
 * MSVC-only because it links a prebuilt MSVC C++ static library, while the
 * loader itself cross-compiles with mingw). A C ABI is the only thing both can
 * agree on.
 *
 * A mod exports FableModQuery and, usually, FableModShutdown.
 */

#ifndef FABLE_MOD_API_H
#define FABLE_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FABLE_MOD_ABI_VERSION 1

/* Fable.exe has no ASLR and always loads here; a mod should verify this before
 * using any hardcoded address. */
#define FABLE_IMAGE_BASE 0x00400000u

typedef enum {
    FABLE_LOG_INFO  = 0,
    FABLE_LOG_WARN  = 1,
    FABLE_LOG_ERROR = 2
} FableLogLevel;

/* What became of a mod folder the loader found. */
typedef enum {
    FABLE_MOD_LOADED  = 0,      /* queried successfully, not started yet */
    FABLE_MOD_RUNNING = 1,      /* start() returned 0 */
    FABLE_MOD_FAILED  = 2       /* would not load, or would not start */
} FableModStatus;

/* One row of the loader's inventory, as handed to enumerate_mods. Every string
 * points into the loader's own storage and is valid for the life of the
 * process. */
typedef struct FableModEntry {
    uint32_t size;                  /* sizeof(FableModEntry) */
    const char *folder;             /* the mods/<folder> it was found in */
    const char *name;               /* declared identifier, or folder if unknown */
    const char *display_name;       /* may be NULL */
    const char *version;            /* may be NULL */
    int32_t status;                 /* FableModStatus */
    const char *detail;             /* why it failed, else NULL */

    /* One line about what the mod is, for a UI that shows more than a name.
     * NULL if the mod predates this field or did not set one. Appended after
     * the fields above, so guard it with the entry's own `size`. */
    const char *description;
} FableModEntry;

/* Services the loader provides to every mod. Fields are only ever appended,
 * and `size` lets a mod detect what a given loader actually supports. */
typedef struct FableModHost {
    uint32_t size;                  /* sizeof(FableModHost) as the loader built it */
    uint32_t abi_version;           /* FABLE_MOD_ABI_VERSION */

    void (*log)(FableLogLevel level, const char *fmt, ...);

    /* Look up another loaded mod by name, so mods can cooperate -- this is how
     * a UI mod hands its panels to the multiplayer mod without either one
     * linking the other. */
    void *(*get_mod_interface)(const char *mod_name);

    /* Publish this mod's own interface under its name. */
    void (*set_mod_interface)(const char *mod_name, void *iface);

    /* Everything in mods/, including the folders that failed -- a mod list
     * wants to show those most of all. Writes up to `max` entries and returns
     * how many exist, so calling it with max = 0 counts them.
     *
     * Appended after the fields above, so guard it with FABLE_HOST_HAS. */
    int (*enumerate_mods)(FableModEntry *out, int max);
} FableModHost;

/* True if this loader is new enough to have `field`. The struct only ever
 * grows, so its size says what is there. */
#define FABLE_HOST_HAS(host, field) \
    ((host)->size >= offsetof(FableModHost, field) + sizeof((host)->field))

/* What a mod tells the loader about itself. */
typedef struct FableModInfo {
    uint32_t size;                  /* sizeof(FableModInfo) */
    uint32_t abi_version;           /* FABLE_MOD_ABI_VERSION */
    const char *name;               /* stable identifier, e.g. "crossover" */
    const char *display_name;
    const char *version;

    /* Called once after every mod has been loaded, so a mod may depend on
     * another being present without caring about load order. Return 0 on
     * success; a non-zero return disables the mod. */
    int (*start)(const FableModHost *host);

    /* Called before the loader unloads the mod. May be NULL. */
    void (*stop)(void);

    /* One line about what this mod is, shown wherever a mod is described
     * rather than just listed. May be NULL. Appended after the fields above --
     * a mod built against an older header simply reports a smaller `size` and
     * the loader reads no further. */
    const char *description;
} FableModInfo;

/* True if a mod was built against a header new enough to have `field`. Mods are
 * separate DLLs built at different times, so the struct's own size is the only
 * honest test. */
#define FABLE_MOD_INFO_HAS(info, field) \
    ((info)->size >= offsetof(FableModInfo, field) + sizeof((info)->field))

/* The single required export. Return a pointer to a statically allocated
 * FableModInfo, or NULL to decline being loaded. */
typedef const FableModInfo *(*FableModQueryFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* FABLE_MOD_API_H */
