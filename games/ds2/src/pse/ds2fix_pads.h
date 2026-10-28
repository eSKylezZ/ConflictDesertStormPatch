/* ds2fix.pads (version 1): the game's own DirectInput joysticks read for menus, for DS2 plugins.
 *
 * Published by the host plugin ds2fix (src/pse/ds2fix.cpp, owner ds2fix.coop) through pse.messaging RegisterInterface
 * while it loads; a plugin loaded after it finds it in its own PSEPlugin_Load (manifest `requires = ds2fix >= 0.1`):
 *
 *   const Ds2FixPadsV1* pads = (const Ds2FixPadsV1*)m->QueryInterface(pse->plugin, DS2FIX_PADS_INTERFACE, 1);
 *
 * Joystick j = the game's j-th DirectInput game controller ([0x67E7DC + j*4], count [0x67F6C0]) - the number the game's
 * input blocks use (block +0x3A4), so a co-op player's pad is his block's joystick. Read raw (before the game's own
 * remapping) with the per-vendor button map ds2fix's co-op join screen uses (src/features/coop.cpp): PlayStation
 * (vendor 0x054C: DualSense, DualShock 4) Cross 1 / Circle 2 / Triangle 3 / L1 4 / R1 5 / Options 9; Xbox and others
 * A 0 / B 1 / Y 3 / LB 4 / RB 5 / Start 7; directions from the POV hat or the left stick past half. Game thread only. */
#pragma once
#include <stdint.h>

#include "pse/pse_api.h"

#define DS2FIX_PADS_INTERFACE "ds2fix.pads"

enum Ds2FixPadButton {
    DS2FIX_PAD_UP = 1u << 0,
    DS2FIX_PAD_DOWN = 1u << 1,
    DS2FIX_PAD_LEFT = 1u << 2,
    DS2FIX_PAD_RIGHT = 1u << 3,
    DS2FIX_PAD_ACCEPT = 1u << 4, /* Cross / A */
    DS2FIX_PAD_BACK = 1u << 5,   /* Circle / B */
    DS2FIX_PAD_START = 1u << 6,  /* Options / Start */
    DS2FIX_PAD_ALT = 1u << 7,    /* Triangle / Y */
    DS2FIX_PAD_L1 = 1u << 8,     /* L1 / LB */
    DS2FIX_PAD_R1 = 1u << 9      /* R1 / RB */
};
enum Ds2FixPadKind { DS2FIX_PAD_NONE = 0, DS2FIX_PAD_PLAYSTATION = 1, DS2FIX_PAD_XBOX = 2, DS2FIX_PAD_OTHER = 3 };

typedef struct Ds2FixPadsV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    int (PSE_CALL* Count)(void);                 /* the game's joysticks (0..8) */
    uint32_t (PSE_CALL* Buttons)(int joystick);  /* Ds2FixPadButton bits held now; 0 = none / no such pad */
    int (PSE_CALL* Kind)(int joystick);          /* Ds2FixPadKind */
} Ds2FixPadsV1;

/* ds2fix.coop (version 1): the co-op session ds2fix's join screen started (src/features/coop.cpp), published next to
 * ds2fix.pads: Players() = 2..4 while a session runs (0 = none), Joystick(p) = player p's joystick (-1 = keyboard and
 * mouse). Player p = input block p = split-screen view p; a soldier's owner is unit +0x1CF squad role mapped by
 * 0x475610 (owner getter 0x475970). */
/* ds2fix.skins (version 1): another look for one soldier in play - Dress(soldier, body model name, texture name):
 * the body (a CAnimInstance::SetModel switch, so only a model with his skeleton's key count) and / or one texture on
 * every skin part (ds2fix.skins' draw hook, src/features/skins.cpp); NULL / "" keeps that part. Returns 0 when not done.
 * Kept until the next level. Used by ds2fix.loadout's UNIFORM / FACE rows. */
#define DS2FIX_SKINS_INTERFACE "ds2fix.skins"
typedef struct Ds2FixSkinsV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    int (PSE_CALL* Dress)(void* soldier, const char* body, const char* texture);
} Ds2FixSkinsV1;

#define DS2FIX_COOP_INTERFACE "ds2fix.coop"
typedef struct Ds2FixCoopV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    int (PSE_CALL* Players)(void);
    int (PSE_CALL* Joystick)(int player);
} Ds2FixCoopV1;
