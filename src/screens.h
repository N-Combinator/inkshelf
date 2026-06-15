/*
 * screens.h — concrete screen constructors.
 *
 * Each returns a pointer to a (statically allocated) screen_t ready to hand
 * to nav_push(). The OPDS browser and WiFi-drop screens are stubs for now
 * and get fleshed out in their own milestones.
 */

#ifndef INKSHELF_SCREENS_H
#define INKSHELF_SCREENS_H

#include "app.h"

/* Root screen: the main menu (OPDS Catalog / WiFi Book Drop). */
screen_t *screen_main_menu(void);

/* OPDS catalog entry point: pick a preset catalog or type a custom URL,
 * then browse/search/download. Implemented in opds_ui.c. */
screen_t *screen_opds_catalog(void);

#endif /* INKSHELF_SCREENS_H */
