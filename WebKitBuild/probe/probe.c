/* Offscreen probe: one WebKitWebView on a WPE Platform display and view of
   the embedder's own (as Stencil's host subclasses them), loading argv[1]
   at 1280x800 and writing the first frame after the load finishes to
   argv[2] as a PPM, with the page's title on stdout. Exits 0 once written,
   1 on a load failure or after 30 s.
   PROBE_SIZE=WxH sizes the view; PROBE_SCRIPT runs that JavaScript after
   the load and writes the first frame at least half a second after it
   finished instead (to see repaints, not only the first paint). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

static const char* output;
static GMainLoop* loop;
static gboolean loaded;
static gboolean armed;
static int status = 1;
static int width = 1280, height = 800;

/* The embedder's view: takes each buffer the web process rendered. */
#define PROBE_TYPE_VIEW (probe_view_get_type())
G_DECLARE_FINAL_TYPE(ProbeView, probe_view, PROBE, VIEW, WPEView)
struct _ProbeView {
    WPEView parent;
};
G_DEFINE_FINAL_TYPE(ProbeView, probe_view, WPE_TYPE_VIEW)

static void write_frame(WPEBuffer* buffer)
{
    GError* error = NULL;
    GBytes* pixels = wpe_buffer_import_to_pixels(buffer, &error);
    if (!pixels) {
        fprintf(stderr, "probe: no pixels: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }
    int width = wpe_buffer_get_width(buffer);
    int height = wpe_buffer_get_height(buffer);
    gsize size = 0;
    const unsigned char* data = g_bytes_get_data(pixels, &size);
    gsize stride = size / (gsize)height;
    FILE* file = fopen(output, "wb");
    if (!file) {
        perror(output);
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const unsigned char* row = data + (gsize)y * stride;
        for (int x = 0; x < width; x++) {
            /* BGRA in memory. */
            unsigned char rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
    fprintf(stderr, "probe: wrote %dx%d (%s) to %s\n", width, height, G_OBJECT_TYPE_NAME(buffer), output);
    status = 0;
}

/* A frame handed over, until the idle callback says it is on screen. */
typedef struct {
    WPEView* view;
    WPEBuffer* buffer;
} Shown;

static gboolean frame_shown(gpointer data)
{
    Shown* shown = data;
    wpe_view_buffer_rendered(shown->view, shown->buffer);
    wpe_view_buffer_released(shown->view, shown->buffer);
    g_object_unref(shown->buffer);
    g_object_unref(shown->view);
    g_free(shown);
    return G_SOURCE_REMOVE;
}

static gboolean probe_view_render_buffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damage, guint n_damage, GError** error)
{
    (void)damage;
    (void)n_damage;
    (void)error;
    static int frames;
    frames++;
    if (armed && status) {
        write_frame(buffer);
        /* PROBE_HOLD keeps the page up (to watch its process die). */
        if (!status && !g_getenv("PROBE_HOLD"))
            g_main_loop_quit(loop);
    }
    Shown* shown = g_new(Shown, 1);
    shown->view = g_object_ref(view);
    shown->buffer = g_object_ref(buffer);
    g_idle_add(frame_shown, shown);
    return TRUE;
}

static void probe_view_class_init(ProbeViewClass* klass)
{
    WPE_VIEW_CLASS(klass)->render_buffer = probe_view_render_buffer;
}

static void probe_view_init(ProbeView* view)
{
    (void)view;
}

/* The embedder's display: makes the views above, each in a headless
   toplevel (which only keeps size, scale and state). */
#define PROBE_TYPE_DISPLAY (probe_display_get_type())
G_DECLARE_FINAL_TYPE(ProbeDisplay, probe_display, PROBE, DISPLAY, WPEDisplay)
struct _ProbeDisplay {
    WPEDisplay parent;
};
G_DEFINE_FINAL_TYPE(ProbeDisplay, probe_display, WPE_TYPE_DISPLAY)

static gboolean probe_display_connect(WPEDisplay* display, GError** error)
{
    (void)display;
    (void)error;
    return TRUE;
}

static WPEView* probe_display_create_view(WPEDisplay* display)
{
    WPEView* view = g_object_new(PROBE_TYPE_VIEW, "display", display, NULL);
    WPEToplevel* toplevel = g_object_new(WPE_TYPE_TOPLEVEL_HEADLESS, "display", display, NULL);
    wpe_view_set_toplevel(view, toplevel);
    g_object_unref(toplevel);
    wpe_toplevel_resized(toplevel, width, height);
    wpe_view_resized(view, width, height);
    wpe_view_map(view);
    return view;
}

static void probe_display_class_init(ProbeDisplayClass* klass)
{
    WPE_DISPLAY_CLASS(klass)->connect = probe_display_connect;
    WPE_DISPLAY_CLASS(klass)->create_view = probe_display_create_view;
}

static void probe_display_init(ProbeDisplay* display)
{
    (void)display;
}

static gboolean arm(gpointer data)
{
    (void)data;
    armed = TRUE;
    return G_SOURCE_REMOVE;
}

static void script_done(GObject* object, GAsyncResult* result, gpointer data)
{
    (void)data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        fprintf(stderr, "probe: script failed: %s\n", error->message);
        g_clear_error(&error);
    } else {
        char* text = jsc_value_to_string(value);
        fprintf(stderr, "probe: script returned %s\n", text);
        g_free(text);
        g_object_unref(value);
    }
    g_timeout_add(500, arm, NULL);
}

static void load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    fprintf(stderr, "probe: load event %d %s\n", (int)event, webkit_web_view_get_uri(view));
    if (event == WEBKIT_LOAD_FINISHED) {
        printf("title: %s\n", webkit_web_view_get_title(view));
        fflush(stdout);
        loaded = TRUE;
        const char* script = g_getenv("PROBE_SCRIPT");
        if (script)
            webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, script_done, NULL);
        else
            armed = TRUE;
    }
}

