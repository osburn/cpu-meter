/*
 * cpu_meter — a small CPU-load speedometer.
 *
 * A 0–100% gauge showing the average CPU load of all cores, with a
 * toggle button that reveals the per-core loads.
 *
 * Data source : /proc/stat (one sample per second, busy% = delta-based)
 * Rendering   : GTK 3 + Cairo
 *
 * Build:  make
 *        (or: gcc cpu_meter.c -o cpu_meter $(pkg-config --cflags --libs gtk+-3.0) -lm)
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#define GAUGE_SIZE   400
#define GAUGE_MIN    280  /* smallest gauge; below this the dial is hard to read */
#define WINDOW_W     420
#define WINDOW_H     560
#define VBOX_PAD     14   /* keep in sync with the border width set in main() */
#define VBOX_SPACE   12   /* keep in sync with the spacing set in main()      */
#define MAX_CORES    128

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total;       /* sum of the fields above */
    unsigned long long idle_iowait; /* idle + iowait */
} CpuStat;

typedef struct {
    GtkWidget *win;
    GtkWidget *gauge;
    GtkWidget *btn;
    GtkWidget *core_box;
    GtkWidget *core_bars[MAX_CORES];
    GtkWidget *core_lbls[MAX_CORES];

    int      ncores;
    double  *pct;         /* latest per-core load %          */
    double   target;      /* overall % to ease the needle to */
    double   shown;       /* overall % currently drawn       */
    int      frames;      /* ticks elapsed (screenshot mode) */
    double   fake_load;   /* >= 0: use this value, ignore /proc/stat */
    char    *screenshot;  /* if set: dump a PNG once settled, then quit */
} App;

/* ------------------------------------------------------------------ */
/* /proc/stat                                                          */
/* ------------------------------------------------------------------ */

static CpuStat *read_proc_stat(int *out_n)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return NULL;

    CpuStat *arr = NULL;
    int cap = 0, n = 0;
    char line[512];

    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0)
            continue;
        if (!isdigit((unsigned char)line[3]))
            continue; /* skip the aggregate "cpu  ..." line */

        CpuStat s;
        memset(&s, 0, sizeof s);
        /* user nice system idle iowait irq softirq steal [guest guest_nice] */
        if (sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &s.user, &s.nice, &s.system, &s.idle,
                   &s.iowait, &s.irq, &s.softirq, &s.steal) < 4)
            continue;

        s.total       = s.user + s.nice + s.system + s.idle
                      + s.iowait + s.irq + s.softirq + s.steal;
        s.idle_iowait = s.idle + s.iowait;

        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            arr = realloc(arr, (size_t)cap * sizeof *arr);
            if (!arr) { fclose(f); return NULL; }
        }
        arr[n++] = s;
    }
    fclose(f);

    if (n == 0) { free(arr); return NULL; }
    *out_n = n;
    return arr;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

/* green (0%) -> yellow (50%) -> red (100%) */
static void load_color(double pct, double *r, double *g, double *b)
{
    double t;
    if (pct < 50.0) {
        t = pct / 50.0;
        *r = 0.20 + (0.95 - 0.20) * t;
        *g = 0.78 + (0.80 - 0.78) * t;
        *b = 0.25 + (0.10 - 0.25) * t;
    } else {
        t = (pct - 50.0) / 50.0;
        *r = 0.95 + (0.93 - 0.95) * t;
        *g = 0.80 + (0.22 - 0.80) * t;
        *b = 0.10 + (0.20 - 0.10) * t;
    }
}

