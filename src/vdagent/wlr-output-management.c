/* wlr-output-management.c - see wlr-output-management.h

    Copyright 2024 Red Hat, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/wayland/gdkwayland.h>
#include <syslog.h>

#include "vdagentd-proto.h"
#include "spice/vd_agent.h"
#include "wlr-protocol/wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-output-management.h"

/* Binds the same shared wl_display GDK already owns (see clipboard.c's
 * file-level comment for why that means our listeners keep getting
 * dispatched for free, without needing our own GMainLoop integration). */

typedef struct {
    VDAgentWlrOutputMgmt *mgmt; /* borrowed */
    struct zwlr_output_head_v1 *head;
    char *name; /* owned; NULL until the name event arrives */
} HeadInfo;

struct VDAgentWlrOutputMgmt {
    struct wl_display *wl_display; /* borrowed from GDK; not ours to destroy */
    struct wl_registry *wl_registry;
    struct zwlr_output_manager_v1 *manager; /* NULL if compositor doesn't implement this */
    uint32_t serial;
    gboolean got_done;
    GHashTable *heads_by_name; /* owned: char* -> HeadInfo* */
    GHashTable *connector_mapping; /* refed; connector name -> SPICE display id */
};

/* ---- zwlr_output_mode_v1: we only ever set_custom_mode(), never
 * set_mode(), so nothing about a mode is worth tracking. The proxy still
 * has to stay alive until the compositor says the mode is gone: a head's
 * current_mode event names one of its modes by object id, and libwayland
 * treats an id it no longer knows as a fatal protocol error. Destroying
 * each mode as soon as it was introduced therefore took the whole agent
 * down ("Error reading events from display: Invalid argument") on the
 * first mode change after startup - which is every time the SPICE client
 * resizes the guest. */

static void mode_noop_size(void *d, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h) { (void)d; (void)m; (void)w; (void)h; }
static void mode_noop_refresh(void *d, struct zwlr_output_mode_v1 *m, int32_t r) { (void)d; (void)m; (void)r; }
static void mode_noop_preferred(void *d, struct zwlr_output_mode_v1 *m) { (void)d; (void)m; }

static void mode_finished(void *data, struct zwlr_output_mode_v1 *mode)
{
    (void)data;
    /* release (v3) tells the compositor it may free its side too; before
     * v3 there is no destructor request and destroy is client-side only. */
    if (wl_proxy_get_version((struct wl_proxy *)mode) >=
        ZWLR_OUTPUT_MODE_V1_RELEASE_SINCE_VERSION) {
        zwlr_output_mode_v1_release(mode);
    } else {
        zwlr_output_mode_v1_destroy(mode);
    }
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
    mode_noop_size,
    mode_noop_refresh,
    mode_noop_preferred,
    mode_finished,
};

static void head_mode(void *data, struct zwlr_output_head_v1 *head, struct zwlr_output_mode_v1 *mode)
{
    (void)data;
    (void)head;
    zwlr_output_mode_v1_add_listener(mode, &mode_listener, NULL);
}

/* ---- zwlr_output_head_v1: only name/mode/finished carry real logic; the
 * rest (description, physical_size, enabled, current_mode, position,
 * transform, scale, make, model, serial_number, adaptive_sync) MUST still
 * have a handler -- wl_proxy_add_listener requires every field the bound
 * interface version can send to be non-NULL, or the dispatcher calls
 * through a NULL function pointer the moment the compositor sends one. */

static void head_name(void *data, struct zwlr_output_head_v1 *head, const char *name)
{
    HeadInfo *info = data;
    (void)head;
    g_free(info->name);
    info->name = g_strdup(name);
    g_hash_table_replace(info->mgmt->heads_by_name, g_strdup(name), info);
}

static void head_finished(void *data, struct zwlr_output_head_v1 *head)
{
    HeadInfo *info = data;
    if (info->name) {
        g_hash_table_remove(info->mgmt->heads_by_name, info->name);
    }
    zwlr_output_head_v1_destroy(head);
    g_free(info->name);
    g_free(info);
}