static gboolean load_failed(WebKitWebView* view, WebKitLoadEvent event, const char* uri, GError* error, gpointer data)
{
    (void)view;
    (void)event;
    (void)data;
    fprintf(stderr, "probe: load of %s failed: %s\n", uri, error->message);
    g_main_loop_quit(loop);
    return FALSE;
}

static void process_terminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)view;
    (void)data;
    fprintf(stderr, "probe: web process terminated (reason %d)\n", (int)reason);
    g_main_loop_quit(loop);
}

static gboolean timed_out(gpointer data)
{
    (void)data;
    fprintf(stderr, "probe: timed out (loaded: %d)\n", loaded);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: probe URL OUT.ppm\n");
        return 2;
    }
    output = argv[2];
    const char* size = g_getenv("PROBE_SIZE");
    if (size && sscanf(size, "%dx%d", &width, &height) != 2) {
        fprintf(stderr, "probe: PROBE_SIZE is WxH, not %s\n", size);
        return 2;
    }
    GError* error = NULL;
    WPEDisplay* display = g_object_new(PROBE_TYPE_DISPLAY, NULL);
    if (!wpe_display_connect(display, &error)) {
        fprintf(stderr, "probe: display: %s\n", error->message);
        return 1;
    }
    /* PROBE_SOFTWARE draws the page without the GPU (to tell the engine's
       faults from a graphics stack's): the page picks how it draws when it
       is created. */
    WebKitSettings* settings = webkit_settings_new();
    if (g_getenv("PROBE_SOFTWARE")) {
        WebKitFeatureList* features = webkit_settings_get_all_features();
        for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
            WebKitFeature* feature = webkit_feature_list_get(features, i);
            if (!g_strcmp0(webkit_feature_get_identifier(feature), "HardwareAcceleration")) {
                webkit_settings_set_feature_enabled(settings, feature, FALSE);
                fprintf(stderr, "probe: hardware acceleration off\n");
            }
        }
        webkit_feature_list_unref(features);
    }
    WebKitWebView* view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings, NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(process_terminated), NULL);
    webkit_web_view_load_uri(view, argv[1]);
    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(30, timed_out, NULL);
    g_main_loop_run(loop);
    return status;
}
