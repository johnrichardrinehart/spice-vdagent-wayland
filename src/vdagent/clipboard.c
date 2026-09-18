/*  clipboard.c - vdagent clipboard handling code

    Copyright 2017 Red Hat, Inc.

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

#ifdef USE_GTK_FOR_CLIPBOARD
# include <gtk/gtk.h>
# include <gdk/wayland/gdkwayland.h>
# include <gio/gio.h>
# include <gio/gunixinputstream.h>
# include <gio/gunixoutputstream.h>
# include <string.h>
# include <syslog.h>
# include <unistd.h>

# include "vdagentd-proto.h"
# include "spice/vd_agent.h"
# include "wlr-protocol/wlr-data-control-unstable-v1-client-protocol.h"
#endif

#include "clipboard.h"

#ifdef USE_GTK_FOR_CLIPBOARD
/* GTK4 port notes (Phase 1+2+3: text and image formats (PNG/BMP/TIFF/JPG),
 * both CLIPBOARD and PRIMARY selections).
 *
 * Two entirely different mechanisms are used for the two directions, and
 * that split is deliberate, not incidental:
 *
 * - Host has new data, guest should see it (vdagent_clipboard_grab, "the
 *   write side"): uses GTK4's ordinary GdkClipboard (gdk_clipboard_set_
 *   content + VdagentClipboardProvider below). This works fine as-is --
 *   *offering* a selection has never been the problem here.
 *
 * - Guest copies something, host should see it ("the observe side"): does
 *   NOT use GdkClipboard at all. Per the Wayland protocol specification
 *   (not a GDK or Hyprland bug -- confirmed by reading both GDK's and
 *   Hyprland's actual source), a client only receives wl_data_device
 *   selection-changed notifications while it holds keyboard focus. vdagent
 *   is a headless background daemon with no window a user would ever
 *   focus, so it structurally can never observe another client's clipboard
 *   change through the standard wl_data_device protocol GdkClipboard is
 *   built on -- confirmed via GDK_DEBUG=clipboard,events: the offer's
 *   mime-type list came back empty every time because the compositor
 *   never routed the full data_offer event sequence to an unfocused
 *   client, exactly as the spec allows.
 *
 *   The fix is a different, purpose-built Wayland protocol:
 *   wlr-data-control-unstable-v1 (zwlr_data_control_manager_v1 and
 *   friends), designed specifically for focus-independent clipboard
 *   observation -- it's what clipboard managers and `wl-paste --watch`
 *   actually use, for exactly this reason. See data_control_* below.
 */
/* VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD / _PRIMARY are 0 / 1, matching
 * these indices directly. */
#define SELECTION_COUNT 2

#define TEXT_MIME_TYPE "text/plain;charset=utf-8"

/* VD_AGENT_CLIPBOARD_* type ids index directly into TYPE_COUNT-sized
 * arrays below (NONE's slot 0 is simply never populated). */
#define TYPE_COUNT (VD_AGENT_CLIPBOARD_IMAGE_JPG + 1)

/* A spice type can have more than one recognized mime spelling (image/bmp
 * has three real-world aliases) but a mime string maps to exactly one
 * type, so this table is looked up two different ways below. */
