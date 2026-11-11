/* Probe: one WebKitWebView on a WPE Platform display and view of the
   embedder's own (as Stencil's host subclasses them), loading argv[1] at
   1280x800 and writing the first frame after the load finishes to argv[2]
   as a PPM, with the page's title on stdout. Exits 0 once written, 1 on a
   load failure or after the time limit (30 s).

   PROBE_SIZE=WxH sizes the view. PROBE_SCRIPT runs that JavaScript after
   the load and writes the first frame at least half a second after it
   finished instead (to see repaints, not only the first paint).
   PROBE_SOFTWARE turns the HardwareAcceleration feature off. PROBE_HOLD
   keeps the page up after writing.

   PROBE_MEASURE=N counts the page's animation frames over N seconds after
   one second of warm-up and prints them with the CPU time this process and
   its children (the web and network processes) spent meanwhile, instead of
   writing a frame.

   Built with PROBE_ZERO_COPY (against an engine that has them):
   - Darwin: frames that arrive as WPEBufferIOSurface are read from the
     surface itself.
   - Windows: PROBE_WINDOW=1 shows a window (a child window filling a
     top-level one, as Stencil's host has) and hands it to the view with
     wpe_view_set_native_window(), so the web process presents into it.
     Then, PROBE_PAUSE ms (3000) apart, writing each step's name to
     OUT.ppm.step: "shown"; "input" (PROBE_INPUT=1: real mouse input,
     SendInput, over the button and the page; the page window counts what
     reaches it and forwards it to the view); "resized" (PROBE_RESIZE=WxH); "scripted"
     (PROBE_SCRIPT); "reloaded" (PROBE_KILL=1: the web process is killed
     and the page reloaded); "cleared" (PROBE_CLEAR=1: the window is taken
     back and the next frame, which must arrive as a buffer again, is
     written to OUT.ppm); "suspended" (PROBE_SUSPEND=1: with the web
     process suspended, the windows are resized, moved, hidden, shown and
     destroyed, each timed); then "done". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>
#include <libproc.h>
#include <mach/mach_time.h>
#endif

static const char* output;
static GMainLoop* loop;
static WebKitWebView* webView;
static WPEView* probeView;
static gboolean loaded;
static gboolean armed;
static int status = 1;
static int width = 1280, height = 800;
static guint pause_ms = 3000;

/* The page's frames are written as BGRA, top row first. */
static void write_ppm(const unsigned char* data, gsize stride, int width, int height, const char* kind)
{
    FILE* file = fopen(output, "wb");
    if (!file) {
        perror(output);
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const unsigned char* row = data + (gsize)y * stride;
        for (int x = 0; x < width; x++) {
            unsigned char rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
    fprintf(stderr, "probe: wrote %dx%d (%s) to %s\n", width, height, kind, output);
    status = 0;
}

static void write_frame(WPEBuffer* buffer)
{
#if defined(PROBE_ZERO_COPY) && defined(__APPLE__)
    if (WPE_IS_BUFFER_IOSURFACE(buffer)) {
        /* What a host does with it: read (or show) the surface itself. */
        IOSurfaceRef surface = wpe_buffer_iosurface_get_surface(WPE_BUFFER_IOSURFACE(buffer));
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        write_ppm(IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface), (int)IOSurfaceGetWidth(surface), (int)IOSurfaceGetHeight(surface), G_OBJECT_TYPE_NAME(buffer));
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        return;
    }
#endif
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
    write_ppm(data, size / (gsize)height, width, height, G_OBJECT_TYPE_NAME(buffer));
}

/* A frame handed over, until the idle callback says it is on screen. */
typedef struct {
    WPEView* view;
    WPEBuffer* buffer;
} Shown;

static int buffer_frames;

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

/* Measuring, frames are on screen at the next refresh of a 60 Hz display. */
static GQueue refresh_queue = G_QUEUE_INIT;

static gboolean refresh(gpointer data)
{
    (void)data;
    Shown* shown;
    while ((shown = g_queue_pop_head(&refresh_queue)))
        frame_shown(shown);
    return G_SOURCE_CONTINUE;
}

/* The embedder's view: takes each buffer the web process rendered. */
#define PROBE_TYPE_VIEW (probe_view_get_type())
G_DECLARE_FINAL_TYPE(ProbeView, probe_view, PROBE, VIEW, WPEView)
struct _ProbeView {
    WPEView parent;
};
G_DEFINE_FINAL_TYPE(ProbeView, probe_view, WPE_TYPE_VIEW)

static void next_step(void);

static gboolean probe_view_render_buffer(WPEView* view, WPEBuffer* buffer, const WPERectangle* damage, guint n_damage, GError** error)
{
    (void)damage;
    (void)n_damage;
    (void)error;
    buffer_frames++;
    if (armed && status) {
        write_frame(buffer);
        if (!status && !g_getenv("PROBE_HOLD") && !g_getenv("PROBE_WINDOW"))
            g_main_loop_quit(loop);
    }
    Shown* shown = g_new(Shown, 1);
    shown->view = g_object_ref(view);
    shown->buffer = g_object_ref(buffer);
    if (g_getenv("PROBE_MEASURE"))
        g_queue_push_tail(&refresh_queue, shown);
    else
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

#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
/* The host's windows: a top-level one and the page's child filling it. */
static HWND topWindow, pageWindow;

static gboolean pump_messages(gpointer data);

/* Mouse messages the page window got (real input over the web process's
   presentation window must reach it), forwarded to the view as a host does. */
static int mouseMoves, mouseButtons, mouseWheels;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SIZE && window == topWindow && pageWindow)
        MoveWindow(pageWindow, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
    if (window == pageWindow && probeView) {
        double x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
        WPEEvent* event = NULL;
        switch (message) {
        case WM_MOUSEMOVE:
            mouseMoves++;
            event = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, probeView, WPE_INPUT_SOURCE_MOUSE, GetMessageTime(), 0, x, y, 0, 0);
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            mouseButtons++;
            event = wpe_event_pointer_button_new(message == WM_LBUTTONDOWN ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, probeView, WPE_INPUT_SOURCE_MOUSE, GetMessageTime(), message == WM_LBUTTONDOWN ? 0 : WPE_MODIFIER_POINTER_BUTTON1, 1, x, y, message == WM_LBUTTONDOWN ? 1 : 0);
            break;
        case WM_MOUSEWHEEL: {
            mouseWheels++;
            POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ScreenToClient(pageWindow, &point);
            event = wpe_event_scroll_new(probeView, WPE_INPUT_SOURCE_MOUSE, GetMessageTime(), 0, 0, GET_WHEEL_DELTA_WPARAM(wParam) / 120.0, FALSE, FALSE, point.x, point.y);
            break;
        }
        }
        if (event) {
            wpe_view_event(probeView, event);
            wpe_event_unref(event);
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/* Real input at a point of the page window, as a user's. */
static void send_input_at(int x, int y, DWORD flags, DWORD data)
{
    POINT point = { x, y };
    ClientToScreen(pageWindow, &point);
    SetCursorPos(point.x, point.y);
    if (!flags)
        return;
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    SendInput(1, &input, sizeof input);
}

static gboolean input_step(gpointer data)
{
    int stage = GPOINTER_TO_INT(data);
    switch (stage) {
    case 0:
        SetForegroundWindow(topWindow);
        send_input_at(120, 210, 0, 0);
        break;
    case 1:
        send_input_at(125, 220, 0, 0);
        break;
    case 2:
        send_input_at(125, 220, MOUSEEVENTF_LEFTDOWN, 0);
        break;
    case 3:
        send_input_at(125, 220, MOUSEEVENTF_LEFTUP, 0);
        break;
    case 4:
        send_input_at(400, 400, MOUSEEVENTF_WHEEL, (DWORD)-120);
        break;
    default:
        fprintf(stderr, "probe: page window got %d moves, %d button messages, %d wheel messages\n", mouseMoves, mouseButtons, mouseWheels);
        return G_SOURCE_REMOVE;
    }
    g_timeout_add(150, input_step, GINT_TO_POINTER(stage + 1));
    return G_SOURCE_REMOVE;
}

typedef LONG (NTAPI *SuspendFunction)(HANDLE);

/* The children of this process named `name`, suspended or resumed. */
static void suspend_children(const char* name, gboolean suspend)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    SuspendFunction function = (SuspendFunction)GetProcAddress(ntdll, suspend ? "NtSuspendProcess" : "NtResumeProcess");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry = { sizeof entry };
    for (BOOL more = Process32First(snapshot, &entry); more; more = Process32Next(snapshot, &entry)) {
        if (entry.th32ParentProcessID != GetCurrentProcessId() || _stricmp(entry.szExeFile, name))
            continue;
        HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, entry.th32ProcessID);
        if (process) {
            fprintf(stderr, "probe: %s %s %lu: %ld\n", suspend ? "suspended" : "resumed", name, entry.th32ProcessID, function(process));
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
}

static double milliseconds_since(LARGE_INTEGER start)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

#define TIMED(label, call) do { LARGE_INTEGER start; QueryPerformanceCounter(&start); call; fprintf(stderr, "probe: %s took %.2f ms\n", label, milliseconds_since(start)); } while (0)

/* What an embedder does to its window while the web process is stuck: none
   of it may wait for that process. */
static void suspended_window_operations(void)
{
    suspend_children("WPEWebProcess.exe", TRUE);
    RECT rect;
    GetWindowRect(topWindow, &rect);
    TIMED("SetWindowPos (resize) of the top-level window", SetWindowPos(topWindow, NULL, 0, 0, rect.right - rect.left + 40, rect.bottom - rect.top + 30, SWP_NOMOVE | SWP_NOZORDER));
    TIMED("SetWindowPos (move) of the page window", SetWindowPos(pageWindow, NULL, 10, 10, 0, 0, SWP_NOSIZE | SWP_NOZORDER));
    TIMED("ShowWindow(SW_HIDE) of the page window", ShowWindow(pageWindow, SW_HIDE));
    TIMED("ShowWindow(SW_SHOW) of the page window", ShowWindow(pageWindow, SW_SHOW));
    TIMED("ShowWindow(SW_HIDE) of the top-level window", ShowWindow(topWindow, SW_HIDE));
    TIMED("ShowWindow(SW_SHOW) of the top-level window", ShowWindow(topWindow, SW_SHOW));
    pump_messages(NULL);
    TIMED("DestroyWindow of the top-level window", DestroyWindow(topWindow));
    topWindow = pageWindow = NULL;
    suspend_children("WPEWebProcess.exe", FALSE);
}

static gboolean pump_messages(gpointer data)
{
    (void)data;
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return G_SOURCE_CONTINUE;
}

static void create_windows(void)
{
    WNDCLASSW top = { 0 };
    top.lpfnWndProc = window_proc;
    top.hInstance = GetModuleHandleW(NULL);
    top.lpszClassName = L"ProbeTop";
    top.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    top.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&top);
    WNDCLASSW page = top;
    page.lpszClassName = L"ProbePage";
    page.hbrBackground = NULL;
    RegisterClassW(&page);
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    topWindow = CreateWindowW(L"ProbeTop", L"WPE probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, top.hInstance, NULL);
    pageWindow = CreateWindowW(L"ProbePage", NULL, WS_CHILD | WS_VISIBLE, 0, 0, width, height, topWindow, NULL, top.hInstance, NULL);
    g_timeout_add(5, pump_messages, NULL);
}
#endif

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
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
    /* Before the page exists, as a host may. */
    if (pageWindow)
        wpe_view_set_native_window(view, pageWindow);
#endif
    probeView = view;
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

/* CPU time of this process and of its children: milliseconds, but on
   Windows millions of CPU cycles (its process times are counted in clock
   ticks, too coarse on the test VM). */
static void cpu_times(double* self, double* children)
{
    *self = 0;
    *children = 0;
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(GetCurrentProcess(), &cycles))
        *self = cycles / 1e6;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry = { sizeof entry };
    for (BOOL more = Process32First(snapshot, &entry); more; more = Process32Next(snapshot, &entry)) {
        if (entry.th32ParentProcessID != GetCurrentProcessId())
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process && QueryProcessCycleTime(process, &cycles))
            *children += cycles / 1e6;
        if (process)
            CloseHandle(process);
    }
    CloseHandle(snapshot);
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    *self = usage.ru_utime.tv_sec * 1e3 + usage.ru_utime.tv_usec / 1e3 + usage.ru_stime.tv_sec * 1e3 + usage.ru_stime.tv_usec / 1e3;
#ifdef __APPLE__
    pid_t pids[16];
    int count = proc_listchildpids(getpid(), pids, sizeof pids) ;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    for (int i = 0; i < count && i < 16; i++) {
        struct rusage_info_v2 info;
        if (!proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t*)&info))
            *children += (double)(info.ri_user_time + info.ri_system_time) * timebase.numer / timebase.denom / 1e6;
    }
