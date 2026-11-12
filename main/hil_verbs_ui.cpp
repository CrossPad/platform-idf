/* CDC verbs: the screen, touch, encoder, apps and pad LEDs. Dispatch order is in hil_control.cpp. */
#include "hil_verbs.h"

static const char *TAG = "hil_control";

/* ── App control (must run on the LVGL task) ───────────────────────── */

// Single in-flight launch request — lv_async_call runs before the next CDC
// command can arrive.
EXT_RAM_BSS_ATTR static char s_pending_app[32];

static crosspad_gui::ILvglApp *find_app(const char *name)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    for (const auto &app : orch.getApps()) {
        if (app && crosspad::control::appNameMatches(app->getName(), name)) {
            return app.get();
        }
    }
    return nullptr;
}

static void async_app_start(void *)
{
    auto *app = find_app(s_pending_app);
    if (!app) {
        ESP_LOGW(TAG, "APP_START: '%s' not found", s_pending_app);
        return;
    }
    ESP_LOGI(TAG, "APP_START: launching '%s'", s_pending_app);
    crosspad_gui::AppOrchestrator::onAppSelected(app, app_c);
}

/* APP_STOP rebuilds the launcher, which exists only once LoadMainScreen() has
 * run in a normal boot (isGuiInitialised is set after it; transfer and network
 * mode set it too but have no launcher). Earlier -- the splash, before
 * crosspad_gui::bootstrap() -- LoadMainScreen() asserts in getGuiPlatform(). */
static bool launcher_up(void)
{
    return status.isGuiInitialised && !usb_transfer_active() && !net_mode_active();
}

static void async_app_stop(void *)
{
    if (!launcher_up()) {
        ESP_LOGW(TAG, "APP_STOP: no launcher in this boot (yet), ignored");
        return;
    }
    ESP_LOGI(TAG, "APP_STOP: back to launcher");
    LoadMainScreen(NULL);
}

// The app-initiated exit, as an in-app back button takes it. Unlike APP_STOP
// it does not rebuild the main screen, so teardown leftovers (encoder
// ownership above all) stay observable.
static void async_app_destroy(void *)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    if (running && running->isStarted()) {
        running->destroyApp();
        orch.setRunningApp(nullptr);
    }
}

// The framework's own self-close, with no harness bookkeeping: whatever is
// left behind is the app's or the framework's doing.
static void async_app_self_close(void *)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    if (running && running->isStarted()) {
        running->destroyAppAsync();
    }
}

static void handle_app_list(void)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    char out[256];
    size_t n = snprintf(out, sizeof(out), "APPS: ");
    bool first = true;
    for (const auto &app : orch.getApps()) {
        if (!app) continue;
        n += snprintf(out + n, sizeof(out) - n, "%s%s",
                      first ? "" : ",", app->getName());
        first = false;
        if (n >= sizeof(out) - 48) break;
    }
    auto *running = orch.getRunningApp();
    snprintf(out + n, sizeof(out) - n, " running=%s\r\n",
             running ? running->getName() : "-");
    cdc_reply(out);
}

static void handle_touch_trace(const char *arg)
{
    char reply[96];
    /* Every subcommand answers in the same shape — sample rows (DUMP only)
     * followed by the trailer. A verb that replied "OK" to one argument and a
     * multi-line block to another needed the reader to know which before it
     * could read either. */
    bool dump = false;
    if (strncmp(arg, "ON", 2) == 0 || strncmp(arg, "OFF", 3) == 0) {
        if (!touch_trace_set(arg[1] == 'N')) { cdc_reply("ERR TOUCH_TRACE no_mem\r\n"); return; }
    } else if (strncmp(arg, "CLEAR", 5) == 0) {
        touch_trace_clear();
    } else if (strncmp(arg, "DUMP", 4) == 0) {
        dump = true;
    } else {
        cdc_reply("ERR TOUCH_TRACE ON|OFF|CLEAR|DUMP\r\n");
        return;
    }

    const unsigned n = touch_trace_count();
    for (unsigned i = 0; dump && i < n; i++) {
        unsigned t = 0, pressed = 0;
        int x = 0, y = 0;
        if (!touch_trace_get(i, &t, &x, &y, &pressed)) break;
        snprintf(reply, sizeof(reply), "TOUCH_TRACE: %u %u %d %d\r\n", t, pressed, x, y);
        cdc_reply(reply);
    }
    snprintf(reply, sizeof(reply),
             "TOUCH_TRACE: end n=%u on=%d swipes=%u last=%s fwd=%s\r\n",
             n, touch_trace_is_on() ? 1 : 0,
             (unsigned)crosspad_gui::touch_gesture_count(),
             crosspad_gui::swipeDirName(crosspad_gui::touch_gesture_last()),
             crosspad_gui::touch_gesture_forward_result());
    cdc_reply(reply);
}