static const struct {
    guint       type;
    const char *mime_type;
} type2mime[] = {
    {VD_AGENT_CLIPBOARD_UTF8_TEXT,  TEXT_MIME_TYPE},
    {VD_AGENT_CLIPBOARD_IMAGE_PNG,  "image/png"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP,  "image/bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP,  "image/x-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP,  "image/x-MS-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP,  "image/x-win-bitmap"},
    {VD_AGENT_CLIPBOARD_IMAGE_TIFF, "image/tiff"},
    {VD_AGENT_CLIPBOARD_IMAGE_JPG,  "image/jpeg"},
};

static guint type_from_mime_type(const char *mime_type)
{
    for (guint i = 0; i < G_N_ELEMENTS(type2mime); i++) {
        if (!g_ascii_strcasecmp(mime_type, type2mime[i].mime_type)) {
            return type2mime[i].type;
        }
    }
    return VD_AGENT_CLIPBOARD_NONE;
}

/* Canonical (first-listed) mime spelling for a spice type -- used on the
 * write side, where we choose what string to advertise/request. The
 * observe side instead keeps whatever exact string the guest offered (see
 * Selection.current_mime below): a source app may only recognize the
 * specific alias spelling it advertised, so we must ask it back for that
 * same one rather than our own canonical choice. */
static const char *mime_type_for_type(guint type)
{
    for (guint i = 0; i < G_N_ELEMENTS(type2mime); i++) {
        if (type2mime[i].type == type) {
            return type2mime[i].mime_type;
        }
    }
    return NULL;
}

enum {
    OWNER_NONE,
    OWNER_GUEST,
    OWNER_CLIENT
};

typedef struct {
    /* write side (host -> guest): ordinary GdkClipboard */
    GdkClipboard *clipboard;
    guint         owner;
    GList        *requests_from_apps; /* GTask* list: VDAgent --> Client (guest paste of our data) */
    /* which types the client told us it can supply, from the most recent
     * vdagent_clipboard_grab() -- read by ref_formats (what to advertise)
     * and write_mime_type_async (what to accept a request for). */
    gboolean      type_available[TYPE_COUNT];

    /* observe side (guest -> host): wlr-data-control, see above.
     * pending_mime lives on VDAgentClipboards, not here -- a
     * freshly-introduced offer (data_offer event) doesn't say which
     * selection it's for until the following selection/primary_selection
     * event arrives, so there's nothing to key a per-Selection pending
     * state on yet. */
    struct zwlr_data_control_offer_v1 *current_offer; /* finalized; safe to receive() from */
    char         *current_mime[TYPE_COUNT]; /* owned; exact string the guest offered, NULL if absent */
    /* zwlr_data_control_device_v1's "selection" event fires for every
     * seat-wide selection change, including ones this same process just
     * caused via gdk_clipboard_set_content() on the *other* protocol
     * object (regular wl_data_device, via GDK) -- there's no is_local()
     * equivalent for data-control the way GdkClipboard has, so it can't
     * tell "the guest just copied something" apart from "we just wrote
     * the host's clipboard" on its own. Set right before every
     * gdk_clipboard_set_content() call and consumed by the very next
     * data_control_selection callback, which is that write's own echo.
     * Only used on the GdkClipboard fallback path below: when the write
     * goes out over data-control we own the selection outright, and
     * sel->source is a far sturdier way to recognise our own echo. */
    gboolean      expect_own_selection;

    /* write side over data-control. Setting a selection through the
     * ordinary wl_data_device -- which is all GdkClipboard can do -- needs
     * a serial from an input event the client received, and a headless
     * agent with no surface never has one, so on a compositor that checks
     * (niri does) gdk_clipboard_set_content() silently owns nothing.
     * zwlr_data_control_device_v1 exists precisely to let a focus-less
     * client take the selection, so prefer it and keep GdkClipboard as the
     * fallback for compositors without the protocol. */
    struct zwlr_data_control_source_v1 *source; /* ours while owner == OWNER_CLIENT */
    GList        *sends_to_guest;               /* PendingSend*, awaiting client data */
} Selection;

#define VDAGENT_TYPE_CLIPBOARD_PROVIDER (vdagent_clipboard_provider_get_type())
G_DECLARE_FINAL_TYPE(VdagentClipboardProvider, vdagent_clipboard_provider,
                     VDAGENT, CLIPBOARD_PROVIDER, GdkContentProvider)

struct _VdagentClipboardProvider {
    GdkContentProvider parent;
    VDAgentClipboards *clipboards; /* borrowed; outlives every provider it creates */
    guint sel_id;
};

G_DEFINE_FINAL_TYPE(VdagentClipboardProvider, vdagent_clipboard_provider, GDK_TYPE_CONTENT_PROVIDER)

/* Both of these are defined further down, once struct _VDAgentClipboards
 * (which they dereference, via self->clipboards->selections[...]) is
 * actually complete. */
static GdkContentFormats *vdagent_clipboard_provider_ref_formats(GdkContentProvider *provider);

static void vdagent_clipboard_provider_write_mime_type_async(
    GdkContentProvider *provider, const char *mime_type, GOutputStream *stream,
    int io_priority, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

static gboolean vdagent_clipboard_provider_write_mime_type_finish(
    GdkContentProvider *provider, GAsyncResult *result, GError **error)
{
    (void)provider;
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void vdagent_clipboard_provider_class_init(VdagentClipboardProviderClass *klass)
{
    GdkContentProviderClass *provider_class = GDK_CONTENT_PROVIDER_CLASS(klass);
    provider_class->ref_formats = vdagent_clipboard_provider_ref_formats;
    provider_class->write_mime_type_async = vdagent_clipboard_provider_write_mime_type_async;
    provider_class->write_mime_type_finish = vdagent_clipboard_provider_write_mime_type_finish;
}

static void vdagent_clipboard_provider_init(VdagentClipboardProvider *self)
{
    (void)self;
}
#endif

struct _VDAgentClipboards {
    GObject parent;

#ifdef USE_GTK_FOR_CLIPBOARD
    UdscsConnection *conn;

    Selection selections[SELECTION_COUNT];

    struct wl_display *wl_display; /* borrowed from GDK; not ours to destroy */
    struct wl_registry *wl_registry;
    struct zwlr_data_control_manager_v1 *data_control_manager;
    struct zwlr_data_control_device_v1 *data_control_device;

    /* being built for whichever offer was most recently introduced by a
     * data_offer event; see the Selection typedef above for why this
     * isn't keyed per-selection. */
    struct zwlr_data_control_offer_v1 *pending_offer;
    char *pending_mime[TYPE_COUNT]; /* owned; NULL where not (yet) offered */
#else
    struct vdagent_x11 *x11;
#endif
};

struct _VDAgentClipboardsClass
{
    GObjectClass parent;
};

G_DEFINE_TYPE(VDAgentClipboards, vdagent_clipboards, G_TYPE_OBJECT)

#ifdef USE_GTK_FOR_CLIPBOARD
static GdkContentFormats *vdagent_clipboard_provider_ref_formats(GdkContentProvider *provider)
{
    VdagentClipboardProvider *self = VDAGENT_CLIPBOARD_PROVIDER(provider);
    Selection *sel = &self->clipboards->selections[self->sel_id];

    GdkContentFormatsBuilder *builder = gdk_content_formats_builder_new();
    for (guint type = 0; type < TYPE_COUNT; type++) {
        if (sel->type_available[type]) {
            gdk_content_formats_builder_add_mime_type(builder, mime_type_for_type(type));
        }
    }
    return gdk_content_formats_builder_free_to_formats(builder);
}

static void vdagent_clipboard_provider_write_mime_type_async(
    GdkContentProvider *provider, const char *mime_type, GOutputStream *stream,
    int io_priority, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    (void)io_priority;
    VdagentClipboardProvider *self = VDAGENT_CLIPBOARD_PROVIDER(provider);
    VDAgentClipboards *c = self->clipboards;
    Selection *sel = &c->selections[self->sel_id];

    GTask *task = g_task_new(provider, cancellable, callback, user_data);

    guint type = type_from_mime_type(mime_type);
    if (type == VD_AGENT_CLIPBOARD_NONE || !sel->type_available[type]) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "unsupported mime type %s", mime_type);
        g_object_unref(task);
        return;
    }

    g_task_set_task_data(task, g_object_ref(stream), g_object_unref);
    /* vdagent_clipboard_data() only carries a type id back from the
     * client, not the mime string GTK asked for -- remember which type
     * this task is waiting on so it can be matched back up. */
    g_object_set_data(G_OBJECT(task), "vdagent-type", GUINT_TO_POINTER(type));
    sel->requests_from_apps = g_list_append(sel->requests_from_apps, task);

    udscs_write(c->conn, VDAGENTD_CLIPBOARD_REQUEST, self->sel_id, type, NULL, 0);
}

/* Cancel every pending write-side request and mark no one owns the
 * selection any more -- called whenever ownership is about to change out
 * from under in-flight requests, so nothing completes against stale state.
 */
static void clipboard_new_owner(VDAgentClipboards *c, guint sel_id, guint new_owner)
{
    Selection *sel = &c->selections[sel_id];
    GList *l;

    for (l = sel->requests_from_apps; l != NULL; l = l->next) {
        GTask *task = l->data;
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "clipboard ownership changed");
        g_object_unref(task);
    }
    g_clear_pointer(&sel->requests_from_apps, g_list_free);

    sel->owner = new_owner;
}