/* ------------------------------------------------------------------ */
/* gauge drawing                                                       */
/* ------------------------------------------------------------------ */

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    App *a = ud;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    double cx = W / 2.0;
    double cy = H / 2.0 + 8;
    double R  = MIN(W, H) / 2.0 - 46;

    const double start = M_PI * 0.75;  /* 135°  (bottom-left)   */
    const double sweep = M_PI * 1.5;   /* 270° sweep            */
    double frac = CLAMP(a->shown, 0.0, 100.0) / 100.0;

    /* dial face */
    cairo_arc(cr, cx, cy, R + 22, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.12, 0.13, 0.15);
    cairo_fill(cr);

    /* track */
    cairo_set_line_width(cr, 20);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, 0.24, 0.26, 0.30);
    cairo_arc(cr, cx, cy, R, start, start + sweep);
    cairo_stroke(cr);

    /* value arc */
    if (frac > 0.004) {
        double r, g, b;
        load_color(a->shown, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_arc(cr, cx, cy, R, start, start + sweep * frac);
        cairo_stroke(cr);
    }

    /* ticks + numbers */
    for (int t = 0; t <= 100; t += 5) {
        double ang  = start + sweep * t / 100.0;
        gboolean major = (t % 10 == 0);
        double ca = cos(ang), sa = sin(ang);
        double r1 = R - 18;
        double r2 = R - (major ? 34 : 26);

        cairo_set_line_width(cr, major ? 2.5 : 1.5);
        cairo_set_source_rgb(cr, major ? 0.75 : 0.42, major ? 0.77 : 0.45, major ? 0.80 : 0.48);
        cairo_move_to(cr, cx + ca * r1, cy + sa * r1);
        cairo_line_to(cr, cx + ca * r2, cy + sa * r2);
        cairo_stroke(cr);

        if (major) {
            double rl = R - 52;
            char buf[8];
            g_snprintf(buf, sizeof buf, "%d", t);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 13);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, buf, &ext);
            cairo_set_source_rgb(cr, 0.80, 0.82, 0.86);
            cairo_move_to(cr, cx + ca * rl - ext.width / 2, cy + sa * rl - ext.height / 2);
            cairo_show_text(cr, buf);
        }
    }

    /* needle */
    double na = start + sweep * frac;
    double ca = cos(na), sa = sin(na);
    cairo_set_line_width(cr, 4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, 0.95, 0.96, 0.98);
    cairo_move_to(cr, cx - ca * 12, cy - sa * 12);
    cairo_line_to(cr, cx + ca * (R - 40), cy + sa * (R - 40));
    cairo_stroke(cr);

    /* hub */
    cairo_arc(cr, cx, cy, 9, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.88, 0.30, 0.25);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, 4, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.12, 0.13, 0.15);
    cairo_fill(cr);

    /* big percentage readout */
    char buf[16];
    g_snprintf(buf, sizeof buf, "%.0f%%", a->shown);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 36);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_set_source_rgb(cr, 0.96, 0.97, 0.98);
    /* one full text height (36pt font) below where it used to sit */
    cairo_move_to(cr, cx - ext.width / 2, cy + 46 + 36 - ext.height / 2);
    cairo_show_text(cr, buf);

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* update loop                                                         */
/* ------------------------------------------------------------------ */