/* ── Encoder navigation introspection ──────────────────────────────── */

// Who owns encoder navigation. An app that made its own lv_group must hand
// the encoder back on teardown, or the launcher is unnavigable until reboot —
// and nothing else can observe that from outside.

/* A finger the bench does not have: TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms]
 * [hold_ms] presses, holds (a long press, then a drag), moves, rests and lets
 * go through the panel's own read path, so the
 * swipe recogniser, LVGL's scrolling and the trace all see an ordinary contact.
 * TOUCH_TAP x y [hold_ms] is a drag that does not move. The reply comes at
 * once; the contact takes ms + rest_ms after it. */
static void handle_touch_inject(const char *arg, bool tap)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, ms = 0, rest = 0, hold = 0;
    int n;
    if (tap) {
        n = sscanf(arg, "%d %d %d", &x1, &y1, &ms);
        if (n < 2) { cdc_reply("ERR TOUCH_TAP x y [hold_ms]\r\n"); return; }
        if (n < 3) ms = 80;
        x2 = x1; y2 = y1;
    } else {
        n = sscanf(arg, "%d %d %d %d %d %d %d", &x1, &y1, &x2, &y2, &ms, &rest, &hold);
        if (n < 5) { cdc_reply("ERR TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms] [hold_ms]\r\n"); return; }
    }
    if (ms < 1 || ms > 5000 || rest < 0 || rest > 5000 || hold < 0 || hold > 5000) {
        cdc_reply("ERR TOUCH range ms 1..5000 rest 0..5000 hold 0..5000\r\n");
        return;
    }
    if (!touch_trace_inject(x1, y1, x2, y2, (unsigned)ms, (unsigned)rest, (unsigned)hold)) {
        cdc_reply(touch_trace_injecting() ? "ERR TOUCH busy\r\n" : "ERR TOUCH no_panel\r\n");
        return;
    }
    cdc_reply("OK\r\n");
}

/* UI_TREE -- every visible widget of the screen and the top layer
 * (crosspad-gui ui_tree_probe), one "UITREE:" row each, pre-order: index,
 * parent, class, LCD rectangle, interactive, hit (a press on its middle
 * reaches it, not something drawn over it), checked/disabled/focused, then the
 * text (its own label, else the first one under it) to the end of the line.
 * Then "UITREE: end n=.. running=..". What a test needs to find a control by
 * what it says and tap it where it is (TOUCH_TAP). Copied under the LVGL lock,
 * written after it: the CDC writes must not hold the UI. */
