/*
 * cairo-dock-niri-toplevel.c
 * * Integration for Niri (v25.11+) Window Manager.
 * * Protocols used:
 * 1. wlr_foreign_toplevel_management_unstable_v1 (Window Management)
 * 2. ext_workspace_v1 (Workspace Listing & State)
 * * Actions (Move to Workspace) are handled via Niri IPC (niri msg)
 * because standard protocols do not yet support moving windows to specific
 * dynamic workspace indices reliably.
 * * Copyright 2024-2025
 * * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 */

#include "gldi-config.h"
#ifdef HAVE_WAYLAND

#include <gdk/gdkwayland.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Protocol Client Headers
// Ensure these are generated during build
#include "wayland-wlr-foreign-toplevel-management-client-protocol.h"
#include "wayland-ext-workspace-v1-client-protocol.h"

// Cairo-Dock Core Headers
#include "cairo-dock-desktop-manager.h"
#include "cairo-dock-windows-manager-priv.h"
#include "cairo-dock-container-priv.h"
#include "cairo-dock-dock-factory.h"
#include "cairo-dock-dock-manager.h"
#include "cairo-dock-log.h"
#include "cairo-dock-wayland-wm.h"
#include "cairo-dock-ext-workspaces.h" // Helper for ext-workspace-v1

#define NIRI_IPC_CMD "niri"

typedef struct zwlr_foreign_toplevel_handle_v1 wfthandle;

// Global Protocol Managers
static struct zwlr_foreign_toplevel_manager_v1* s_ptoplevel_manager = NULL;
static struct ext_workspace_manager_v1* s_pworkspace_manager = NULL;

static uint32_t toplevel_id = 0, toplevel_ver = 0;
static uint32_t workspace_id = 0, workspace_ver = 0;
static gboolean toplevel_found = FALSE;
static gboolean workspace_found = FALSE;

/**********************************************************************
 * Niri IPC Helper Functions                                          */

/**
 * Execute a Niri IPC action via the 'niri msg' command line tool.
 * Since we are running inside the session, 'niri' should automatically 
 * find the socket via NIRI_SOCKET env var.
 */
static void _niri_ipc_action(const char *action, const char *arg)
{
	gchar *command;
	if (arg)
		command = g_strdup_printf("%s msg action %s %s", NIRI_IPC_CMD, action, arg);
	else
		command = g_strdup_printf("%s msg action %s", NIRI_IPC_CMD, action);

	GError *error = NULL;
	// Fire and forget asynchronous command
	if (!g_spawn_command_line_async(command, &error))
	{
		cd_warning("Niri Integration: Failed to execute IPC command '%s': %s", 
			command, error ? error->message : "Unknown error");
		if (error) g_error_free(error);
	}
	g_free(command);
}

/**
 * Moves a window to a specific workspace index.
 * * Strategy:
 * 1. Activate the window via Wayland protocol (wlr-foreign-toplevel).
 * This ensures Niri focuses the correct window.
 * 2. Send 'move-window-to-workspace' IPC command which acts on the focused window.
 */
static void _niri_move_window_to_workspace(GldiWaylandWindowActor *wactor, int workspace_idx)
{
	if (!wactor || !wactor->handle) return;

	// 1. Activate Window
	GdkDisplay *dsp = gdk_display_get_default();
	GdkSeat *seat = gdk_display_get_default_seat(dsp);
	struct wl_seat* wl_seat = gdk_wayland_seat_get_wl_seat(seat);
	
	zwlr_foreign_toplevel_handle_v1_activate(wactor->handle, wl_seat);

	// 2. Move via IPC
	// Niri workspaces are dynamically indexed. The IPC usually expects 1-based index
	// or a name. Cairo-Dock uses 0-based index.
	// Valid Niri 25.11 command: niri msg action move-window-to-workspace <index>
	gchar *idx_str = g_strdup_printf("%d", workspace_idx + 1);
	_niri_ipc_action("move-window-to-workspace", idx_str);
	g_free(idx_str);
}

/**********************************************************************
 * Window Manager Backend Interface                                   */

static void _show(GldiWindowActor *actor)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	GdkDisplay *dsp = gdk_display_get_default();
	GdkSeat *seat = gdk_display_get_default_seat(dsp);
	struct wl_seat* wl_seat = gdk_wayland_seat_get_wl_seat(seat);
	
	zwlr_foreign_toplevel_handle_v1_activate(wactor->handle, wl_seat);
}

static void _close(GldiWindowActor *actor)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	zwlr_foreign_toplevel_handle_v1_close(wactor->handle);
}

static void _minimize(GldiWindowActor *actor)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	zwlr_foreign_toplevel_handle_v1_set_minimized(wactor->handle);
}

static void _maximize(GldiWindowActor *actor, gboolean bMaximize)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	if (bMaximize) 
		zwlr_foreign_toplevel_handle_v1_set_maximized(wactor->handle);
	else 
		zwlr_foreign_toplevel_handle_v1_unset_maximized(wactor->handle);
}

