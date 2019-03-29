/*
 * CPU temperature plugin for LXPanel
 *
 * Based on 'cpu' and 'thermal' plugin code from LXPanel
 * Copyright for relevant code as for LXPanel
 *
 */
 
/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <glib/gi18n.h>

#include "plugin.h"

#define BORDER_SIZE 2

#define TEMP_LOW   40.0
#define TEMP_RANGE 50.0

#define MAX_NUM_SENSORS             10

#define PROC_THERMAL_DIRECTORY      "/proc/acpi/thermal_zone/"
#define PROC_THERMAL_TEMPF          "temperature"
#define PROC_THERMAL_TRIP           "trip_points"

#define SYSFS_THERMAL_DIRECTORY     "/sys/class/thermal/"
#define SYSFS_THERMAL_SUBDIR_PREFIX "thermal_zone"
#define SYSFS_THERMAL_TEMPF         "temp"


typedef gint (*GetTempFunc) (char const *);

/* Private context for plugin */

typedef struct
{
    GdkColor foreground_color;			    /* Foreground colour for drawing area */
    GdkColor background_color;			    /* Background colour for drawing area */
    GdkColor low_throttle_color;			/* Colour for bars with ARM freq cap */
    GdkColor high_throttle_color;			/* Colour for bars with throttling */
    GtkWidget *da;				            /* Drawing area */
    cairo_surface_t *pixmap;				/* Pixmap to be drawn on drawing area */
    guint timer;				            /* Timer for periodic update */
    float *stats_cpu;			            /* Ring buffer of temperature values */
    int *stats_throttle;                    /* Ring buffer of throttle status */
    unsigned int ring_cursor;			    /* Cursor for ring buffer */
    guint pixmap_width;				        /* Width of drawing area pixmap; also size of ring buffer; does not include border size */
    guint pixmap_height;			        /* Height of drawing area pixmap; does not include border size */
    int numsensors;
    char *sensor_array[MAX_NUM_SENSORS];
    GetTempFunc get_temperature[MAX_NUM_SENSORS];
    gint temperature[MAX_NUM_SENSORS];
    config_setting_t *settings;
} CPUTempPlugin;

static void redraw_pixmap (CPUTempPlugin * c);
static gboolean cpu_update (CPUTempPlugin * c);
static gboolean configure_event (GtkWidget * widget, GdkEventConfigure * event, CPUTempPlugin * c);
#if !GTK_CHECK_VERSION(3, 0, 0)
static gboolean expose_event (GtkWidget * widget, GdkEventExpose * event, CPUTempPlugin * c);
#else
static gboolean draw (GtkWidget * widget, cairo_t * cr, CPUTempPlugin * c);
#endif

static void cpu_destructor (gpointer user_data);

static gint proc_get_temperature (char const *sensor_path)
{
    FILE *state;
    char buf[256], sstmp[100];
    char *pstr;

    if (sensor_path == NULL) return -1;

    snprintf (sstmp, sizeof (sstmp), "%s%s", sensor_path, PROC_THERMAL_TEMPF);

    if (!(state = fopen( sstmp, "r")))
    {
        g_warning ("cputemp: cannot open %s", sstmp);
        return -1;
    }

    while (fgets (buf, 256, state) && !(pstr = strstr (buf, "temperature:")));
    if (pstr)
    {
        pstr += 12;
        while (*pstr && *pstr == ' ') ++pstr;

        pstr[strlen (pstr) - 3] = '\0';
        fclose (state);
        return atoi (pstr);
    }

    fclose (state);
    return -1;
}

static gint _get_reading (const char *path)
{
    FILE *state;
    char buf[256];
    char *pstr;

    if (!(state = fopen (path, "r")))
    {
        g_warning ("cputemp: cannot open %s", path);
        return -1;
    }

    while (fgets (buf, 256, state) && !(pstr = buf));
    if (pstr)
    {
        fclose (state);
        return atoi (pstr) / 1000;
    }

    fclose (state);
    return -1;
}

static gint sysfs_get_temperature (char const *sensor_path)
{
    char sstmp [100];

    if (sensor_path == NULL) return -1;

    snprintf (sstmp, sizeof (sstmp), "%s%s", sensor_path, SYSFS_THERMAL_TEMPF);

    return _get_reading (sstmp);
}

static gint hwmon_get_temperature (char const *sensor_path)
{
    if (sensor_path == NULL) return -1;
    return _get_reading (sensor_path);
}