static void head_noop_description(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_noop_physical_size(void *d, struct zwlr_output_head_v1 *h, int32_t w, int32_t ht) { (void)d; (void)h; (void)w; (void)ht; }
static void head_noop_enabled(void *d, struct zwlr_output_head_v1 *h, int32_t e) { (void)d; (void)h; (void)e; }
static void head_noop_current_mode(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m) { (void)d; (void)h; (void)m; }
static void head_noop_position(void *d, struct zwlr_output_head_v1 *h, int32_t x, int32_t y) { (void)d; (void)h; (void)x; (void)y; }
static void head_noop_transform(void *d, struct zwlr_output_head_v1 *h, int32_t t) { (void)d; (void)h; (void)t; }
static void head_noop_scale(void *d, struct zwlr_output_head_v1 *h, wl_fixed_t s) { (void)d; (void)h; (void)s; }
static void head_noop_make(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_noop_model(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_noop_serial_number(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_noop_adaptive_sync(void *d, struct zwlr_output_head_v1 *h, uint32_t s) { (void)d; (void)h; (void)s; }

static const struct zwlr_output_head_v1_listener head_listener = {
    head_name,
    head_noop_description,
    head_noop_physical_size,
    head_mode,
    head_noop_enabled,
    head_noop_current_mode,
    head_noop_position,
    head_noop_transform,
    head_noop_scale,
    head_finished,
    head_noop_make,
    head_noop_model,
    head_noop_serial_number,
    head_noop_adaptive_sync,
};

/* ---- zwlr_output_manager_v1 ---- */

static void manager_head(void *data, struct zwlr_output_manager_v1 *manager,
                          struct zwlr_output_head_v1 *head)
{
    VDAgentWlrOutputMgmt *mgmt = data;
    (void)manager;

    HeadInfo *info = g_new0(HeadInfo, 1);
    info->mgmt = mgmt;
    info->head = head;
    zwlr_output_head_v1_add_listener(head, &head_listener, info);
}

static void manager_done(void *data, struct zwlr_output_manager_v1 *manager, uint32_t serial)
{
    VDAgentWlrOutputMgmt *mgmt = data;
    (void)manager;
    mgmt->serial = serial;
    mgmt->got_done = TRUE;
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *manager)
{
    VDAgentWlrOutputMgmt *mgmt = data;
    (void)manager;
    /* Compositor is tearing the global down (e.g. it's being disabled or
     * the session is ending). Nothing to reconnect to for the process's
     * remaining lifetime -- vdagent_wlr_output_mgmt_set_config() checks
     * mgmt->manager and simply won't find it usable any more. */
    mgmt->manager = NULL;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
    manager_head,
    manager_done,
    manager_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t version)
{
    VDAgentWlrOutputMgmt *mgmt = data;
    if (g_strcmp0(interface, "zwlr_output_manager_v1") == 0) {
        mgmt->manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface,
                                          MIN(version, 4));
        zwlr_output_manager_v1_add_listener(mgmt->manager, &manager_listener, mgmt);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

/* ---- zwlr_output_configuration_v1: the outcome of one apply() ---- */

typedef struct {
    VDAgentDisplay *display; /* borrowed; outlives every request this file makes */
} ConfigRequest;

static void configuration_done(ConfigRequest *req, struct zwlr_output_configuration_v1 *config,
                                const char *outcome)
{
    syslog(LOG_DEBUG, "%s: compositor %s the monitor config", __func__, outcome);
    /* Report back whatever the compositor actually ended up with -- on
     * success this is the new resolution, on failure/cancellation it's
     * whatever was already there, letting the client know its request
     * didn't stick either way. */
    vdagent_display_send_daemon_guest_res(req->display, TRUE);
    zwlr_output_configuration_v1_destroy(config);
    g_free(req);
}

static void configuration_succeeded(void *data, struct zwlr_output_configuration_v1 *config)
{
    configuration_done(data, config, "applied");
}

static void configuration_failed(void *data, struct zwlr_output_configuration_v1 *config)
{
    configuration_done(data, config, "rejected");
}

static void configuration_cancelled(void *data, struct zwlr_output_configuration_v1 *config)
{
    configuration_done(data, config, "cancelled (output state changed mid-request)");
}

static const struct zwlr_output_configuration_v1_listener configuration_listener = {
    configuration_succeeded,
    configuration_failed,
    configuration_cancelled,
};

/* ---- public API ---- */

VDAgentWlrOutputMgmt *vdagent_wlr_output_mgmt_create(GHashTable *connector_mapping)
{
    VDAgentWlrOutputMgmt *mgmt = g_new0(VDAgentWlrOutputMgmt, 1);
    mgmt->connector_mapping = g_hash_table_ref(connector_mapping);
    mgmt->heads_by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GdkDisplay *gdk_display = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        return mgmt; /* manager stays NULL; set_config() will report "unsupported" */
    }

    mgmt->wl_display = gdk_wayland_display_get_wl_display(gdk_display);
    mgmt->wl_registry = wl_display_get_registry(mgmt->wl_display);
    wl_registry_add_listener(mgmt->wl_registry, &registry_listener, mgmt);
    wl_display_roundtrip(mgmt->wl_display); /* globals bound */
    if (mgmt->manager) {
        wl_display_roundtrip(mgmt->wl_display); /* initial heads + done */
    }

    return mgmt;
}

void vdagent_wlr_output_mgmt_destroy(VDAgentWlrOutputMgmt *mgmt)
{
    if (!mgmt) {
        return;
    }
    g_hash_table_unref(mgmt->heads_by_name);
    g_hash_table_unref(mgmt->connector_mapping);
    g_free(mgmt);
}

gboolean vdagent_wlr_output_mgmt_set_config(VDAgentWlrOutputMgmt *mgmt, VDAgentDisplay *display,
                                             VDAgentMonitorsConfig *mon_config)
{
    if (!mgmt->manager || !mgmt->got_done) {
        return FALSE;
    }

    /* Make sure our head list and serial reflect the compositor's current
     * state before building a configuration against them -- create_
     * configuration() with a stale serial gets cancelled. */
    wl_display_roundtrip(mgmt->wl_display);

    guint n_enabled = 0;
    for (guint i = 0; i < mon_config->num_of_monitors; i++) {
        if (mon_config->monitors[i].width && mon_config->monitors[i].height) {
            n_enabled++;
        }
    }
    if (n_enabled == 0) {
        syslog(LOG_WARNING, "%s: client sent config with all monitors disabled, ignoring",
               __func__);
        vdagent_display_send_daemon_guest_res(display, TRUE);
        return TRUE;
    }

    struct zwlr_output_configuration_v1 *config =
        zwlr_output_manager_v1_create_configuration(mgmt->manager, mgmt->serial);

    /* It is a protocol error to omit a head from the configuration, so
     * every currently known head needs an enable_head or disable_head
     * call, not just the ones mon_config actually changes. */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mgmt->heads_by_name);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *connector = key;
        HeadInfo *info = value;

        gpointer display_id_ptr = NULL;
        gboolean have_display_id =
            g_hash_table_lookup_extended(mgmt->connector_mapping, connector, NULL, &display_id_ptr);
        guint display_id = GPOINTER_TO_UINT(display_id_ptr);

        const VDAgentMonConfig *mon = NULL;
        if (have_display_id && display_id < mon_config->num_of_monitors) {
            mon = &mon_config->monitors[display_id];
        }

        if (mon && mon->width && mon->height) {
            struct zwlr_output_configuration_head_v1 *cfg_head =
                zwlr_output_configuration_v1_enable_head(config, info->head);
            zwlr_output_configuration_head_v1_set_custom_mode(cfg_head, mon->width, mon->height, 0);
            zwlr_output_configuration_head_v1_set_position(cfg_head, mon->x, mon->y);
            /* No destroy request exists for this interface -- the
             * compositor destroys it along with the parent configuration.
             * This just frees our local proxy handle; see head_mode()'s
             * comment for the same pattern. */
            zwlr_output_configuration_head_v1_destroy(cfg_head);
        } else {
            zwlr_output_configuration_v1_disable_head(config, info->head);
        }
    }

    ConfigRequest *req = g_new(ConfigRequest, 1);
    req->display = display;
    zwlr_output_configuration_v1_add_listener(config, &configuration_listener, req);
    zwlr_output_configuration_v1_apply(config);
    wl_display_flush(mgmt->wl_display);

    return TRUE;
}