static void handle_ui_tree(void)
{
    constexpr uint16_t kMax = 512;
    constexpr size_t kText = 96;
    struct Row { crosspad_gui::UiNode n; char cls[16]; char text[kText]; bool hit; };
    /* PSRAM for the call only: a screen with an app and a dialog over it is
     * a few hundred widgets, which is no reason to hold 60 kB for good. */
    auto *nodes = static_cast<crosspad_gui::UiNode *>(
        heap_caps_malloc(sizeof(crosspad_gui::UiNode) * kMax, MALLOC_CAP_SPIRAM));
    auto *rows = static_cast<Row *>(heap_caps_malloc(sizeof(Row) * kMax, MALLOC_CAP_SPIRAM));
    if (!nodes || !rows) {
        heap_caps_free(nodes);
        heap_caps_free(rows);
        cdc_reply("ERR UI_TREE no_mem\r\n");
        return;
    }
    EXT_RAM_BSS_ATTR static char running[32];
    if (!lvgl_port_lock(1000)) {
        heap_caps_free(nodes);
        heap_caps_free(rows);
        cdc_reply("ERR UI_TREE lvgl_busy\r\n");
        return;
    }
    const uint16_t count = crosspad_gui::ui_tree_probe(nodes, kMax);
    for (uint16_t i = 0; i < count; ++i) {
        Row &r = rows[i];
        r.n = nodes[i];
        snprintf(r.cls, sizeof(r.cls), "%s", nodes[i].cls ? nodes[i].cls : "obj");
        snprintf(r.text, sizeof(r.text), "%s", nodes[i].text ? nodes[i].text : "");
        for (char *c = r.text; *c; ++c) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        /* What a finger on its middle would land on: the probe lists what is
         * drawn, also under an app's screen or a dialog; only a control the
         * press reaches (it or something inside it) is one a person can use. */
        r.hit = false;
        if (r.n.interactive) {
            lv_point_t pt = { (int32_t)(r.n.x + r.n.w / 2), (int32_t)(r.n.y + r.n.h / 2) };
            lv_obj_t *on = lv_indev_search_obj(lv_layer_top(), &pt);
            if (!on) on = lv_indev_search_obj(lv_screen_active(), &pt);
            /* It, or something inside it: an ancestor of what is hit with the
             * node's own rectangle. Inside its rectangle is not enough -- a
             * pad of the app over the map lies inside the map's button. */
            for (lv_obj_t *o = on; o && !r.hit; o = lv_obj_get_parent(o)) {
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                r.hit = a.x1 == r.n.x && a.y1 == r.n.y &&
                        lv_area_get_width(&a) == r.n.w && lv_area_get_height(&a) == r.n.h;
            }
        }
    }
    auto *app = crosspad_gui::AppOrchestrator::getInstance().getRunningApp();
    snprintf(running, sizeof(running), "%s", app && app->getName() ? app->getName() : "-");
    lvgl_port_unlock();
    heap_caps_free(nodes);

    EXT_RAM_BSS_ATTR static char line[208];
    for (uint16_t i = 0; i < count; ++i) {
        const Row &r = rows[i];
        snprintf(line, sizeof(line),
                 "UITREE: i=%u p=%d cls=%s x=%ld y=%ld w=%ld h=%ld int=%d hit=%d chk=%d dis=%d foc=%d text=%s\r\n",
                 (unsigned)i, (int)r.n.parent, r.cls, (long)r.n.x, (long)r.n.y, (long)r.n.w, (long)r.n.h,
                 r.n.interactive ? 1 : 0, r.hit ? 1 : 0, r.n.checked ? 1 : 0, r.n.disabled ? 1 : 0,
                 r.n.focused ? 1 : 0, r.text);
        cdc_reply(line);
    }
    heap_caps_free(rows);
    snprintf(line, sizeof(line), "UITREE: end n=%u max=%u running=%s\r\n", (unsigned)count, (unsigned)kMax, running);
    cdc_reply(line);
}

/* The top row and what the swipes did: which bar is the page, where the row
 * rests (0 transport, 320 status), the drawer, where the encoder is, the last
 * screen-wide swipe, the MODE map and who owns the pads. */
static void handle_topbar()
{
    char reply[200];
    if (!lvgl_port_lock(1000)) { cdc_reply("ERR TOPBAR lvgl_busy\r\n"); return; }
    lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
    lv_obj_t *focused = g ? lv_group_get_focused(g) : nullptr;
    const char *focus = !focused ? "none"
                      : focused == crosspad_gui::statusbar_get_arrow() ? "arrow"
                      : crosspad_gui::transportbar_has_focus() ? "transport"
                      : "screen";
    snprintf(reply, sizeof(reply),
             "TOPBAR: transport=%d scroll=%ld drawer=%d focus=%s swipe=%s swipes=%u map=%d pads=%s\r\n",
             crosspad_gui::transportbar_shown() ? 1 : 0,
             (long)crosspad_gui::transportbar_row_scroll(),
             crosspad_gui::statusbar_drawer_is_open() ? 1 : 0,
             focus,
             crosspad_gui::swipeDirName(crosspad_gui::touch_gesture_last()),
             (unsigned)crosspad_gui::touch_gesture_count(),
             crosspad_gui::mode_overlay_is_open() ? 1 : 0,
             crosspad::getPadManager().getActivePadLogic().c_str());
    lvgl_port_unlock();
    cdc_reply(reply);
}

static void handle_enc_state(void)
{
    lv_group_t *cur = nullptr;
    if (lvgl_port_lock(1000)) {
        cur = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
        lvgl_port_unlock();
    }
    char reply[128];
    snprintf(reply, sizeof(reply),
             "ENC: group=%p launcher=%p owner=%s\r\n",
             (void *)cur, (void *)app_group,
             cur == nullptr      ? "none"
             : cur == app_group  ? "launcher"
                                 : "app");
    cdc_reply(reply);
}

