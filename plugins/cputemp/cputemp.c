/*
 * CPU usage plugin to lxpanel
 *
 * Copyright (C) 2006-2008 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *               2006-2008 Jim Huang <jserv.tw@gmail.com>
 *               2009 Marty Jack <martyj19@comcast.net>
 *               2009 Jürgen Hötzel <juergen@archlinux.org>
 *               2012 Rafał Mużyło <galtgendo@gmail.com>
 *               2012-2013 Henry Gebhardt <hsggebhardt@gmail.com>
 *               2013 Marko Rauhamaa <marko@pacujo.net>
 *               2014 Andriy Grytsenko <andrej@rep.kiev.ua>
 *               2015 Rafał Mużyło <galtgendo@gmail.com>
 *
 * This file is a part of LXPanel project.
 *
 * Copyright (C) 2004 by Alexandre Pereira da Silva <alexandre.pereira@poli.usp.br>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
/*A little bug fixed by Mykola <mykola@2ka.mipt.ru>:) */

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <glib/gi18n.h>

#include "plugin.h"

#define MAX_NUM_SENSORS 10
#define BORDER_SIZE 2
#define PROC_THERMAL_DIRECTORY "/proc/acpi/thermal_zone/" /* must be slash-terminated */
#define PROC_THERMAL_TEMPF  "temperature"
#define PROC_THERMAL_TRIP  "trip_points"
#define PROC_TRIP_CRITICAL "critical (S5):"

#define SYSFS_THERMAL_DIRECTORY "/sys/class/thermal/" /* must be slash-terminated */
#define SYSFS_THERMAL_SUBDIR_PREFIX "thermal_zone"
#define SYSFS_THERMAL_TEMPF  "temp"
#define SYSFS_THERMAL_TRIP  "trip_point_0_temp"

#define TEMP_LOW   40.0
#define TEMP_RANGE 50.0


/* #include "../../dbg.h" */

typedef unsigned long long CPUTick;		/* Value from /proc/stat */
typedef float CPUSample;			/* Saved CPU utilization value as 0.0..1.0 */
typedef gint (*GetTempFunc)(char const *);


struct cpu_stat {
    CPUTick u, n, s, i;				/* User, nice, system, idle */
};

/* Private context for CPU plugin. */
typedef struct {
    GdkColor foreground_color;			/* Foreground color for drawing area */
    GdkColor background_color;			/* Background color for drawing area */
    GtkWidget * da;				/* Drawing area */
    cairo_surface_t * pixmap;				/* Pixmap to be drawn on drawing area */

    guint timer;				/* Timer for periodic update */
    CPUSample * stats_cpu;			/* Ring buffer of CPU utilization values */
    unsigned int ring_cursor;			/* Cursor for ring buffer */
    guint pixmap_width;				/* Width of drawing area pixmap; also size of ring buffer; does not include border size */
    guint pixmap_height;			/* Height of drawing area pixmap; does not include border size */
    gboolean show_percentage;				/* Display usage as a percentage */


    int warning1;
    int warning2;
    int not_custom_levels, auto_sensor;
    char *sensor;
    int numsensors;
    char *sensor_array[MAX_NUM_SENSORS];
    char *sensor_name[MAX_NUM_SENSORS];
    GetTempFunc get_temperature[MAX_NUM_SENSORS];
    GetTempFunc get_critical[MAX_NUM_SENSORS];
    gint temperature[MAX_NUM_SENSORS];
    gint critical[MAX_NUM_SENSORS];


    config_setting_t *settings;
} CPUTempPlugin;

static void redraw_pixmap(CPUTempPlugin * c);
static gboolean cpu_update(CPUTempPlugin * c);
static gboolean configure_event(GtkWidget * widget, GdkEventConfigure * event, CPUTempPlugin * c);
#if !GTK_CHECK_VERSION(3, 0, 0)
static gboolean expose_event(GtkWidget * widget, GdkEventExpose * event, CPUTempPlugin * c);
#else
static gboolean draw(GtkWidget * widget, cairo_t * cr, CPUTempPlugin * c);
#endif