/* ---- write side over data-control (see the Selection comment) ---- */

/* One guest application waiting on the read end of a pipe while we fetch
 * the data it asked for from the client. */
typedef struct {
    guint          type;
    GOutputStream *stream; /* the pipe fd the compositor handed us */
} PendingSend;

static void pending_send_free(PendingSend *ps)
{
    g_clear_object(&ps->stream);
    g_free(ps);
}

/* Drop our source and fail anything still waiting on it. Safe to call when
 * we do not currently own the selection. */
static void clipboard_source_clear(VDAgentClipboards *c, guint sel_id)
{
    Selection *sel = &c->selections[sel_id];

    g_list_free_full(g_steal_pointer(&sel->sends_to_guest),
                     (GDestroyNotify)pending_send_free);
    g_clear_pointer(&sel->source, zwlr_data_control_source_v1_destroy);
}

static void data_control_send_written_cb(GObject *source, GAsyncResult *result,
                                         gpointer user_data)
{
    GOutputStream *stream = G_OUTPUT_STREAM(source);
    GBytes *bytes = user_data;
    GError *error = NULL;

    if (!g_output_stream_write_all_finish(stream, result, NULL, &error)) {
        syslog(LOG_WARNING, "%s: %s", __func__,
               error ? error->message : "write failed");
        g_clear_error(&error);
    }
    g_bytes_unref(bytes);
    /* Closing is what tells the reader the selection data has ended. */
    g_output_stream_close(stream, NULL, NULL);
    g_object_unref(stream);
}