// Encoder stimulation — the only way to reach an in-app Back button on a unit
// without touch.
static int s_enc_delta = 0;

static uint32_t s_enc_press_ms = 80;

static void async_enc_rotate(void *)
{
    // Through the encoder read callback, the physical encoder's own path.
    // lv_group_send_data() only sends LV_EVENT_KEY: focus never moves, and
    // widgets that gate on lv_indev_active() see no encoder at all.
    encoderTop_hil_diff().fetch_add(s_enc_delta);
}

static void async_enc_press(void *)
{
    // Hold for at least one indev read period, so LVGL forms the click (or,
    // held longer, the long press) itself.
    encoderTop_hil_press_until().store(lv_tick_get() + s_enc_press_ms);
}

// Why the pads are dark: painted black, brightness zero, an animation holding
// the strip, or no power-state callback ever arrived to repaint.
static void handle_led_state(void)
{
    uint32_t refreshes = 0;
    const uint32_t txfail = esp_led_strip_refresh_failures(&refreshes);
    const HilLedBoard board{g_hil_last_pwr_state.load(), g_hil_pwr_state_count.load(), txfail, refreshes};
    /* Static: 512 B on the drain loop's 4 kB stack, under snprintf, overflowed
     * it at the mirror window's four asks a second (task=main cause=65). */
    EXT_RAM_BSS_ATTR static char out[512];
    hil_text_led_state(out, sizeof(out), crosspad::control::readLedState(crosspad::getPadLedController()), board);
    cdc_reply(out);
}
/* Whole-group readback: what is on this screen at all. Rotating blindly and
 * counting pointers cannot tell a row that was never built from one the
 * encoder has not reached. Captions live in different places per widget, so
 * the lookup walks the subtree a few levels deep, then the parent. */
static const char *first_label_in(lv_obj_t *scope, int depth)
{
    if (!scope) return nullptr;
    if (lv_obj_check_type(scope, &lv_label_class)) return lv_label_get_text(scope);
    if (depth == 0) return nullptr;
    uint32_t cnt = lv_obj_get_child_count(scope);
    for (uint32_t i = 0; i < cnt; ++i) {
        const char *t = first_label_in(lv_obj_get_child(scope, i), depth - 1);
        if (t) return t;
    }
    return nullptr;
}

static const char *widget_label(lv_obj_t *obj)
{
    if (!obj) return "";
    const char *t = first_label_in(obj, 3);
    if (!t) t = first_label_in(lv_obj_get_parent(obj), 1);
    return t ? t : "";
}

static void handle_enc_group(void)
{
    if (!lvgl_port_lock(1000)) {
        cdc_reply("ENCGROUP: ERR lock\r\n");
        return;
    }
    lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
    uint32_t n = g ? lv_group_get_obj_count(g) : 0;
    char head[64];
    snprintf(head, sizeof(head), "ENCGROUP: count=%u\r\n", (unsigned)n);
    cdc_reply(head);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *o = lv_group_get_obj_by_index(g, i);
        /* Coordinates too: a text list says what is on the screen but not
         * where, and "where" is the only way to check a layout from off the
         * device -- two widgets that overlap look fine in a list of names. */
        lv_area_t a = {0, 0, -1, -1};
        if (o) lv_obj_get_coords(o, &a);
        char line[200];
        snprintf(line, sizeof(line), "  [%u] %p x=%d y=%d w=%d h=%d %s\r\n",
                 (unsigned)i, (void *)o, (int)a.x1, (int)a.y1,
                 (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), widget_label(o));
        cdc_reply(line);
    }
    lvgl_port_unlock();
}

/* Focused-widget readback: the object plus the first label text on it, or
 * driving the UI over ENC_ROTATE is blind. */
static void handle_enc_focus(void)
{
    char reply[192];
    if (lvgl_port_lock(1000)) {
        lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
        lv_obj_t *focused = g ? lv_group_get_focused(g) : nullptr;
        const char *text = widget_label(focused);
        snprintf(reply, sizeof(reply), "ENCFOCUS: obj=%p idx=%d edit=%d text=%s\r\n",
                 (void *)focused,
                 focused ? (int)lv_obj_get_index(focused) : -1,
                 g && lv_group_get_editing(g) ? 1 : 0,
                 text ? text : "");
        lvgl_port_unlock();
    } else {
        snprintf(reply, sizeof(reply), "ENCFOCUS: ERR lock\r\n");
    }
    cdc_reply(reply);
}