static void cpu_destructor(gpointer user_data);

static gint _get_reading(const char *path, gboolean quiet)
{
    FILE *state;
    char buf[256];
    char* pstr;

    if (!(state = fopen(path, "r"))) {
        if (!quiet)
            g_warning("thermal: cannot open %s", path);
        return -1;
    }

    while( fgets(buf, 256, state) &&
            ! ( pstr = buf ) );
    if( pstr )
    {
        fclose(state);
        return atoi(pstr)/1000;
    }

    fclose(state);
    return -1;
}

static gint
proc_get_critical(char const* sensor_path){
    FILE *state;
    char buf[ 256 ], sstmp [ 100 ];
    char* pstr;

    if(sensor_path == NULL) return -1;

    snprintf(sstmp,sizeof(sstmp),"%s%s",sensor_path,PROC_THERMAL_TRIP);

    if (!(state = fopen( sstmp, "r"))) {
        g_warning("thermal: cannot open %s", sstmp);
        return -1;
    }

    while( fgets(buf, 256, state) &&
            ! ( pstr = strstr(buf, PROC_TRIP_CRITICAL) ) );
    if( pstr )
    {
        pstr += strlen(PROC_TRIP_CRITICAL);
        while( *pstr && *pstr == ' ' )
            ++pstr;

        pstr[strlen(pstr)-3] = '\0';
        fclose(state);
        return atoi(pstr);
    }

    fclose(state);
    return -1;
}

static gint
proc_get_temperature(char const* sensor_path){
    FILE *state;
    char buf[ 256 ], sstmp [ 100 ];
    char* pstr;

    if(sensor_path == NULL) return -1;

    snprintf(sstmp,sizeof(sstmp),"%s%s",sensor_path,PROC_THERMAL_TEMPF);

    if (!(state = fopen( sstmp, "r"))) {
        g_warning("thermal: cannot open %s", sstmp);
        return -1;
    }

    while( fgets(buf, 256, state) &&
            ! ( pstr = strstr(buf, "temperature:") ) );
    if( pstr )
    {
        pstr += 12;
        while( *pstr && *pstr == ' ' )
            ++pstr;

        pstr[strlen(pstr)-3] = '\0';
        fclose(state);
        return atoi(pstr);
    }

    fclose(state);
    return -1;
}

static gint
sysfs_get_critical(char const* sensor_path){
    char sstmp [ 100 ];

    if(sensor_path == NULL) return -1;

    snprintf(sstmp,sizeof(sstmp),"%s%s",sensor_path,SYSFS_THERMAL_TRIP);

    return _get_reading(sstmp, TRUE);
}

static gint
sysfs_get_temperature(char const* sensor_path){
    char sstmp [ 100 ];

    if(sensor_path == NULL) return -1;

    snprintf(sstmp,sizeof(sstmp),"%s%s",sensor_path,SYSFS_THERMAL_TEMPF);

    return _get_reading(sstmp, FALSE);
}

static gint
hwmon_get_critical(char const* sensor_path)
{
    char sstmp [ 100 ];
    int spl;

    if(sensor_path == NULL) return -1;

    spl = strlen(sensor_path) - 6;
    if (spl < 17 || spl > 94)
        return -1;

    snprintf(sstmp, sizeof(sstmp), "%.*s_crit", spl, sensor_path);

    return _get_reading(sstmp, TRUE);
}

static gint
hwmon_get_temperature(char const* sensor_path)
{
    if(sensor_path == NULL) return -1;

    return _get_reading(sensor_path, FALSE);
}


