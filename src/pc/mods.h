/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pop-in characters (MeleeForge): custom characters dropped into mods/ as .ssb packages are found at boot, listed
 * under a CHARACTERS icon on character select, and assigned to players. A custom character plays with a base
 * fighter's moves; the player picks that fighter on the grid and gets the custom model, per player. */
#ifndef PC_MODS_H
#define PC_MODS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void pc_mods_init(void);      /* after pc_menu_init (RmlUi + fonts ready) */
void pc_mods_update(void);    /* once per frame, after pc_menu_update */
void pc_mods_css_enter(void); /* character select opened */
void pc_mods_css_exit(void);  /* character select closed */
bool pc_mods_menu_open(void); /* the dropdown is open */

/* Fighter creation (ft/fighter.c): the model for this player, or NULL to keep the base fighter's own.
 * base_joint is the costume joint the game loaded for (kind, costume); it changes when the game reloads its
 * costumes, which tells the loader its own cached model is stale too. */
void* pc_mods_fighter_joint(int player, int kind, void* base_joint);
/* The material-animation joint that goes with that model; false = keep the game's. */
bool pc_mods_fighter_matanim(int player, int kind, void** matanim);
/* The fighter system was reset for a new scene (its archives are gone). */
void pc_mods_scene_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MODS_H */