/* A guest application is pasting: the compositor hands us the mime type it
 * wants and a pipe to write it into. The data lives on the client, so ask
 * for it and answer once vdagent_clipboard_data() brings it back. */
static void data_control_source_send(void *data, struct zwlr_data_control_source_v1 *source,
                                     const char *mime_type, int32_t fd)
{
    VDAgentClipboards *c = data;

    for (guint sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        Selection *sel = &c->selections[sel_id];
        if (sel->source != source) {
            continue;
        }

        guint type = type_from_mime_type(mime_type);
        if (type == VD_AGENT_CLIPBOARD_NONE || !sel->type_available[type]) {
            syslog(LOG_WARNING, "%s: sel_id=%u: guest asked for unoffered type %s",
                   __func__, sel_id, mime_type);
            close(fd);
            return;
        }

        PendingSend *ps = g_new0(PendingSend, 1);
        ps->type = type;
        ps->stream = g_unix_output_stream_new(fd, TRUE);
        sel->sends_to_guest = g_list_append(sel->sends_to_guest, ps);

        udscs_write(c->conn, VDAGENTD_CLIPBOARD_REQUEST, sel_id, type, NULL, 0);
        return;
    }

    /* A source we have already replaced; nothing can answer for it. */
    close(fd);
}

static void data_control_source_cancelled(void *data,
                                          struct zwlr_data_control_source_v1 *source)
{
    VDAgentClipboards *c = data;

    for (guint sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        Selection *sel = &c->selections[sel_id];
        if (sel->source != source) {
            continue;
        }
        /* Someone else took the selection. Release ownership before the
         * selection event for the new owner arrives, so that event is read
         * as a genuine guest copy rather than suppressed as our own echo.
         */
        clipboard_source_clear(c, sel_id);
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        return;
    }

