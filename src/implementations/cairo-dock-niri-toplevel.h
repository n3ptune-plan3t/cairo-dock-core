/*
 * cairo-dock-niri-toplevel.h
 *
 * Copyright 2024-2025
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef __CAIRO_DOCK_NIRI_TOPLEVEL__
#define __CAIRO_DOCK_NIRI_TOPLEVEL__

#include <wayland-client.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * Checks if the advertised Wayland interface matches the protocols
 * required by the Niri backend (wlr-foreign-toplevel or ext-workspace).
 * * @param id The registry ID of the interface.
 * @param interface The name of the interface (e.g., "zwlr_foreign_toplevel_manager_v1").
 * @param version The version of the interface.
 * @return TRUE if the interface is relevant to this module, FALSE otherwise.
 */
gboolean gldi_niri_toplevel_match_protocol (uint32_t id, const char *interface, uint32_t version);

/**
 * Attempts to initialize the Niri window manager backend.
 * This should be called after the registry has been fully scanned.
 * * @param registry The global Wayland registry.
 * @return TRUE if initialization was successful (protocols bound), FALSE otherwise.
 */
gboolean gldi_niri_toplevel_try_init (struct wl_registry *registry);

G_END_DECLS

#endif