static int add_sensor (CPUTempPlugin* c, char const* sensor_path, GetTempFunc get_temp)
{
    if (c->numsensors + 1 > MAX_NUM_SENSORS)
    {
        g_message ("cputemp: Too many sensors (max %d), ignoring '%s'",
            MAX_NUM_SENSORS, sensor_path);
        return -1;
    }

    c->sensor_array[c->numsensors] = g_strdup (sensor_path);
    c->get_temperature[c->numsensors] = get_temp;
    c->numsensors++;

    g_message ("cputemp: Added sensor %s", sensor_path);

    return 0;
}


static gboolean try_hwmon_sensors (CPUTempPlugin* c, const char *path)
{
    GDir *sensorsDirectory;
    const char *sensor_name;
    char sensor_path[100], buf[256];
    FILE *fp;
    gboolean found = FALSE;

    if (!(sensorsDirectory = g_dir_open (path, 0, NULL))) return found;

    while ((sensor_name = g_dir_read_name (sensorsDirectory)))
    {
        if (strncmp (sensor_name, "temp", 4) == 0 &&
            strcmp (&sensor_name[5], "_input") == 0)
        {
            snprintf (sensor_path, sizeof (sensor_path), "%s/temp%c_label", path, sensor_name[4]);
            fp = fopen (sensor_path, "r");
            buf[0] = '\0';
            if (fp)
            {
                if (fgets (buf, 256, fp))
                {
                    char *pp = strchr (buf, '\n');
                    if (pp) *pp = '\0';
                }
                fclose (fp);
            }
            snprintf (sensor_path, sizeof (sensor_path), "%s/%s", path, sensor_name);
            add_sensor (c, sensor_path, hwmon_get_temperature);
            found = TRUE;
        }
    }
    g_dir_close (sensorsDirectory);
    return found;
}

static void find_hwmon_sensors (CPUTempPlugin* c)
{
    char dir_path[100];
    char *cptr;
    int i; /* sensor type num, we'll try up to 4 */

    for (i = 0; i < 4; i++)
    {
        snprintf (dir_path, sizeof (dir_path), "/sys/class/hwmon/hwmon%d/device", i);
        if (try_hwmon_sensors (c, dir_path)) continue;
        /* no sensors found under device/, try parent dir */
        cptr = strrchr (dir_path, '/');
        *cptr = '\0';
        try_hwmon_sensors (c, dir_path);
    }
}

static void find_sensors (CPUTempPlugin* c, char const* directory, char const* subdir_prefix, GetTempFunc get_temp)
{
    GDir *sensorsDirectory;
    const char *sensor_name;
    char sensor_path[100];

    if (!(sensorsDirectory = g_dir_open (directory, 0, NULL))) return;

    /* Scan the thermal_zone directory for available sensors */
    while ((sensor_name = g_dir_read_name (sensorsDirectory)))
    {
        if (sensor_name[0] == '.') continue;
        if (subdir_prefix)
        {
            if (strncmp (sensor_name, subdir_prefix, strlen (subdir_prefix)) != 0)  continue;
        }
        snprintf (sensor_path, sizeof (sensor_path), "%s%s/", directory, sensor_name);
        add_sensor (c, sensor_path, get_temp);
    }
    g_dir_close (sensorsDirectory);
}

static void check_sensors (CPUTempPlugin *c)
{
    int i;

    for (i = 0; i < c->numsensors; i++) g_free (c->sensor_array[i]);
    c->numsensors = 0;

    find_sensors (c, PROC_THERMAL_DIRECTORY, NULL, proc_get_temperature);
    find_sensors (c, SYSFS_THERMAL_DIRECTORY, SYSFS_THERMAL_SUBDIR_PREFIX, sysfs_get_temperature);
    if (c->numsensors == 0) find_hwmon_sensors (c);
    
    g_message ("cputemp: Found %d sensors", c->numsensors);
}