    zwlr_data_control_source_v1_destroy(source);
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
    data_control_source_send,
    data_control_source_cancelled,
};

/* ---- observe side: wlr-data-control (see the file-level comment) ---- */

static void data_control_offer_offer(void *data, struct zwlr_data_control_offer_v1 *offer,
                                      const char *mime_type)
{
    VDAgentClipboards *c = data;
    (void)offer;
    guint type = type_from_mime_type(mime_type);
    if (type == VD_AGENT_CLIPBOARD_NONE) {
        return; /* a format we don't handle, e.g. text/uri-list */
    }
    /* first alias wins if the source somehow offers more than one for the
     * same type (e.g. both image/bmp and image/x-bmp) */
    if (c->pending_mime[type] == NULL) {
        c->pending_mime[type] = g_strdup(mime_type);
    }
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    data_control_offer_offer,
};

static void data_control_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
                                     struct zwlr_data_control_offer_v1 *offer)
{
    VDAgentClipboards *c = data;
    (void)device;

    /* defensive: a prior data_offer sequence should always have been
     * consumed by the selection event that follows it, but don't leak its
     * strings if that assumption is ever wrong. */
    for (guint type = 0; type < TYPE_COUNT; type++) {
        g_clear_pointer(&c->pending_mime[type], g_free);
    }
    c->pending_offer = offer;
    zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, c);
}

/* Shared by data_control_selection (CLIPBOARD) and
 * data_control_primary_selection (PRIMARY) -- same finalize logic once
 * the sel_id is known, just applied to a different Selection slot. */
static void data_control_offer_finalized(VDAgentClipboards *c, guint sel_id,
                                          struct zwlr_data_control_offer_v1 *offer)
{
    Selection *sel = &c->selections[sel_id];

    if (sel->current_offer) {
        zwlr_data_control_offer_v1_destroy(sel->current_offer);
        sel->current_offer = NULL;
    }
    for (guint type = 0; type < TYPE_COUNT; type++) {
        g_clear_pointer(&sel->current_mime[type], g_free);
    }

    if ((sel->owner == OWNER_CLIENT && sel->source) || sel->expect_own_selection) {
        /* echo of our own vdagent_clipboard_grab() -- not a real guest
         * change. We still have to consume/destroy the offer object (it's
         * real, just uninteresting) and drop whatever mime types this
         * offer accumulated, but must not treat it as GUEST taking
         * ownership.
         *
         * While the selection is ours over data-control, sel->source says
         * so for as long as it stays ours, rather than for exactly one
         * event: the compositor re-announces the selection more than once
         * in practice, and a one-shot flag lets the second announcement
         * through as a phantom guest copy, which we then grab straight
         * back to the client in an endless loop. Ownership ends at the
         * source's cancelled event, which fires before the new owner's
         * selection event. */
        sel->expect_own_selection = FALSE;
        for (guint type = 0; type < TYPE_COUNT; type++) {
            g_clear_pointer(&c->pending_mime[type], g_free);
        }
        if (offer) {
            zwlr_data_control_offer_v1_destroy(offer);
        }
        return;
    }

    if (!offer) {
        /* guest cleared its clipboard */
        if (sel->owner == OWNER_GUEST) {
            clipboard_new_owner(c, sel_id, OWNER_NONE);
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
        }
        return;
    }

    /* This offer was introduced by the data_offer event immediately
     * preceding this one, so its accumulated mime types are still our
     * pending ones. */
    sel->current_offer = offer;
    guint32 types[TYPE_COUNT];
    guint n_types = 0;
    for (guint type = 0; type < TYPE_COUNT; type++) {
        sel->current_mime[type] = g_steal_pointer(&c->pending_mime[type]);
        if (sel->current_mime[type]) {
            types[n_types++] = type;
        }
    }

    if (n_types == 0) {
        return; /* nothing in a format we support (yet) */
    }

    clipboard_new_owner(c, sel_id, OWNER_GUEST);
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_GRAB, sel_id, 0,
               (guint8 *)types, n_types * sizeof(guint32));
}