static gboolean tick(gpointer ud)
{
    App *a = ud;
    static CpuStat *prev = NULL;
    static int prev_n = 0;
    static guint64 last_read = 0;

    guint64 now = g_get_monotonic_time();

    if (a->fake_load >= 0.0) {
        for (int i = 0; i < a->ncores; i++)
            a->pct[i] = a->fake_load;
        for (int i = 0; i < a->ncores; i++) {
            char buf[16];
            gtk_level_bar_set_value(GTK_LEVEL_BAR(a->core_bars[i]), a->fake_load);
            g_snprintf(buf, sizeof buf, "%.0f%%", a->fake_load);
            gtk_label_set_text(GTK_LABEL(a->core_lbls[i]), buf);
        }
        a->target = a->fake_load;
    } else if (now - last_read >= 1000000) { /* refresh at most once per second */
        int cur_n = 0;
        CpuStat *cur = read_proc_stat(&cur_n);
        if (cur) {
            if (cur_n == prev_n) {
                double sum = 0;
                for (int i = 0; i < cur_n; i++) {
                    unsigned long long dt = cur[i].total - prev[i].total;
                    unsigned long long di = cur[i].idle_iowait - prev[i].idle_iowait;
                    double p = (dt > 0) ? (double)(dt - di) * 100.0 / (double)dt : 0.0;
                    a->pct[i] = p;
                    sum += p;
                }
                for (int i = 0; i < cur_n; i++) {
                    char buf[16];
                    gtk_level_bar_set_value(GTK_LEVEL_BAR(a->core_bars[i]), a->pct[i]);
                    g_snprintf(buf, sizeof buf, "%.0f%%", a->pct[i]);
                    gtk_label_set_text(GTK_LABEL(a->core_lbls[i]), buf);
                }
                a->target = sum / cur_n;
            }
            /* first sample (or core count changed): just take a baseline */
            free(prev);
            prev = cur;
            prev_n = cur_n;
        }
        last_read = now;
    }

    /* ease the needle toward the target for a smooth motion */
    double d = a->target - a->shown;
    if (fabs(d) > 0.02) {
        a->shown += d * 0.30;
        if (fabs(a->target - a->shown) <= 0.02)
            a->shown = a->target;
        gtk_widget_queue_draw(a->gauge);
    }

    /* screenshot mode: dump a PNG once the needle has settled, then quit */
    if (a->screenshot && ++a->frames > 12 && fabs(a->target - a->shown) < 0.4) {
        GdkWindow *gw = gtk_widget_get_window(a->win);
        if (gw) {
            int w = gtk_widget_get_allocated_width(a->win);
            int h = gtk_widget_get_allocated_height(a->win);
            GdkPixbuf *pb = gdk_pixbuf_get_from_window(gw, 0, 0, w, h);
            if (pb) {
                GError *err = NULL;
                gdk_pixbuf_save(pb, a->screenshot, "png", &err, NULL);
                g_print("screenshot saved: %s%s\n", a->screenshot, err ? " (with error)" : "");
                if (err)
                    g_error_free(err);
                g_object_unref(pb);
            }
        }
        gtk_main_quit();
    }

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

/* Size the expanded window so gauge + button + all core rows fit on the
   screen.  The gauge is the flexible part: it shrinks from GAUGE_SIZE
   down to GAUGE_MIN so that a core row never has to give up space.
   Returns the gauge size to use (and sets *win_h), or -1 if the panel
   cannot be measured yet (widgets not realized). */
static int fit_expanded(App *a, int *win_h)
{
    int core_h = 0, btn_h = 0;
    gtk_widget_get_preferred_height(a->core_box, NULL, &core_h);
    gtk_widget_get_preferred_height(a->btn, NULL, &btn_h);
    if (core_h <= 0 || btn_h <= 0)
        return -1;

    int overhead = 2 * VBOX_PAD + 2 * VBOX_SPACE + btn_h + core_h;

    /* height the window may use while staying on the monitor */
    int avail = WINDOW_H; /* fallback when screen info is unavailable */
    GdkDisplay *dpy = gtk_widget_get_display(a->win);
    GdkMonitor *mon = dpy ? gdk_display_get_monitor(dpy, 0) : NULL;
    if (mon) {
        GdkRectangle wa;
        gdk_monitor_get_workarea(mon, &wa);
        avail = wa.height - 40; /* leave room for the window titlebar */
    }

    /* keep the window at WINDOW_W: a square gauge wider than the usable
       width would force a wider window */
    int cap   = MIN(GAUGE_SIZE, WINDOW_W - 2 * VBOX_PAD);
    int gauge = CLAMP(avail - overhead, GAUGE_MIN, cap);
    *win_h = overhead + gauge;
    return gauge;
}

static void on_toggle(GtkToggleButton *btn, gpointer ud)
{
    App *a = ud;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_widget_show(a->core_box); /* rows are already shown; toggle the box itself */
        int win_h;
        int gauge = fit_expanded(a, &win_h);
        if (gauge > 0) {
            /* let the gauge take the height deficit, so every core row
               stays fully visible no matter how short the screen is */
            gtk_widget_set_size_request(a->gauge, gauge, gauge);
            gtk_window_resize(GTK_WINDOW(a->win), WINDOW_W, win_h);
        }
        /* if the panel is not measurable yet, main() sizes the window once shown */
    } else {
        gtk_widget_hide(a->core_box);
        gtk_widget_set_size_request(a->gauge, GAUGE_SIZE, GAUGE_SIZE);
        gtk_window_resize(GTK_WINDOW(a->win), WINDOW_W, WINDOW_H);
    }
    gtk_widget_queue_draw(a->gauge);
}

