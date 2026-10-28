/*
 * Pivotal Script Extender (PSE) - pse.ui (version 1): drawing over the game, texts, pop-up messages, keyboard focus.
 *
 * Reference: docs/sdk/reference/ui.md. Changes: docs/sdk/API_CHANGELOG.md. API 0 = UNSTABLE (see pse_api.h).
 *
 * Settings need no UI code at all: every setting a plugin declares through pse.settings gets a row on its own page under
 * the game's PSE plugins menu (DS1: OPTIONS > DESERTSTORMFIX > PLUGINS), and PSE_SETTING_LAUNCHER ones also appear in
 * the launcher. pse.ui is for what settings pages don't cover.
 *
 * Served by the game's host (DS1: DesertStormFix, registered while it loads - before any other plugin), so
 *     const PSEUiInterface* ui = pse->Query(pse->plugin, PSE_UI_INTERFACE, 1);
 * may return NULL when the host isn't installed: treat it as optional unless the manifest requires the host.
 *
 * Every function: game thread only (pse.tasks RunOnGameThread from other threads). Strings are UTF-8; characters the
 * game's fonts don't have are replaced (accented letters by their base letter, others by '?').
 */
#ifndef PSE_UI_H
#define PSE_UI_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_UI_INTERFACE "pse.ui"

/* Draw flags (DrawRect / DrawText / DrawImage / TextWidth). */
enum {
    PSE_UI_PIXELS = 0x1,        /* x / y / w / h in back-buffer pixels; default: fractions of the screen (0..1) */
    PSE_UI_FOLLOW_MENU = 0x2,   /* fade with the front-end menus (their transitions / alpha) */
    PSE_UI_CENTER = 0x4,        /* text: x is the centre of the line */
    PSE_UI_RIGHT = 0x8,         /* text: x is the right end of the line */
    PSE_UI_LARGE = 0x10         /* text: the large (menu title) font; default: the small menu font */
};

typedef uint32_t PSEUiImage;    /* 0 = none */

typedef struct PSEUiScreen {
    uint32_t size;              /* sizeof(PSEUiScreen) */
    int32_t width, height;      /* back buffer, pixels */
    float hudScale;             /* the game's 2D scale: 800x600 art drawn at this size (>= 1 above 800x600) */
    float lineHeight;           /* small font line height, pixels (the large font's: about twice) */
    int32_t frontEnd;           /* 1 = the front-end menus are up (no mission running) */
} PSEUiScreen;

/* Keyboard events while this plugin has the keyboard (CaptureKeyboard). */
enum { PSE_UI_KEY_DOWN = 1, PSE_UI_KEY_CHAR = 2 };
typedef struct PSEUiKey {
    uint32_t size;              /* sizeof(PSEUiKey), set by the caller */
    uint32_t type;              /* PSE_UI_KEY_DOWN (code = Windows virtual-key code) / PSE_UI_KEY_CHAR (code = Unicode) */
    uint32_t code;
    uint32_t repeat;            /* key down: 1 = auto-repeat of a held key */
} PSEUiKey;

typedef struct PSEUiInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* ---- drawing ----
     * Call every game frame (PSE_EVENT_FRAME is the place). What a plugin queues during a game frame is drawn over
     * that frame's picture, on top of the game's menus and HUD, in call order, and then forgotten. Colours 0xAARRGGBB.
     * Errors: PSE_E_INVALID_ARG (bad plugin / NULL text / unknown image), PSE_E_NO_MEMORY (more than 4096 items this
     * frame). */
    PSEResult (PSE_CALL* DrawRect)(PSEPluginHandle plugin, float x, float y, float w, float h, uint32_t argb,
                                   uint32_t flags);
    /* Top-left of the line's text box at y (the baseline is below it). One line: '\n' ends the text. */
    PSEResult (PSE_CALL* DrawText)(PSEPluginHandle plugin, float x, float y, const char* text, uint32_t argb,
                                   uint32_t flags);
    /* argb tints the image (0xFFFFFFFF = as it is). */
    PSEResult (PSE_CALL* DrawImage)(PSEPluginHandle plugin, PSEUiImage image, float x, float y, float w, float h,
                                    uint32_t argb, uint32_t flags);
    /* Loads a PNG / DDS / BMP / JPEG file once (path: absolute, or relative to the game folder). Returns 0 when it
     * can't be read. width / height (may be NULL) = the image's size in pixels. Kept until exit; the same path again
     * returns the same image. */
    PSEUiImage (PSE_CALL* LoadImage)(PSEPluginHandle plugin, const char* path, uint32_t* width, uint32_t* height);
    /* Width of one line of text in the units `flags` selects (fraction of the screen width, or pixels). */
    float (PSE_CALL* TextWidth)(PSEPluginHandle plugin, const char* text, uint32_t flags);
    /* PSE_E_NOT_FOUND before the game has a screen (during start-up). */
    PSEResult (PSE_CALL* GetScreen)(PSEUiScreen* out);

    /* ---- texts ----
     * A text the game itself can show: anything that takes a game text hash (DS1: the catalog lookup behind every
     * menu, prompt and message). The host hands out the hashes - never pick your own numbers. Same plugin + key =
     * same hash for the whole run; registering the key again replaces the text. Returns 0 on bad arguments or when
     * the plugin has 4096 keys. */
    uint32_t (PSE_CALL* RegisterText)(PSEPluginHandle plugin, const char* key, const char* text);
    uint32_t (PSE_CALL* TextHash)(PSEPluginHandle plugin, const char* key);   /* 0 = not registered */

    /* ---- messages ----
     * The game's pop-up bar (DS1: the message bar across the top of the screen in a mission, as for picked-up items
     * and warnings) for `ms` milliseconds (0 = 4000); a newer message replaces it. In the front end, where the game
     * has none, the host draws a bar like it itself. */
    PSEResult (PSE_CALL* ShowPopup)(PSEPluginHandle plugin, const char* text, uint32_t ms);

    /* ---- keyboard focus ----
     * on != 0: key presses and typed characters go to this plugin (ReadKey) instead of the game, until it calls
     * CaptureKeyboard(plugin, 0) (or fails / unloads). Key releases still reach the game, so nothing stays held.
     * The mouse and pads are not captured. One plugin at a time: PSE_E_CONFLICT while another has it. */
    PSEResult (PSE_CALL* CaptureKeyboard)(PSEPluginHandle plugin, int on);
    /* Takes the oldest queued event: 1 = *out filled, 0 = none (or not capturing). Up to 64 are queued. */
    int (PSE_CALL* ReadKey)(PSEPluginHandle plugin, PSEUiKey* out);
} PSEUiInterface;

#ifdef __cplusplus
}
#endif

#endif /* PSE_UI_H */