static void data_control_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                    struct zwlr_data_control_offer_v1 *offer)
{
    (void)device;
    data_control_offer_finalized(data, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, offer);
}

static void data_control_primary_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                            struct zwlr_data_control_offer_v1 *offer)
{
    (void)device;
    data_control_offer_finalized(data, VD_AGENT_CLIPBOARD_SELECTION_PRIMARY, offer);
}

static void data_control_finished(void *data, struct zwlr_data_control_device_v1 *device)
{
    /* Compositor tore down our data-control device (e.g. seat removed).
     * Nothing to reconnect to for the process's remaining lifetime. */
    (void)data;
    (void)device;
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
    data_control_data_offer,
    data_control_selection,
    data_control_finished,
    data_control_primary_selection,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    VDAgentClipboards *c = data;
    if (g_strcmp0(interface, "zwlr_data_control_manager_v1") == 0) {
        c->data_control_manager = wl_registry_bind(registry, name,
                                                    &zwlr_data_control_manager_v1_interface,
                                                    MIN(version, 2));
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

typedef struct {
    VDAgentClipboards *c;
    guint sel_id;
    guint type;
} ReceiveRequest;

/* Data lands in a pipe from zwlr_data_control_offer_v1_receive(); read it
 * into a growable buffer and hand it to the client. */
static void data_control_splice_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ReceiveRequest *req = user_data;
    VDAgentClipboards *c = req->c;
    guint sel_id = req->sel_id;
    guint type = req->type;
    g_free(req);
    GError *error = NULL;
    GOutputStream *sink = G_OUTPUT_STREAM(source);

    gssize spliced = g_output_stream_splice_finish(sink, result, &error);
    if (spliced < 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: %s", __func__, sel_id,
               error ? error->message : "splice failed");
        g_clear_error(&error);
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                   VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        g_object_unref(sink);
        return;
    }

    gpointer data = g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(sink));
    gsize size = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(sink));
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id, type, data, size);
    g_object_unref(sink);
}
#endif

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_grab(c->x11, sel_id, types, n_types);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);

    Selection *sel = &c->selections[sel_id];
    for (guint type = 0; type < TYPE_COUNT; type++) {
        sel->type_available[type] = FALSE;
    }
    guint n_supported = 0;
    for (guint i = 0; i < n_types; i++) {
        if (types[i] < TYPE_COUNT && mime_type_for_type(types[i])) {
            sel->type_available[types[i]] = TRUE;
            n_supported++;
        }
    }
    if (n_supported == 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no supported type offered", __func__, sel_id);
        return;
    }

    if (c->data_control_device) {
        gboolean primary = (sel_id == VD_AGENT_CLIPBOARD_SELECTION_PRIMARY);

        if (primary &&
            wl_proxy_get_version((struct wl_proxy *)c->data_control_device) <
                ZWLR_DATA_CONTROL_DEVICE_V1_SET_PRIMARY_SELECTION_SINCE_VERSION) {
            goto gdk_fallback;
        }

        /* Replacing our own selection: drop the previous source first, so
         * its cancelled event cannot clear the ownership we are about to
         * take. */
        clipboard_source_clear(c, sel_id);

        sel->source = zwlr_data_control_manager_v1_create_data_source(c->data_control_manager);
        zwlr_data_control_source_v1_add_listener(sel->source, &source_listener, c);
        for (guint type = 0; type < TYPE_COUNT; type++) {
            if (sel->type_available[type]) {
                zwlr_data_control_source_v1_offer(sel->source, mime_type_for_type(type));
            }
        }

        /* Ownership has to be recorded before the request reaches the
         * compositor, or the resulting selection event races us and is
         * mistaken for a guest copy. */
        clipboard_new_owner(c, sel_id, OWNER_CLIENT);

        if (primary) {
            zwlr_data_control_device_v1_set_primary_selection(c->data_control_device, sel->source);
        } else {
            zwlr_data_control_device_v1_set_selection(c->data_control_device, sel->source);
        }
        wl_display_flush(c->wl_display);
        return;
    }