/* Redraw after timer callback or resize. */
static void redraw_pixmap (CPUTempPlugin * c)
{
    GdkColor col, t1col, t2col;
    cairo_t * cr = cairo_create(c->pixmap);
    GtkStyle * style = gtk_widget_get_style(c->da);
    cairo_set_line_width (cr, 1.0);
    /* Erase pixmap. */
    cairo_rectangle(cr, 0, 0, c->pixmap_width, c->pixmap_height);
    col.red = c->background_color.blue;
    col.green = c->background_color.green;
    col.blue = c->background_color.red;
    gdk_cairo_set_source_color(cr, &col);
    cairo_fill(cr);

    /* Recompute pixmap. */
    unsigned int i;
    unsigned int drawing_cursor = c->ring_cursor;
    col.red = c->foreground_color.blue;
    col.green = c->foreground_color.green;
    col.blue = c->foreground_color.red;
    t1col.red = c->low_throttle_color.blue;
    t1col.green = c->low_throttle_color.green;
    t1col.blue = c->low_throttle_color.red;
    t2col.red = c->high_throttle_color.blue;
    t2col.green = c->high_throttle_color.green;
    t2col.blue = c->high_throttle_color.red;
    for (i = 0; i < c->pixmap_width; i++)
    {
        /* Draw one bar of the CPU usage graph. */
        if (c->stats_cpu[drawing_cursor] != 0.0)
        {
            if (c->stats_throttle[drawing_cursor] & 0x4)
                gdk_cairo_set_source_color (cr, &t2col);
            else if (c->stats_throttle[drawing_cursor] & 0x2)
                gdk_cairo_set_source_color (cr, &t1col);
            else
                gdk_cairo_set_source_color (cr, &col);

            float val = c->stats_cpu[drawing_cursor] * 100.0;
            val -= TEMP_LOW;
            val /= TEMP_RANGE;
            cairo_move_to (cr, i + 0.5, c->pixmap_height);
            cairo_line_to (cr, i + 0.5, c->pixmap_height - val * c->pixmap_height);
            cairo_stroke (cr);
        }

        /* Increment and wrap drawing cursor. */
        drawing_cursor += 1;
        if (drawing_cursor >= c->pixmap_width) drawing_cursor = 0;
    }

    /* draw a border in black */
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_set_line_width (cr, 1);
    cairo_move_to (cr, 0, 0);
    cairo_line_to (cr, 0, c->pixmap_height);
    cairo_line_to (cr, c->pixmap_width, c->pixmap_height);
    cairo_line_to (cr, c->pixmap_width, 0);
    cairo_line_to (cr, 0, 0);
    cairo_stroke (cr);

    int fontsize = 12;
    if (c->pixmap_width > 50) fontsize = c->pixmap_height / 3;
    char buffer[10];
    int val = 100 * c->stats_cpu[c->ring_cursor ? c->ring_cursor - 1 : c->pixmap_width - 1];
    sprintf (buffer, "%3d°", val);
    cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, fontsize);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_move_to (cr, (c->pixmap_width >> 1) - ((fontsize * 5) / 4), ((c->pixmap_height + fontsize) >> 1) - 1);
    cairo_show_text (cr, buffer);

    /* check_cairo_status(cr); */
    cairo_destroy(cr);

    /* Redraw pixmap. */
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (cairo_image_surface_get_data (c->pixmap), GDK_COLORSPACE_RGB, TRUE, 8, c->pixmap_width, c->pixmap_height, c->pixmap_width *4, NULL, NULL);
    gtk_image_set_from_pixbuf (GTK_IMAGE (c->da), pixbuf);
}

static gint get_temperature (CPUTempPlugin *c)
{
    gint max = -273, cur, i;

    for (i = 0; i < c->numsensors; i++)
    {
        cur = c->get_temperature[i] (c->sensor_array[i]);
        if (cur > max) max = cur;
        c->temperature[i] = cur;
    }

    return max;
}

#if __arm__
static char *get_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    int len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return NULL;
    if (getline (&line, &len, fp) > 0)
    {
        res = line;
        while (*res)
        {
            if (g_ascii_isspace (*res)) *res = 0;
            res++;
        }
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res;
}

static int get_throttle (CPUTempPlugin *c)
{
    char *buf;
    unsigned int val;

    buf = get_string ("vcgencmd get_throttled");
    if (sscanf (buf, "throttled=0x%x", &val) != 1) val = 0;
    g_free (buf);
    return val;
}
#endif

/* Periodic timer callback. */
static gboolean cpu_update (CPUTempPlugin *c)
{
    if (g_source_is_destroyed (g_main_current_source ()))
        return FALSE;

    int t = get_temperature (c);
    c->stats_cpu[c->ring_cursor] = t / 100.0;
#if __arm__    
    c->stats_throttle[c->ring_cursor] = get_throttle (c);
#else
    c->stats_throttle[c->ring_cursor] = 0;
#endif
    c->ring_cursor += 1;
    if (c->ring_cursor >= c->pixmap_width) c->ring_cursor = 0;

    redraw_pixmap (c);
    return TRUE;
}