/* UI-affecting settings, invisible from outside otherwise — "the drawer arrow
 * is gone" is as often a setting as a bug. */
static void handle_ui_state(void)
{
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    char reply[192];
    snprintf(reply, sizeof(reply),
             "UI: display=%s touch=%s drawer=%d lcd=%d rgb=%d theme=%u launcher=%u bt_icon=%s app=%s\r\n",
             /* Headless: every UI command here is a no-op. Say so, rather
              * than leaving it to be inferred from an empty ENC_GROUP. */
             lv_display_get_default() ? "yes" : "none",
             /* Separate from display=: the LCD is write-only, so touch is the
              * only part of that flex that answers. "no" means the FT6x36 did
              * not ACK; the screen may still be fine. */
             bsp_display_get_input_dev() ? "yes" : "no",
             cfg && cfg->quickSettingsEnabled ? 1 : 0,
             cfg ? (int)cfg->LCDbrightness : -1,
             cfg ? (int)cfg->RGBbrightness : -1,
             cfg ? (unsigned)cfg->themeColorIndex : 0u,
             /* the launcher's look (LauncherStyle) — a mirror (the browser
              * twin) draws the same main screen */
             cfg ? (unsigned)cfg->launcherStyle : 0u,
             /* what the status-bar glyph should look like right now */
             status.bluetoothConnected ? "solid"
             : status.bluetoothActive  ? "dim"
                                       : "hidden",
             running ? running->getName() : "-");
    cdc_reply(reply);
}

/* ── Heavy verbs, off the CDC task ────────────────────────────────────
 *
 * The CDC drain loop (main task, 5 kB stack) is also where the WiFi remote,
 * BLE companion and SysEx companion commands run. It validates, hands work
 * to the owner and answers; it does not do the work. A verb whose result
 * takes longer than a few ms either answers at once and leaves the result to
 * a status verb, or -- where the reply *is* the result and a host already
 * waits for it (SCREENSHOT, STM_FW_UPDATE) -- is answered by the task that
 * does the work, as FW_LIST / FW_INSTALL / PITCHED_BENCH already are. */

/* SCREENSHOT: rendered by LVGL in 40-row strips (lv_snapshot's own layer
 * redraw, one strip at a time) into one 38 kB buffer, and each strip written
 * by this task -- never the drain loop. A whole-screen RGB888 snapshot needs
 * one 230 kB block, which a board with a kit loaded does not have (PSRAM
 * largest ~90 kB: "ERR SCREENSHOT no memory"). The BMP is top-down (negative
 * height). A UI that changes mid-capture can show a seam between strips. */
static std::atomic<bool> s_shot_busy{false};
constexpr int32_t kShotRows = 40;
static lv_draw_buf_t* s_shotBuf = nullptr;      /* LVGL thread only */
static std::atomic<int32_t> s_shotY{0};
static std::atomic<int> s_shotState{0};         /* 0 pending, 1 rendered, 2 no memory */
static int32_t s_shotW = 0, s_shotH = 0;

static void shot_render_strip(void *)
{
    lv_obj_t* scr = lv_screen_active();
    lv_area_t a;
    lv_obj_get_coords(scr, &a);
    s_shotW = lv_area_get_width(&a);
    s_shotH = lv_area_get_height(&a);
    if (!s_shotBuf) s_shotBuf = lv_draw_buf_create(s_shotW, kShotRows, LV_COLOR_FORMAT_RGB888, LV_STRIDE_AUTO);
    if (!s_shotBuf) { s_shotState.store(2); return; }
    lv_draw_buf_clear(s_shotBuf, NULL);
    const int32_t y1 = a.y1 + s_shotY.load();
    lv_area_t strip = {a.x1, y1, a.x2, LV_MIN(y1 + kShotRows - 1, a.y2)};
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = s_shotBuf;
    layer.buf_area = {a.x1, y1, a.x1 + s_shotW - 1, y1 + kShotRows - 1};
    layer.color_format = LV_COLOR_FORMAT_RGB888;
    layer._clip_area = strip;
    layer.phy_clip_area = strip;
    lv_display_t* disp = lv_obj_get_display(scr);
    lv_display_t* dispOld = lv_refr_get_disp_refreshing();
    lv_layer_t* headOld = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    lv_obj_redraw(&layer, scr);
    /* and what the panel shows over it (the quick settings drawer, overlays):
     * a twin compares its own screen, top layer included, with this one */
    if (lv_obj_t* top = lv_display_get_layer_top(disp)) lv_obj_redraw(&layer, top);
    while (layer.draw_task_head) { lv_draw_dispatch_wait_for_request(); lv_draw_dispatch(); }
    disp->layer_head = headOld;
    lv_refr_set_disp_refreshing(dispOld);
    s_shotState.store(1);
}