static int
add_sensor(CPUTempPlugin* th, char const* sensor_path, const char *sensor_name,
           GetTempFunc get_temp, GetTempFunc get_crit)
{
    if (th->numsensors + 1 > MAX_NUM_SENSORS){
        g_warning("thermal: Too many sensors (max %d), ignoring '%s'",
                MAX_NUM_SENSORS, sensor_path);
        return -1;
    }

    th->sensor_array[th->numsensors] = g_strdup(sensor_path);
    th->sensor_name[th->numsensors] = g_strdup(sensor_name);
    th->get_critical[th->numsensors] = get_crit;
    th->get_temperature[th->numsensors] = get_temp;
    th->numsensors++;

    g_debug("thermal: Added sensor %s", sensor_path);

    return 0;
}


static gboolean try_hwmon_sensors(CPUTempPlugin* th, const char *path)
{
    GDir *sensorsDirectory;
    const char *sensor_name;
    char sensor_path[100], buf[256];
    FILE *fp;
    gboolean found = FALSE;

    if (!(sensorsDirectory = g_dir_open(path, 0, NULL)))
        return found;

    while ((sensor_name = g_dir_read_name(sensorsDirectory)))
    {
        if (strncmp(sensor_name, "temp", 4) == 0 &&
            strcmp(&sensor_name[5], "_input") == 0)
        {
            snprintf(sensor_path, sizeof(sensor_path), "%s/temp%c_label", path,
                     sensor_name[4]);
            fp = fopen(sensor_path, "r");
            buf[0] = '\0';
            if (fp)
            {
                if (fgets(buf, 256, fp))
                {
                    char *pp = strchr(buf, '\n');
                    if (pp)
                        *pp = '\0';
                }
                fclose(fp);
            }
            snprintf(sensor_path, sizeof(sensor_path), "%s/%s", path, sensor_name);
            add_sensor(th, sensor_path, buf[0] ? buf : sensor_name,
                       hwmon_get_temperature, hwmon_get_critical);
            found = TRUE;
        }
    }
    g_dir_close(sensorsDirectory);
    return found;
}

static void find_hwmon_sensors(CPUTempPlugin* th)
{
    char dir_path[100];
    char *c;
    int i; /* sensor type num, we'll try up to 4 */

    for (i = 0; i < 4; i++)
    {
        snprintf(dir_path, sizeof(dir_path), "/sys/class/hwmon/hwmon%d/device", i);
        if (try_hwmon_sensors(th, dir_path))
            continue;
        /* no sensors found under device/, try parent dir */
        c = strrchr(dir_path, '/');
        *c = '\0';
        try_hwmon_sensors(th, dir_path);
    }
}




/* find_sensors():
 *      - Get the sensor directory, and store it in '*sensor'.
 *      - It is searched for in 'directory'.
 *      - Only the subdirectories starting with 'subdir_prefix' are accepted as sensors.
 *      - 'subdir_prefix' may be NULL, in which case any subdir is considered a sensor. */
static void
find_sensors(CPUTempPlugin* th, char const* directory, char const* subdir_prefix,
             GetTempFunc get_temp, GetTempFunc get_crit)
{
    GDir *sensorsDirectory;
    const char *sensor_name;
    char sensor_path[100];

    if (! (sensorsDirectory = g_dir_open(directory, 0, NULL)))
        return;

    /* Scan the thermal_zone directory for available sensors */
    while ((sensor_name = g_dir_read_name(sensorsDirectory))) {
        if (sensor_name[0] == '.')
            continue;
        if (subdir_prefix) {
            if (strncmp(sensor_name, subdir_prefix, strlen(subdir_prefix)) != 0)
                continue;
        }
        snprintf(sensor_path,sizeof(sensor_path),"%s%s/", directory, sensor_name);
        add_sensor(th, sensor_path, sensor_name, get_temp, get_crit);
    }
    g_dir_close(sensorsDirectory);
}

static void
remove_all_sensors(CPUTempPlugin *th)
{
    int i;

    g_debug("thermal: Removing all sensors (%d)", th->numsensors);

    for (i = 0; i < th->numsensors; i++)
    {
        g_free(th->sensor_array[i]);
        g_free(th->sensor_name[i]);
    }

    th->numsensors = 0;
}