/* Handler for configure_event on drawing area. */
static void cpu_configuration_changed (LXPanel *panel, GtkWidget *p)
{
    CPUTempPlugin *c = lxpanel_plugin_get_data (p);

    /* Allocate pixmap and statistics buffer without border pixels. */
    guint new_pixmap_height = panel_get_icon_size (panel) - (BORDER_SIZE << 1);
    guint new_pixmap_width = (new_pixmap_height * 3) >> 1;
    if (new_pixmap_width < 50) new_pixmap_width = 50;
    if ((new_pixmap_width > 0) && (new_pixmap_height > 0))
    {
        /* If statistics buffer does not exist or it changed size, reallocate and preserve existing data. */
        if ((c->stats_cpu == NULL) || (new_pixmap_width != c->pixmap_width))
        {
            float *new_stats_cpu = g_new0 (typeof (*c->stats_cpu), new_pixmap_width);
            int *new_stats_throttle = g_new0 (typeof (*c->stats_throttle), new_pixmap_width);
            if (c->stats_cpu != NULL)
            {
                if (new_pixmap_width > c->pixmap_width)
                {
                    /* New allocation is larger.
                     * Introduce new "oldest" samples of zero following the cursor. */
                    memcpy (&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof (float));
                    memcpy (&new_stats_cpu[new_pixmap_width - c->pixmap_width + c->ring_cursor],
                        &c->stats_cpu[c->ring_cursor], (c->pixmap_width - c->ring_cursor) * sizeof (float));
                    memcpy (&new_stats_throttle[0],
                        &c->stats_throttle[0], c->ring_cursor * sizeof (int));
                    memcpy (&new_stats_throttle[new_pixmap_width - c->pixmap_width + c->ring_cursor],
                        &c->stats_throttle[c->ring_cursor], (c->pixmap_width - c->ring_cursor) * sizeof (int));
                }
                else if (c->ring_cursor <= new_pixmap_width)
                {
                    /* New allocation is smaller, but still larger than the ring buffer cursor.
                     * Discard the oldest samples following the cursor. */
                    memcpy (&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof (float));
                    memcpy (&new_stats_cpu[c->ring_cursor],
                        &c->stats_cpu[c->pixmap_width - new_pixmap_width + c->ring_cursor], (new_pixmap_width - c->ring_cursor) * sizeof (float));
                    memcpy (&new_stats_throttle[0],
                        &c->stats_throttle[0], c->ring_cursor * sizeof (int));
                    memcpy (&new_stats_throttle[c->ring_cursor],
                        &c->stats_throttle[c->pixmap_width - new_pixmap_width + c->ring_cursor], (new_pixmap_width - c->ring_cursor) * sizeof (int));
                }
                else
                {
                    /* New allocation is smaller, and also smaller than the ring buffer cursor.
                     * Discard all oldest samples following the ring buffer cursor and additional samples at the beginning of the buffer. */
                    memcpy (&new_stats_cpu[0],
                        &c->stats_cpu[c->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof (float));
                    memcpy (&new_stats_throttle[0],
                        &c->stats_throttle[c->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof (int));
                    c->ring_cursor = 0;
                }
                g_free (c->stats_cpu);
                g_free (c->stats_throttle);
            }
            c->stats_cpu = new_stats_cpu;
            c->stats_throttle = new_stats_throttle;
        }

        /* Allocate or reallocate pixmap. */
        c->pixmap_width = new_pixmap_width;
        c->pixmap_height = new_pixmap_height;
        if (c->pixmap) cairo_surface_destroy (c->pixmap);
        c->pixmap = cairo_image_surface_create (CAIRO_FORMAT_RGB24, c->pixmap_width, c->pixmap_height);

        /* Redraw pixmap at the new size. */
        redraw_pixmap (c);
    }
}

/* Handler for expose_event on drawing area. */
#if !GTK_CHECK_VERSION(3, 0, 0)
static gboolean expose_event (GtkWidget * widget, GdkEventExpose * event, CPUTempPlugin * c)
#else
static gboolean draw (GtkWidget * widget, cairo_t * cr, CPUTempPlugin * c)
#endif
{
    /* Draw the requested part of the pixmap onto the drawing area.
     * Translate it in both x and y by the border size. */
    if (c->pixmap != NULL)
    {
#if !GTK_CHECK_VERSION(3, 0, 0)
        cairo_t * cr = gdk_cairo_create (gtk_widget_get_window (widget));
        GtkStyle * style = gtk_widget_get_style (c->da);
        gdk_cairo_region (cr, event->region);
        cairo_clip (cr);
        gdk_cairo_set_source_color (cr, &c->foreground_color);
#else
        cairo_set_source_rgb (cr, 0, 0, 0); // FIXME: use black color from style
#endif
        cairo_set_source_surface (cr, c->pixmap, BORDER_SIZE, BORDER_SIZE);
        cairo_paint (cr);
#if !GTK_CHECK_VERSION(3, 0, 0)
        cairo_destroy (cr);
#endif
    }
    return FALSE;
}

/* Plugin constructor. */
static GtkWidget *cpu_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate plugin context and set into Plugin private data pointer. */
    CPUTempPlugin *c = g_new0(CPUTempPlugin, 1);
    GtkWidget *p;
    const char *str;

	c->settings = settings;

    if (config_setting_lookup_string (settings, "Foreground", &str))
    {
        if (!gdk_color_parse (str, &c->foreground_color))
            gdk_color_parse ("dark gray", &c->foreground_color);
    } else gdk_color_parse ("dark gray", &c->foreground_color);

    if (config_setting_lookup_string (settings, "Background", &str))
    {
        if (!gdk_color_parse (str, &c->background_color))
            gdk_color_parse ("light gray", &c->background_color);
    } else gdk_color_parse ("light gray", &c->background_color);

    if (config_setting_lookup_string (settings, "Throttle1", &str))
    {
        if (!gdk_color_parse (str, &c->low_throttle_color))
            gdk_color_parse ("orange", &c->low_throttle_color);
    } else gdk_color_parse ("orange", &c->low_throttle_color);

    if (config_setting_lookup_string (settings, "Throttle2", &str))
    {
        if (!gdk_color_parse (str, &c->high_throttle_color))
            gdk_color_parse ("red", &c->high_throttle_color);
    } else gdk_color_parse ("red", &c->high_throttle_color);

    /* Find the system thermal sensors */
    check_sensors (c);

    /* Allocate top level widget and set into Plugin widget pointer. */
    p = gtk_event_box_new ();
    gtk_widget_set_has_window (p, FALSE);
    lxpanel_plugin_set_data (p, c, cpu_destructor);

    /* Allocate drawing area as a child of top level widget. */
    c->da = gtk_image_new ();
    gtk_widget_add_events (c->da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_BUTTON_MOTION_MASK);
    gtk_container_add (GTK_CONTAINER (p), c->da);

    /* Connect signals. */
#if !GTK_CHECK_VERSION(3, 0, 0)
    g_signal_connect(G_OBJECT (c->da), "expose-event", G_CALLBACK (expose_event), (gpointer) c);
#else
    g_signal_connect(G_OBJECT (c->da), "draw", G_CALLBACK (draw), (gpointer) c);
#endif

    /* Show the widget.  Connect a timer to refresh the statistics. */
    gtk_widget_show (c->da);
    c->timer = g_timeout_add (1500, (GSourceFunc) cpu_update, (gpointer) c);
    return p;
}

/* Plugin destructor. */
static void cpu_destructor(gpointer user_data)
{
    CPUTempPlugin *c = (CPUTempPlugin *) user_data;

    /* Disconnect the timer. */
    g_source_remove(c->timer);

    /* Deallocate memory. */
    cairo_surface_destroy (c->pixmap);
    g_free (c->stats_cpu);
    g_free (c);
}

static gboolean cpu_apply_configuration (gpointer user_data)
{
	char colbuf[32];
    GtkWidget *p = user_data;
    CPUTempPlugin *c = lxpanel_plugin_get_data (p);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->foreground_color));
    config_group_set_string (c->settings, "Foreground", colbuf);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->background_color));
    config_group_set_string (c->settings, "Background", colbuf);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->low_throttle_color));
    config_group_set_string (c->settings, "Throttle1", colbuf);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->high_throttle_color));
    config_group_set_string (c->settings, "Throttle2", colbuf);
}

/* Callback when the configuration dialog is to be shown. */
static GtkWidget *cpu_configure(LXPanel *panel, GtkWidget *p)
{
    CPUTempPlugin * dc = lxpanel_plugin_get_data(p);
    return lxpanel_generic_config_dlg(_("CPU Temperature"), panel,
        cpu_apply_configuration, p,
        _("Foreground colour"), &dc->foreground_color, CONF_TYPE_COLOR,
        _("Background colour"), &dc->background_color, CONF_TYPE_COLOR,
        _("Colour when ARM frequency capped"), &dc->low_throttle_color, CONF_TYPE_COLOR,
        _("Colour when throttled"), &dc->high_throttle_color, CONF_TYPE_COLOR,
        NULL);
}

FM_DEFINE_MODULE(lxpanel_gtk, cputemp)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("CPU Temperature Monitor"),
    .config = cpu_configure,
    .description = N_("Display CPU temperature"),
    .new_instance = cpu_constructor,
    .reconfigure = cpu_configuration_changed,
};