static void screenshot_task(void *)
{
    const char* err = nullptr;
    FILE* f = fopen("/sdcard/crosspad/screen.bmp", "wb");
    if (!f) err = "ERR SCREENSHOT card\r\n";
    for (int32_t y = 0; !err && (y == 0 || y < s_shotH); y += kShotRows) {
        s_shotY.store(y);
        s_shotState.store(0);
        lv_async_call_locked(shot_render_strip, nullptr);
        for (int waited = 0; s_shotState.load() == 0 && waited < 3000; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
        const int st = s_shotState.load();
        if (st != 1) { err = st == 2 ? "ERR SCREENSHOT no memory\r\n" : "ERR SCREENSHOT busy\r\n"; break; }
        const uint32_t w = (uint32_t)s_shotW, row = (w * 3 + 3) & ~3u;
        if (y == 0) {
            uint8_t hd[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { for (int k = 0; k < 4; ++k) hd[at + k] = (uint8_t)(v >> (8 * k)); };
            put32(2, 54 + row * s_shotH); put32(10, 54); put32(14, 40); put32(18, w);
            put32(22, (uint32_t)(-s_shotH));   /* top-down */
            hd[26] = 1; hd[28] = 24; put32(34, row * s_shotH);
            if (fwrite(hd, 1, 54, f) != 54) err = "ERR SCREENSHOT card\r\n";
        }
        static const uint8_t pad[3] = {0, 0, 0};
        /* LVGL's RGB888 is B, G, R in memory, as BMP wants. */
        for (int32_t r = 0; !err && r < kShotRows && y + r < s_shotH; ++r) {
            if (fwrite(s_shotBuf->data + (uint32_t)r * s_shotBuf->header.stride, 1, w * 3, f) != w * 3 ||
                (row != w * 3 && fwrite(pad, 1, row - w * 3, f) != row - w * 3)) err = "ERR SCREENSHOT card\r\n";
        }
    }
    if (f && fclose(f) != 0 && !err) err = "ERR SCREENSHOT card\r\n";
    /* The strip buffer is LVGL's to free, on its thread. */
    lv_async_call_locked([](void*) { if (s_shotBuf) { lv_draw_buf_destroy(s_shotBuf); s_shotBuf = nullptr; } }, nullptr);
    cdc_reply(err ? err : "SCREENSHOT: /sdcard/crosspad/screen.bmp\r\n");
    s_shot_busy.store(false);
    vTaskDeleteWithCaps(nullptr);   /* created WithCaps */
}

bool verb_tbar(VerbLine &l)
{
    char *cmd = l.cmd;
    /* TBAR -- the transport bar's four readouts, with geometry. They are in
     * no encoder group, so ENC_GROUP cannot reach them, and SCREENSHOT wants
     * 230 kB of contiguous PSRAM a board with a kit loaded does not have. */
    if (strcmp(cmd, "TBAR") == 0) {
        crosspad_gui::TransportBarReadout r[4];
        const uint8_t n = crosspad_gui::transportbar_probe(r, 4);
        if (n == 0) { cdc_reply("TBAR: none\r\n"); return true; }
        for (uint8_t i = 0; i < n; i++) {
            char row[128];
            snprintf(row, sizeof(row), "TBAR: %s x=%d y=%d w=%d h=%d text=%s\r\n",
                     r[i].id, (int)r[i].x, (int)r[i].y, (int)r[i].w, (int)r[i].h,
                     r[i].text ? r[i].text : "");
            cdc_reply(row);
        }
        cdc_reply("TBAR: end\r\n");
        return true;
    }
    return false;
}

bool verb_ui_state(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "UI_STATE", 8) == 0) {
        handle_ui_state();
        return true;
    }
    return false;
}