static void
check_sensors( CPUTempPlugin *th )
{
    // FIXME: scan in opposite order
    find_sensors(th, PROC_THERMAL_DIRECTORY, NULL, proc_get_temperature, proc_get_critical);
    find_sensors(th, SYSFS_THERMAL_DIRECTORY, SYSFS_THERMAL_SUBDIR_PREFIX, sysfs_get_temperature, sysfs_get_critical);
    if (th->numsensors == 0)
        find_hwmon_sensors(th);
    g_info("thermal: Found %d sensors", th->numsensors);
}



/* Redraw after timer callback or resize. */
static void redraw_pixmap(CPUTempPlugin * c)
{
    GdkColor col;
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
    gdk_cairo_set_source_color(cr, &col);
    for (i = 0; i < c->pixmap_width; i++)
    {
        /* Draw one bar of the CPU usage graph. */
        if (c->stats_cpu[drawing_cursor] != 0.0)
        {
            float val = c->stats_cpu[drawing_cursor] * 100.0;
            val -= TEMP_LOW;
            val /= TEMP_RANGE;
            cairo_move_to(cr, i + 0.5, c->pixmap_height);
            cairo_line_to(cr, i + 0.5, c->pixmap_height - val * c->pixmap_height);
            cairo_stroke(cr);
        }

        /* Increment and wrap drawing cursor. */
        drawing_cursor += 1;
        if (drawing_cursor >= c->pixmap_width)
            drawing_cursor = 0;
    }

    /* draw a border in black */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 0, c->pixmap_height);
    cairo_line_to(cr, c->pixmap_width, c->pixmap_height);
    cairo_line_to(cr, c->pixmap_width, 0);
    cairo_line_to(cr, 0, 0);
    cairo_stroke(cr);

    if (c->show_percentage)
    {
        int fontsize = 12;
        if (c->pixmap_width > 50) fontsize = c->pixmap_height / 3;
        char buffer[10];
        int val = 100 * c->stats_cpu[c->ring_cursor ? c->ring_cursor - 1 : c->pixmap_width - 1];
        sprintf (buffer, "%3d°", val);
        cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, fontsize);
        cairo_set_source_rgb (cr, 0, 0, 0);
        cairo_move_to (cr, (c->pixmap_width >> 1) - ((fontsize * 5) / 3), ((c->pixmap_height + fontsize) >> 1) - 1);
        cairo_show_text (cr, buffer);
    }

    /* check_cairo_status(cr); */
    cairo_destroy(cr);

    /* Redraw pixmap. */
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (cairo_image_surface_get_data (c->pixmap), GDK_COLORSPACE_RGB, TRUE, 8, c->pixmap_width, c->pixmap_height, c->pixmap_width *4, NULL, NULL);
    gtk_image_set_from_pixbuf (GTK_IMAGE (c->da), pixbuf);
}

static gint get_temperature(CPUTempPlugin *th, gint *warn)
{
    gint max = -273;
    gint cur, i, w = 0;

    for(i = 0; i < th->numsensors; i++){
        cur = th->get_temperature[i](th->sensor_array[i]);
        if (w == 2) ; /* already warning2 */
        else if (th->not_custom_levels &&
                 th->critical[i] > 0 && cur >= th->critical[i] - 5)
            w = 2;
        else if ((!th->not_custom_levels || th->critical[i] < 0) &&
                 cur >= th->warning2)
            w = 2;
        else if (w == 1) ; /* already warning1 */
        else if (th->not_custom_levels &&
                 th->critical[i] > 0 && cur >= th->critical[i] - 10)
            w = 1;
        else if ((!th->not_custom_levels || th->critical[i] < 0) &&
                 cur >= th->warning1)
            w = 1;
        if (cur > max)
            max = cur;
        th->temperature[i] = cur;
    }
    *warn = w;

    return max;
}