static void _set_fullscreen(GldiWindowActor *actor, gboolean bFullScreen)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	// NULL output means "current output"
	if (bFullScreen) 
		zwlr_foreign_toplevel_handle_v1_set_fullscreen(wactor->handle, NULL);
	else 
		zwlr_foreign_toplevel_handle_v1_unset_fullscreen(wactor->handle);
}

static void _move_to_nth_desktop(GldiWindowActor *actor, int iNumDesktop, int x, int y)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	// Use our hybrid IPC approach
	_niri_move_window_to_workspace(wactor, iNumDesktop);
}

static GldiWindowActor* _get_transient_for(GldiWindowActor* actor)
{
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	wfthandle* parent = wactor->parent;
	if (!parent) return NULL;
	GldiWaylandWindowActor *pactor = zwlr_foreign_toplevel_handle_v1_get_user_data(parent);
	return (GldiWindowActor*)pactor;
}

static void _set_thumbnail_area(GldiWindowActor *actor, GldiContainer* pContainer, int x, int y, int w, int h)
{
	if (!(actor && pContainer)) return;
	
	GldiWaylandWindowActor *wactor = (GldiWaylandWindowActor *)actor;
	GdkWindow* window = gldi_container_get_gdk_window(pContainer);
	if (!window) return;
	
	struct wl_surface* surface = gdk_wayland_window_get_wl_surface(window);
	if (!surface) return;
	
	zwlr_foreign_toplevel_handle_v1_set_rectangle(wactor->handle, surface, x, y, w, h);
}

static void _can_minimize_maximize_close(G_GNUC_UNUSED GldiWindowActor *actor, gboolean *bCanMinimize, gboolean *bCanMaximize, gboolean *bCanClose)
{
	*bCanMinimize = TRUE;
	*bCanMaximize = TRUE;
	*bCanClose = TRUE;
}

static void _get_supported_actions(gboolean *bCanFullscreen, gboolean *bCanSticky, gboolean *bCanBelow, gboolean *bCanAbove, gboolean *bCanKill)
{
	if (bCanFullscreen) *bCanFullscreen = TRUE;
	if (bCanSticky) *bCanSticky = FALSE; // Niri doesn't strictly support "sticky" via this protocol
	if (bCanBelow) *bCanBelow = FALSE;
	if (bCanAbove) *bCanAbove = FALSE;
	if (bCanKill) *bCanKill = FALSE;
}

/**********************************************************************
 * wlr_foreign_toplevel Callbacks                                     */

static void _toplevel_state_cb(void *data, G_GNUC_UNUSED wfthandle *handle, struct wl_array *state)
{
	if (!data) return;
	GldiWaylandWindowActor* wactor = (GldiWaylandWindowActor*)data;
	
	gboolean activated = FALSE;
	gboolean maximized = FALSE;
	gboolean minimized = FALSE;
	gboolean fullscreen = FALSE;
	
	uint32_t* stdata = (uint32_t*)state->data;
	size_t i;
	for (i = 0; i * sizeof(uint32_t) < state->size; i++) {
		switch(stdata[i]) {
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: activated = TRUE; break;
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: maximized = TRUE; break;
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: minimized = TRUE; break;
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: fullscreen = TRUE; break;
		}
	}
	
	gldi_wayland_wm_activated(wactor, activated, FALSE);
	gldi_wayland_wm_maximized_changed(wactor, maximized, FALSE);
	gldi_wayland_wm_minimized_changed(wactor, minimized, FALSE);
	gldi_wayland_wm_fullscreen_changed(wactor, fullscreen, FALSE);
}

static void _toplevel_title_cb(void *data, G_GNUC_UNUSED wfthandle *handle, const char *title)
{
	gldi_wayland_wm_title_changed(data, title, FALSE);
}

static void _toplevel_app_id_cb(void *data, G_GNUC_UNUSED wfthandle *handle, const char *app_id)
{
	gldi_wayland_wm_appid_changed(data, app_id, FALSE);
}

static void _toplevel_parent_cb(void* data, G_GNUC_UNUSED wfthandle *handle, wfthandle *parent)
{
	GldiWaylandWindowActor* wactor = (GldiWaylandWindowActor*)data;
	wactor->parent = parent;
}

static void _toplevel_output_enter_cb(G_GNUC_UNUSED void *data, G_GNUC_UNUSED wfthandle *handle, G_GNUC_UNUSED struct wl_output *output)
{
	// Niri uses infinite scrolling; strict output association is useful but 
	// specific workspace association via standard wlr-protocols is limited.
}

static void _toplevel_output_leave_cb(G_GNUC_UNUSED void *data, G_GNUC_UNUSED wfthandle *handle, G_GNUC_UNUSED struct wl_output *output)
{
}

static struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
	.title = _toplevel_title_cb,
	.app_id = _toplevel_app_id_cb,
	.output_enter = _toplevel_output_enter_cb,
	.output_leave = _toplevel_output_leave_cb,
	.state = _toplevel_state_cb,
	.done = (void (*)(void*, wfthandle*))gldi_wayland_wm_done,
	.closed = (void (*)(void*, wfthandle*))gldi_wayland_wm_closed,
	.parent = _toplevel_parent_cb
};

static void _toplevel_manager_toplevel(G_GNUC_UNUSED void *data, G_GNUC_UNUSED struct zwlr_foreign_toplevel_manager_v1 *manager, wfthandle *handle)
{
	GldiWaylandWindowActor* wactor = gldi_wayland_wm_new_toplevel(handle);
	zwlr_foreign_toplevel_handle_v1_set_user_data(handle, wactor);
	
	// Set default to show on all desktops.
	// Niri's workspaces are dynamic, so unless we have ext-foreign-toplevel-list,
	// we cannot definitively say which workspace ID a window belongs to.
	// However, showing on all keeps the dock usable (windows don't disappear).
	((GldiWindowActor*)wactor)->iNumDesktop = GLDI_DESKTOP_ALL;
	
	// Initial Geometry Hack (Center of screen)
	GldiWindowActor *actor = (GldiWindowActor*)wactor;
	actor->windowGeometry.x = cairo_dock_get_screen_width(0) / 2;
	actor->windowGeometry.y = cairo_dock_get_screen_height(0) / 2;
	actor->windowGeometry.width = 1;
	actor->windowGeometry.height = 1;

	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, wactor);
}

static struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = _toplevel_manager_toplevel,
	.finished = (void (*)(void*, struct zwlr_foreign_toplevel_manager_v1*))cd_message,
};

static void _destroy_handle(gpointer handle) {
	zwlr_foreign_toplevel_handle_v1_destroy((wfthandle*)handle);
}

/**********************************************************************
 * Initialization / Entry Point                                       */

gboolean gldi_niri_toplevel_match_protocol(uint32_t id, const char *interface, uint32_t version)
{
	if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		toplevel_found = TRUE;
		toplevel_id = id;
		toplevel_ver = version;
		return TRUE;
	}
	if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
		workspace_found = TRUE;
		workspace_id = id;
		workspace_ver = version;
		return TRUE;
	}
	return FALSE;
}

gboolean gldi_niri_toplevel_try_init(struct wl_registry *registry)
{
	// We require at least the toplevel manager to function
	if (!toplevel_found) return FALSE;

	// Bind Toplevel Manager
	toplevel_ver = MIN(toplevel_ver, zwlr_foreign_toplevel_manager_v1_interface.version);
	s_ptoplevel_manager = wl_registry_bind(registry, toplevel_id, &zwlr_foreign_toplevel_manager_v1_interface, toplevel_ver);

	// Bind Workspace Manager (if available)
	if (workspace_found) {
		workspace_ver = MIN(workspace_ver, ext_workspace_manager_v1_interface.version);
		s_pworkspace_manager = wl_registry_bind(registry, workspace_id, &ext_workspace_manager_v1_interface, workspace_ver);
	}

	if (s_ptoplevel_manager) {
		// Start listening to protocols
		zwlr_foreign_toplevel_manager_v1_add_listener(s_ptoplevel_manager, &toplevel_manager_listener, NULL);
		
		// If workspace manager exists, register it with Cairo-Dock's ext-workspace helper
		// This populates the "Desktops" list in the dock
		if (s_pworkspace_manager)
			gldi_ext_workspaces_register_manager(s_pworkspace_manager);

		// Register the Window Manager Backend
		GldiWindowManagerBackend wmb = {0};
		wmb.get_active_window = gldi_wayland_wm_get_active_window;
		wmb.move_to_nth_desktop = _move_to_nth_desktop; // IPC based
		wmb.show = _show;
		wmb.close = _close;
		wmb.minimize = _minimize;
		wmb.maximize = _maximize;
		wmb.set_fullscreen = _set_fullscreen;
		wmb.set_thumbnail_area = _set_thumbnail_area;
		wmb.get_transient_for = _get_transient_for;
		wmb.can_minimize_maximize_close = _can_minimize_maximize_close;
		wmb.pick_window = gldi_wayland_wm_pick_window;
		wmb.get_supported_actions = _get_supported_actions;
		
		// Flags: No Viewport Overlap (Tiling WM), Relative Geometry, Has Workspaces
		int flags = GLDI_WM_NO_VIEWPORT_OVERLAP | GLDI_WM_GEOM_REL_TO_VIEWPORT;
		if (s_pworkspace_manager) flags |= GLDI_WM_HAVE_WORKSPACES;
		
		wmb.flags = GINT_TO_POINTER(flags);
		wmb.name = "Niri";
		
		gldi_windows_manager_register_backend(&wmb);
		gldi_wayland_wm_init(_destroy_handle);
		
		cd_message("Niri integration initialized successfully.");
		return TRUE;
	}
	
	return FALSE;
}

#endif