static const char *CSS =
    "window { background-color: #212429; }"
    "label  { color: #e8eaee; }"
    "button { color: #e8eaee; background-image: none; background-color: #33383f;"
    "         border: none; border-radius: 8px; padding: 8px 20px; }"
    "button:hover   { background-color: #3d434c; }"
    "button:checked { background-color: #4a90d9; }"
    "levelbar trough  { min-height: 12px; background-color: #3a4048; }"
    "levelbar value, levelbar highlight { background-color: #4a90d9; }"
    ;

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    app.fake_load = -1;

    gboolean show_cores = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fake-load") == 0 && i + 1 < argc)
            app.fake_load = atof(argv[++i]);
        else if (strcmp(argv[i], "--show-cores") == 0)
            show_cores = TRUE;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            app.screenshot = strdup(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: cpu_meter [--show-cores] [--fake-load N] [--screenshot FILE.png]\n");
            printf("  --show-cores   open with the per-core panel visible\n");
            printf("  --fake-load N  use N (0-100) instead of the real load (for demos/tests)\n");
            printf("  --screenshot F save a PNG of the window once drawn, then exit\n");
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
    }

    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "error: cannot open a display\n");
        return 1;
    }

    int n = 0;
    CpuStat *tmp = read_proc_stat(&n);
    free(tmp);
    if (n <= 0 || n > MAX_CORES) {
        fprintf(stderr, "error: could not read CPU stats from /proc/stat\n");
        return 1;
    }
    app.ncores = n;
    app.pct = g_malloc0((size_t)n * sizeof *app.pct);

    /* window */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "CPU Meter");
    gtk_window_set_default_size(GTK_WINDOW(win), WINDOW_W, WINDOW_H);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(win), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, VBOX_SPACE);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), VBOX_PAD);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *gauge = gtk_drawing_area_new();
    gtk_widget_set_size_request(gauge, GAUGE_SIZE, GAUGE_SIZE);
    gtk_widget_set_halign(gauge, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), gauge, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_toggle_button_new_with_label("Show all cores");
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

    /* per-core panel (hidden until the button is pressed) */
    GtkWidget *core_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), core_box, FALSE, FALSE, 0);

    for (int i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *cl = gtk_label_new(NULL);
        {
            char name[32];
            g_snprintf(name, sizeof name, "Core %d", i);
            gtk_label_set_text(GTK_LABEL(cl), name);
        }
        gtk_widget_set_halign(cl, GTK_ALIGN_START);
        gtk_widget_set_size_request(cl, 56, -1);

        GtkWidget *bar = gtk_level_bar_new_for_interval(0, 100);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), 0);

        GtkWidget *pl = gtk_label_new(" 0%");
        gtk_widget_set_halign(pl, GTK_ALIGN_END);
        gtk_widget_set_size_request(pl, 44, -1);

        gtk_box_pack_start(GTK_BOX(row), cl,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), bar, TRUE,  TRUE,  0);
        gtk_box_pack_end  (GTK_BOX(row), pl,  FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(core_box), row, FALSE, FALSE, 0);
        app.core_bars[i] = bar;
        app.core_lbls[i] = pl;
    }

    app.win = win;
    app.gauge = gauge;
    app.btn = btn;
    app.core_box = core_box;

    g_signal_connect(win,   "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(btn,   "toggled", G_CALLBACK(on_toggle), &app);
    g_signal_connect(gauge, "draw",    G_CALLBACK(on_draw),   &app);

    if (show_cores)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);

    gtk_widget_show_all(win);
    if (!show_cores) {
        gtk_widget_hide(app.core_box);
    } else {
        /* now that the window is shown, sizes measure correctly
           (on_toggle ran earlier, before the widgets were realized) */
        int win_h;
        int gauge = fit_expanded(&app, &win_h);
        if (gauge > 0) {
            gtk_widget_set_size_request(app.gauge, gauge, gauge);
            gtk_window_resize(GTK_WINDOW(win), WINDOW_W, win_h);
        }
    }

    g_timeout_add(100, tick, &app);
    gtk_main();

    free(app.screenshot);
    g_free(app.pct);
    return 0;
}