/* Periodic timer callback. */
static gboolean cpu_update(CPUTempPlugin * c)
{
    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;
        
        int i, t;
        
        t = get_temperature (c, &i);
            c->stats_cpu[c->ring_cursor] = t / 100.0; 
            c->ring_cursor += 1;
            if (c->ring_cursor >= c->pixmap_width)
                c->ring_cursor = 0;

            /* Redraw with the new sample. */
            redraw_pixmap(c);
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
            CPUSample * new_stats_cpu = g_new0(typeof(*c->stats_cpu), new_pixmap_width);
            if (c->stats_cpu != NULL)
            {
                if (new_pixmap_width > c->pixmap_width)
                {
                    /* New allocation is larger.
                     * Introduce new "oldest" samples of zero following the cursor. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof(CPUSample));
                    memcpy(&new_stats_cpu[new_pixmap_width - c->pixmap_width + c->ring_cursor],
                        &c->stats_cpu[c->ring_cursor], (c->pixmap_width - c->ring_cursor) * sizeof(CPUSample));
                }
                else if (c->ring_cursor <= new_pixmap_width)
                {
                    /* New allocation is smaller, but still larger than the ring buffer cursor.
                     * Discard the oldest samples following the cursor. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[0], c->ring_cursor * sizeof(CPUSample));
                    memcpy(&new_stats_cpu[c->ring_cursor],
                        &c->stats_cpu[c->pixmap_width - new_pixmap_width + c->ring_cursor], (new_pixmap_width - c->ring_cursor) * sizeof(CPUSample));
                }
                else
                {
                    /* New allocation is smaller, and also smaller than the ring buffer cursor.
                     * Discard all oldest samples following the ring buffer cursor and additional samples at the beginning of the buffer. */
                    memcpy(&new_stats_cpu[0],
                        &c->stats_cpu[c->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof(CPUSample));
                    c->ring_cursor = 0;
                }
                g_free(c->stats_cpu);
            }
            c->stats_cpu = new_stats_cpu;
        }

        /* Allocate or reallocate pixmap. */
        c->pixmap_width = new_pixmap_width;
        c->pixmap_height = new_pixmap_height;
        if (c->pixmap)
            cairo_surface_destroy(c->pixmap);
        c->pixmap = cairo_image_surface_create(CAIRO_FORMAT_RGB24, c->pixmap_width, c->pixmap_height);
        /* check_cairo_surface_status(&c->pixmap); */

        /* Redraw pixmap at the new size. */
        redraw_pixmap(c);
    }
}

/* Handler for expose_event on drawing area. */
#if !GTK_CHECK_VERSION(3, 0, 0)
static gboolean expose_event(GtkWidget * widget, GdkEventExpose * event, CPUTempPlugin * c)
#else
static gboolean draw(GtkWidget * widget, cairo_t * cr, CPUTempPlugin * c)
#endif
{
    /* Draw the requested part of the pixmap onto the drawing area.
     * Translate it in both x and y by the border size. */
    if (c->pixmap != NULL)
    {
#if !GTK_CHECK_VERSION(3, 0, 0)
        cairo_t * cr = gdk_cairo_create(gtk_widget_get_window(widget));
        GtkStyle * style = gtk_widget_get_style(c->da);
        gdk_cairo_region(cr, event->region);
        cairo_clip(cr);
        gdk_cairo_set_source_color(cr, &c->foreground_color);
#else
        cairo_set_source_rgb(cr, 0, 0, 0); // FIXME: use black color from style
#endif
        cairo_set_source_surface(cr, c->pixmap,
              BORDER_SIZE, BORDER_SIZE);
        cairo_paint(cr);
        /* check_cairo_status(cr); */
#if !GTK_CHECK_VERSION(3, 0, 0)
        cairo_destroy(cr);
#endif
    }
    return FALSE;
}