bool verb_pwr_gesture(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PWR_GESTURE ", 12) == 0) {
        /* The power button cannot be pressed remotely; this injects what the
         * poll would have produced. The resolved context is logged by PWR_BTN
         * (the dispatch is asynchronous, so the reply cannot carry it). */
        const char *arg = cmd + 12;
        if (strncmp(arg, "CLICK", 5) == 0)      power_button_inject(false);
        else if (strncmp(arg, "HOLD", 4) == 0)  power_button_inject(true);
        else { cdc_reply("ERR PWR_GESTURE CLICK|HOLD\r\n"); return true; }
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_toast(VerbLine &l)
{
    char *cmd = l.cmd;
    /* TOAST <text> -- the toast a screen shows, with this text: how a long
     * one wraps and how long it stays, seen with SCREENSHOT. */
    if (strncmp(cmd, "TOAST ", 6) == 0) {
        static char s_toastText[200];
        snprintf(s_toastText, sizeof(s_toastText), "%s", cmd + 6);
        lv_async_call_locked([](void*) { ui_toast_show(s_toastText, 0, UI_TOAST_WARNING); }, nullptr);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_screenshot(VerbLine &l)
{
    char *cmd = l.cmd;
    /* SCREENSHOT -- the active screen as /sdcard/crosspad/screen.bmp (24 bit),
     * for looking at a UI from the host: pull it with fs_transfer. */
    if (strcmp(cmd, "SCREENSHOT") == 0) {
        /* Rendered by the LVGL task, written by a task of its own, answered
         * by that task with the same line (screenshot_task): taken here, on
         * the 4 kB main task, the render overflowed it, and the wait and the
         * card write held every other verb behind them. */
        if (s_shot_busy.exchange(true)) { cdc_reply("ERR SCREENSHOT busy\r\n"); return true; }
        if (xTaskCreatePinnedToCoreWithCaps(screenshot_task, "screenshot", 4096, nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_shot_busy.store(false);
            cdc_reply("ERR SCREENSHOT no task\r\n");
        }
        return true;
    }
    return false;
}

bool verb_touch_trace(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "TOUCH_TRACE ", 12) == 0) {
        handle_touch_trace(cmd + 12);
        return true;
    }
    return false;
}

bool verb_touch_drag(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "TOUCH_DRAG ", 11) == 0) {
        handle_touch_inject(cmd + 11, false);
        return true;
    }
    return false;
}

bool verb_touch_tap(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "TOUCH_TAP ", 10) == 0) {
        handle_touch_inject(cmd + 10, true);
        return true;
    }
    return false;
}

bool verb_topbar(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "TOPBAR") == 0) {
        handle_topbar();
        return true;
    }
    return false;
}

bool verb_ui_tree(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "UI_TREE") == 0) {
        /* During boot, before crosspad_gui::bootstrap(), the probe would
         * assert in getGuiPlatform(): a host polling for the first screen
         * panicked the board into a boot loop. */
        if (!status.isGuiInitialised) {
            cdc_reply("ERR UI_TREE not_ready\r\n");
            return true;
        }
        handle_ui_tree();
        return true;
    }
    return false;
}

bool verb_lvgl_prof(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "LVGL_PROF", 9) == 0) {
        /* LVGL_PROF 1 arms the trace, 2 arms it for the next big frame,
         * 0 stops and dumps it. */
        lvgl_stats_prof(atoi(cmd + 9), [](const char* l) { cdc_reply(l); });
        cdc_reply("LVGLPROF: end\r\n");
        return true;
    }
    return false;
}

bool verb_lvgl_inv(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "LVGL_INV", 8) == 0) {
        /* Which rectangles a UI step dirtied, not just how many pixels. */
        lvgl_stats_inv_dump([](const char* l) { cdc_reply(l); });
        return true;
    }
    return false;
}