#endif
#endif
}

static double measure_self, measure_children;
static gint64 measure_start;

static void measured(GObject* object, GAsyncResult* result, gpointer data)
{
    (void)data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    double self, children;
    cpu_times(&self, &children);
    double seconds = (g_get_monotonic_time() - measure_start) / 1e6;
    if (!value) {
        fprintf(stderr, "probe: measuring failed: %s\n", error->message);
        g_clear_error(&error);
    } else {
        int frames = jsc_value_to_int32(value);
#ifdef _WIN32
        const char* unit = "cpu Mcycles";
#else
        const char* unit = "cpu ms";
#endif
        printf("measure: %.1f s, %d animation frames (%.1f fps), %d buffers to the view, %s: probe %.0f, children %.0f, total %.0f\n",
            seconds, frames, frames / seconds, buffer_frames, unit, self - measure_self, children - measure_children, self - measure_self + children - measure_children);
        fflush(stdout);
        g_object_unref(value);
        status = 0;
    }
    g_main_loop_quit(loop);
}

static gboolean measure_end(gpointer data)
{
    (void)data;
    webkit_web_view_evaluate_javascript(webView, "window.__probeFrames", -1, NULL, NULL, NULL, measured, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean measure_begin(gpointer data)
{
    (void)data;
    webkit_web_view_evaluate_javascript(webView, "window.__probeFrames = 0; (function count() { window.__probeFrames++; requestAnimationFrame(count); })(); 1", -1, NULL, NULL, NULL, NULL, NULL);
    cpu_times(&measure_self, &measure_children);
    measure_start = g_get_monotonic_time();
    buffer_frames = 0;
    g_timeout_add_seconds(atoi(g_getenv("PROBE_MEASURE")), measure_end, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean arm(gpointer data)
{
    (void)data;
    armed = TRUE;
    return G_SOURCE_REMOVE;
}

static void script_done(GObject* object, GAsyncResult* result, gpointer data)
{
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
    if (!data)
        g_timeout_add(500, arm, NULL);
}

/* The window mode's steps, PROBE_PAUSE apart. */
enum Step { STEP_LOADING, STEP_SHOWN, STEP_INPUT, STEP_RESIZED, STEP_SCRIPTED, STEP_RELOADED, STEP_CLEARED, STEP_SUSPENDED, STEP_DONE };
static enum Step step = STEP_LOADING;
static gboolean reloading;

static void report_step(const char* name)
{
    char* path = g_strconcat(output, ".step", NULL);
    g_file_set_contents(path, name, -1, NULL);
    g_free(path);
    fprintf(stderr, "probe: step %s\n", name);
}

static gboolean next_step_timeout(gpointer data)
{
    (void)data;
    next_step();
    return G_SOURCE_REMOVE;
}

static void next_step(void)
{
    switch (step) {
    case STEP_LOADING:
        step = STEP_SHOWN;
        fprintf(stderr, "probe: %d buffers while the window was set\n", buffer_frames);
        report_step("shown");
        break;
    case STEP_SHOWN:
        step = STEP_INPUT;
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
        if (g_getenv("PROBE_INPUT")) {
            input_step(GINT_TO_POINTER(0));
            report_step("input");
            break;
        }
#endif
        next_step();
        return;
    case STEP_INPUT:
        step = STEP_RESIZED;
        if (g_getenv("PROBE_RESIZE")) {
            int w, h;
            if (sscanf(g_getenv("PROBE_RESIZE"), "%dx%d", &w, &h) == 2) {
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
                RECT rect = { 0, 0, w, h };
                AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(topWindow, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
#endif
                wpe_toplevel_resized(wpe_view_get_toplevel(probeView), w, h);
                wpe_view_resized(probeView, w, h);
                report_step("resized");
                break;
            }
        }
        next_step();
        return;
    case STEP_RESIZED:
        step = STEP_SCRIPTED;
        if (g_getenv("PROBE_SCRIPT")) {
            webkit_web_view_evaluate_javascript(webView, g_getenv("PROBE_SCRIPT"), -1, NULL, NULL, NULL, script_done, GINT_TO_POINTER(1));
            report_step("scripted");
            break;
        }
        next_step();
        return;
    case STEP_SCRIPTED:
        step = STEP_RELOADED;
        if (g_getenv("PROBE_KILL")) {
            reloading = TRUE;
            webkit_web_view_terminate_web_process(webView);
            return; /* Continues from the reload's load-changed. */
        }
        next_step();
        return;
    case STEP_RELOADED:
        step = STEP_CLEARED;
        if (g_getenv("PROBE_CLEAR")) {
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
            buffer_frames = 0;
            armed = TRUE;
            status = 1;
            wpe_view_set_native_window(probeView, NULL);
#endif
            report_step("cleared");
            break;
        }
        next_step();
        return;
    case STEP_CLEARED:
        step = STEP_SUSPENDED;
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
        if (g_getenv("PROBE_SUSPEND")) {
            report_step("suspended");
            suspended_window_operations();
            break;
        }
#endif
        next_step();
        return;
    case STEP_SUSPENDED:
        step = STEP_DONE;
        report_step("done");
        fprintf(stderr, "probe: %d buffers since the window was taken back\n", buffer_frames);
        if (!g_getenv("PROBE_CLEAR"))
            status = 0;
        g_main_loop_quit(loop);
        return;
    case STEP_DONE:
        return;
    }
    g_timeout_add(pause_ms, next_step_timeout, NULL);
}

static void load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    fprintf(stderr, "probe: load event %d %s\n", (int)event, webkit_web_view_get_uri(view));
    if (event != WEBKIT_LOAD_FINISHED)
        return;
    printf("title: %s\n", webkit_web_view_get_title(view));
    fflush(stdout);
    if (reloading) {
        reloading = FALSE;
        report_step("reloaded");
        g_timeout_add(pause_ms, next_step_timeout, NULL);
        return;
    }
    loaded = TRUE;
    if (g_getenv("PROBE_MEASURE")) {
        g_timeout_add_seconds(1, measure_begin, NULL);
        return;
    }
    if (g_getenv("PROBE_WINDOW")) {
        /* Frames that still come as buffers (no window could be presented
           into) are written too. */
        armed = TRUE;
        g_timeout_add(pause_ms / 3, next_step_timeout, NULL);
        return;
    }
    const char* script = g_getenv("PROBE_SCRIPT");
    if (script)
        webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, script_done, NULL);
    else
        armed = TRUE;
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
    (void)data;
    fprintf(stderr, "probe: web process terminated (reason %d)\n", (int)reason);
    if (reloading) {
        webkit_web_view_reload(view);
        return;
    }
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
    if (g_getenv("PROBE_PAUSE"))
        pause_ms = (guint)atoi(g_getenv("PROBE_PAUSE"));
    if (g_getenv("PROBE_WINDOW")) {
#if defined(PROBE_ZERO_COPY) && defined(_WIN32)
        create_windows();
#else
        fprintf(stderr, "probe: PROBE_WINDOW needs a Windows build with PROBE_ZERO_COPY\n");
        return 2;
#endif
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
    webView = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings, NULL);
    g_signal_connect(webView, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(webView, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(process_terminated), NULL);
    webkit_web_view_load_uri(webView, argv[1]);
    loop = g_main_loop_new(NULL, FALSE);
    if (g_getenv("PROBE_MEASURE"))
        g_timeout_add(16, refresh, NULL);
    g_timeout_add_seconds(g_getenv("PROBE_WINDOW") || g_getenv("PROBE_MEASURE") ? 120 : 30, timed_out, NULL);
    g_main_loop_run(loop);
    return status;
}