gdk_fallback:
    {
        VdagentClipboardProvider *provider =
            g_object_new(VDAGENT_TYPE_CLIPBOARD_PROVIDER, NULL);
        provider->clipboards = c;
        provider->sel_id = sel_id;

        sel->expect_own_selection = TRUE;
        gdk_clipboard_set_content(sel->clipboard, GDK_CONTENT_PROVIDER(provider));
        g_object_unref(provider);
        clipboard_new_owner(c, sel_id, OWNER_CLIENT);
    }
#endif
}

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_data(c->x11, sel_id, type, data, size);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    Selection *sel = &c->selections[sel_id];

    /* A guest application pasting through our data-control source is
     * waiting on a pipe rather than on a GdkContentProvider task. */
    for (GList *s = sel->sends_to_guest; s != NULL; s = s->next) {
        PendingSend *ps = s->data;
        if (ps->type != type) {
            continue;
        }
        sel->sends_to_guest = g_list_delete_link(sel->sends_to_guest, s);

        if (data == NULL || size == 0) {
            /* Client had nothing for this type; an empty pipe says so. */
            g_output_stream_close(ps->stream, NULL, NULL);
            pending_send_free(ps);
            return;
        }

        /* The buffer belongs to the caller and a pipe write can block on a
         * slow reader, so take a copy and let it drain asynchronously. */
        GBytes *bytes = g_bytes_new(data, size);
        gsize len = 0;
        gconstpointer buf = g_bytes_get_data(bytes, &len);
        g_output_stream_write_all_async(ps->stream, buf, len, G_PRIORITY_DEFAULT,
                                        NULL, data_control_send_written_cb, bytes);
        ps->stream = NULL; /* the async write owns it now */
        pending_send_free(ps);
        return;
    }

    /* Match by the type each request is actually waiting on, not queue
     * position -- more than one write_mime_type_async can be outstanding
     * at once (e.g. a paste target that probes both an image type and
     * text/plain in quick succession). */
    GList *l;
    for (l = sel->requests_from_apps; l != NULL; l = l->next) {
        guint expected = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(l->data), "vdagent-type"));
        if (expected == type) {
            break;
        }
    }
    if (l == NULL) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no pending request for type=%u, skipping",
               __func__, sel_id, type);
        return;
    }
    GTask *task = l->data;
    sel->requests_from_apps = g_list_delete_link(sel->requests_from_apps, l);

    GOutputStream *stream = g_task_get_task_data(task);
    GError *error = NULL;
    gboolean ok = g_output_stream_write_all(stream, data, size, NULL,
                                            g_task_get_cancellable(task), &error);
    if (ok) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
    g_object_unref(task);
#endif
}

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_release(c->x11, sel_id);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    if (c->selections[sel_id].owner != OWNER_CLIENT)
        return;

    gboolean had_source = (c->selections[sel_id].source != NULL);
    clipboard_source_clear(c, sel_id);
    clipboard_new_owner(c, sel_id, OWNER_NONE);
    if (had_source) {
        /* Destroying the source already drops the selection. */
        wl_display_flush(c->wl_display);
    } else {
        gdk_clipboard_set_content(c->selections[sel_id].clipboard, NULL);
    }
#endif
}

void vdagent_clipboards_release_all(VDAgentClipboards *c)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_client_disconnected(c->x11);
#else
    guint sel_id, owner;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        owner = c->selections[sel_id].owner;
        gboolean had_source = (c->selections[sel_id].source != NULL);
        clipboard_source_clear(c, sel_id);
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        if (owner == OWNER_CLIENT && !had_source)
            gdk_clipboard_set_content(c->selections[sel_id].clipboard, NULL);
        else if (owner == OWNER_GUEST && c->conn)
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
    }