bool verb_splash(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "SPLASH") == 0) {
        /* The boot splash again, over whatever is on screen, gone by itself
         * after ~2.2 s: to look at it, and bracketed by LVGL_STATS, to time
         * its frames without a reboot. */
        /* Started on the LVGL thread: it decodes a 300 kB image, which held
         * the LVGL lock -- and this task -- for the whole decode. */
        const bool running = boot_splash_active();
        cdc_reply(running ? "ERR SPLASH already playing\r\n" : "OK\r\n");   /* reply first */
        if (!running) {
            lv_async_call_locked([](void*) {
                if (boot_splash_active()) return;
                auto* cfg = crosspad::CrosspadSettings::getInstance();
                boot_splash_start(lv_layer_top(), cfg ? cfg->LCDbrightness : 100,
                                  [](uint8_t v) { crosspad::getStm32CommandSender().sendBrightness(v); }, true);
            }, nullptr);
        }
        return true;
    }
    return false;
}

bool verb_lvgl_stats(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "LVGL_STATS", 10) == 0) {
        /* Read-and-cleared like CDC_STATS: bracket a window with two reads. */
        char reply[240];
        lvgl_stats_report(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_app_list(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_LIST", 8) == 0) {
        handle_app_list();
        return true;
    }
    return false;
}

bool verb_app_start(VerbLine &l)
{
    char *cmd = l.cmd;
    char (&name)[32] = l.name;
    if (sscanf(cmd, "APP_START %31s", name) == 1) {
        if (!find_app(name)) {
            cdc_reply("ERR unknown app\r\n");
            return true;
        }
        strncpy(s_pending_app, name, sizeof(s_pending_app) - 1);
        s_pending_app[sizeof(s_pending_app) - 1] = '\0';
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_start, nullptr);
        return true;
    }
    return false;
}

bool verb_enc_focus(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ENC_FOCUS", 9) == 0) {
        handle_enc_focus();
        return true;
    }
    return false;
}

bool verb_enc_group(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ENC_GROUP", 9) == 0) {
        handle_enc_group();
        return true;
    }
    return false;
}

bool verb_enc_raw(VerbLine &l)
{
    char *cmd = l.cmd;
    /* ENC_RAW -- the encoder bytes exactly as the STM sends them. The focus
     * moving while nobody is turning anything is either this wobbling or the
     * driver reading it wrong, and there is no telling which from the UI. */
    if (strncmp(cmd, "ENC_RAW", 7) == 0) {
        const auto& in = crosspad::getStm32MessageHandler().input;
        char reply[96];
        snprintf(reply, sizeof(reply), "ENCRAW: top=%d phase=0x%02X pressed=%d\r\n",
                 (int)in.encoderTop, (unsigned)in.encoderPhase,
                 in.encoderPressed ? 1 : 0);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_enc_state(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ENC_STATE", 9) == 0) {
        handle_enc_state();
        return true;
    }
    return false;
}

bool verb_led_state(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (strncmp(cmd, "LED_STATE", 9) == 0) {
        /* LED_STATE COALESCE 0|1: the A/B of who sends the frames (main.cpp
         * turns it on). Off, the frame marked dirty meanwhile goes out now. */
        if (sscanf(cmd + 9, " COALESCE %d", &a) == 1) {
            auto &leds = crosspad::getPadLedController();
            leds.setCoalescedRefresh(a != 0);
            if (!a) leds.updateLEDs();
        }
        handle_led_state();
        return true;
    }
    return false;
}

bool verb_enc_rotate(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "ENC_ROTATE %d", &a) == 1 && a != 0) {
        s_enc_delta = a > 16 ? 16 : (a < -16 ? -16 : a);
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_rotate, nullptr);
        return true;
    }
    return false;
}

bool verb_enc_press(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "ENC_PRESS %d", &a) == 1 && a > 0) {
        s_enc_press_ms = (uint32_t)(a > 5000 ? 5000 : a);
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_press, nullptr);
        return true;
    }
    return false;
}

bool verb_enc_press_2(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ENC_PRESS", 9) == 0) {
        s_enc_press_ms = 80;
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_press, nullptr);
        return true;
    }
    return false;
}

bool verb_app_self_close(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_SELF_CLOSE", 14) == 0) {
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_self_close, nullptr);
        return true;
    }
    return false;
}

bool verb_app_destroy(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_DESTROY", 11) == 0) {
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_destroy, nullptr);
        return true;
    }
    return false;
}

bool verb_app_stop(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "APP_STOP", 8) == 0) {
        if (!launcher_up()) {
            cdc_reply("ERR no_launcher\r\n");
            return true;
        }
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_stop, nullptr);
        return true;
    }
    return false;
}