/* Plugin constructor. */
static GtkWidget *cpu_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate plugin context and set into Plugin private data pointer. */
    CPUTempPlugin * c = g_new0(CPUTempPlugin, 1);
    GtkWidget * p;
    int tmp_int;
    const char *str;

	c->settings = settings;
    if (config_setting_lookup_int(settings, "ShowPercent", &tmp_int))
        c->show_percentage = tmp_int != 0;

    if (config_setting_lookup_string(settings, "Foreground", &str))
    {
	if (!gdk_color_parse (str, &c->foreground_color))
		gdk_color_parse("dark gray",  &c->foreground_color);
    } else gdk_color_parse("dark gray",  &c->foreground_color);

    if (config_setting_lookup_string(settings, "Background", &str))
    {
	if (!gdk_color_parse (str, &c->background_color))
		gdk_color_parse("light gray",  &c->background_color);
    } else gdk_color_parse("light gray",  &c->background_color);
    
    
    remove_all_sensors(c);
    /* FIXME: support wildcards in th->sensor */
    if(c->sensor == NULL) c->auto_sensor = TRUE;
    if(c->auto_sensor) check_sensors(c);
    else if (strncmp(c->sensor, "/sys/", 5) != 0)
        add_sensor(c, c->sensor, c->sensor, proc_get_temperature, proc_get_critical);
    else if (strncmp(c->sensor, "/sys/class/hwmon/", 17) != 0)
        add_sensor(c, c->sensor, c->sensor, sysfs_get_temperature, sysfs_get_critical);
    else
        add_sensor(c, c->sensor, c->sensor, hwmon_get_temperature, hwmon_get_critical);


    
    

    /* Allocate top level widget and set into Plugin widget pointer. */
    p = gtk_event_box_new();
    gtk_widget_set_has_window(p, FALSE);
    lxpanel_plugin_set_data(p, c, cpu_destructor);

    /* Allocate drawing area as a child of top level widget. */
    c->da = gtk_image_new();
    gtk_widget_add_events(c->da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                 GDK_BUTTON_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(p), c->da);

    /* Connect signals. */
#if !GTK_CHECK_VERSION(3, 0, 0)
    g_signal_connect(G_OBJECT(c->da), "expose-event", G_CALLBACK(expose_event), (gpointer) c);
#else
    g_signal_connect(G_OBJECT(c->da), "draw", G_CALLBACK(draw), (gpointer) c);
#endif

    /* Show the widget.  Connect a timer to refresh the statistics. */
    gtk_widget_show(c->da);
    c->timer = g_timeout_add(1500, (GSourceFunc) cpu_update, (gpointer) c);
    return p;
}

/* Plugin destructor. */
static void cpu_destructor(gpointer user_data)
{
    CPUTempPlugin * c = (CPUTempPlugin *)user_data;

    /* Disconnect the timer. */
    g_source_remove(c->timer);

    /* Deallocate memory. */
    cairo_surface_destroy(c->pixmap);
    g_free(c->stats_cpu);
    g_free(c);
}

static gboolean cpu_apply_configuration (gpointer user_data)
{
	char colbuf[32];
    GtkWidget * p = user_data;
    CPUTempPlugin * c = lxpanel_plugin_get_data(p);
    config_group_set_int (c->settings, "ShowPercent", c->show_percentage);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->foreground_color));
    config_group_set_string (c->settings, "Foreground", colbuf);
    sprintf (colbuf, "%s", gdk_color_to_string (&c->background_color));
    config_group_set_string (c->settings, "Background", colbuf);
}

/* Callback when the configuration dialog is to be shown. */
static GtkWidget *cpu_configure(LXPanel *panel, GtkWidget *p)
{
    CPUTempPlugin * dc = lxpanel_plugin_get_data(p);
    return lxpanel_generic_config_dlg(_("CPU Usage"), panel,
        cpu_apply_configuration, p,
        _("Show usage as percentage"), &dc->show_percentage, CONF_TYPE_BOOL,
        _("Foreground colour"), &dc->foreground_color, CONF_TYPE_COLOR,
        _("Background colour"), &dc->background_color, CONF_TYPE_COLOR,
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