#endif
}

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_request(c->x11, sel_id, type);
#else
    Selection *sel;

    if (sel_id >= SELECTION_COUNT || type >= TYPE_COUNT)
        goto err;
    sel = &c->selections[sel_id];
    if (sel->owner != OWNER_GUEST || !sel->current_offer || !sel->current_mime[type]) {
        syslog(LOG_WARNING, "%s: sel_id=%d: received request "
                            "while not owning clipboard", __func__, sel_id);
        goto err;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        syslog(LOG_WARNING, "%s: sel_id=%d: pipe() failed", __func__, sel_id);
        goto err;
    }
    /* Ask for the exact mime string the guest offered (sel->current_mime),
     * not our own canonical spelling -- see mime_type_for_type()'s comment
     * for why those can differ for the same type (e.g. BMP aliases). */
    zwlr_data_control_offer_v1_receive(sel->current_offer, sel->current_mime[type], pipe_fds[1]);
    close(pipe_fds[1]);
    wl_display_flush(c->wl_display); /* must reach the compositor before the source writes */

    GInputStream *src = g_unix_input_stream_new(pipe_fds[0], TRUE);
    GOutputStream *sink = g_memory_output_stream_new_resizable();
    ReceiveRequest *req = g_new(ReceiveRequest, 1);
    req->c = c;
    req->sel_id = sel_id;
    req->type = type;
    g_output_stream_splice_async(sink, src, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                 G_PRIORITY_DEFAULT, NULL, data_control_splice_ready_cb, req);
    g_object_unref(src);
    return;
err:
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                VD_AGENT_CLIPBOARD_NONE, NULL, 0);
#endif
}

static void
vdagent_clipboards_init(VDAgentClipboards *self)
{
}

VDAgentClipboards *vdagent_clipboards_new(struct vdagent_x11 *x11)
{
    VDAgentClipboards *self = g_object_new(VDAGENT_TYPE_CLIPBOARDS, NULL);

#ifndef USE_GTK_FOR_CLIPBOARD
    self->x11 = x11;
#else
    (void)x11;
    GdkDisplay *gdk_display = gdk_display_get_default();

    /* write side only -- see the file-level comment for why the observe
     * side doesn't use GdkClipboard at all. */
    self->selections[VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD].clipboard =
        gdk_display_get_clipboard(gdk_display);
    self->selections[VD_AGENT_CLIPBOARD_SELECTION_PRIMARY].clipboard =
        gdk_display_get_primary_clipboard(gdk_display);

    /* observe side: bind wlr-data-control directly (no GDK equivalent
     * exists -- it's a compositor-specific protocol extension, not part
     * of core Wayland/GTK). */
    self->wl_display = gdk_wayland_display_get_wl_display(gdk_display);
    self->wl_registry = wl_display_get_registry(self->wl_display);
    wl_registry_add_listener(self->wl_registry, &registry_listener, self);
    wl_display_roundtrip(self->wl_display); /* block until globals are bound */

    if (self->data_control_manager) {
        GdkSeat *gdk_seat = gdk_display_get_default_seat(gdk_display);
        struct wl_seat *wl_seat = gdk_wayland_seat_get_wl_seat(gdk_seat);
        self->data_control_device =
            zwlr_data_control_manager_v1_get_data_device(self->data_control_manager, wl_seat);
        zwlr_data_control_device_v1_add_listener(self->data_control_device, &device_listener, self);
    } else {
        syslog(LOG_WARNING, "%s: compositor has no zwlr_data_control_manager_v1; "
                            "guest clipboard changes will not be observed", __func__);
    }
#endif

    return self;
}

void
vdagent_clipboards_set_conn(VDAgentClipboards *self, UdscsConnection *conn)
{
#ifdef USE_GTK_FOR_CLIPBOARD
    self->conn = conn;
#endif
}

static void vdagent_clipboards_dispose(GObject *obj)
{
#ifdef USE_GTK_FOR_CLIPBOARD
    VDAgentClipboards *self = VDAGENT_CLIPBOARDS(obj);

    if (self->conn)
        vdagent_clipboards_release_all(self);
#endif
}

static void
vdagent_clipboards_class_init(VDAgentClipboardsClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);

    oclass->dispose = vdagent_clipboards_dispose;
}
