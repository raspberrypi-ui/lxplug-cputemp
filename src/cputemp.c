/*============================================================================
Copyright (c) 2018-2025 Raspberry Pi
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
============================================================================*/

#include <locale.h>
#include <glib/gi18n.h>

#ifdef LXPLUG
#include "plugin.h"
#else
#include "lxutils.h"
#endif

#include "cputemp.h"

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

#define PROC_THERMAL_DIRECTORY      "/proc/acpi/thermal_zone/"
#define PROC_THERMAL_TEMPF          "temperature"
#define PROC_THERMAL_TRIP           "trip_points"

#define SYSFS_THERMAL_DIRECTORY     "/sys/class/thermal/"
#define SYSFS_THERMAL_SUBDIR_PREFIX "thermal_zone"
#define SYSFS_THERMAL_TEMPF         "temp"

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

conf_table_t conf_table[7] = {
    {CONF_TYPE_COLOUR,   "foreground",   N_("Foreground colour"),               NULL},
    {CONF_TYPE_COLOUR,   "background",   N_("Background colour"),               NULL},
    {CONF_TYPE_COLOUR,   "throttle_1",   N_("Colour when ARM frequency capped"),NULL},
    {CONF_TYPE_COLOUR,   "throttle_2",   N_("Colour when throttled"),           NULL},
    {CONF_TYPE_INT,      "low_temp",     N_("Lower temperature bound"),         NULL},
    {CONF_TYPE_INT,      "high_temp",    N_("Upper temperature bound"),         NULL},
    {CONF_TYPE_NONE,     NULL,           NULL,                                  NULL}
};

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static gint proc_get_temperature (char const *sensor_path);
static gint _get_reading (const char *path);
static gint sysfs_get_temperature (char const *sensor_path);
static gint hwmon_get_temperature (char const *sensor_path);
static int add_sensor (CPUTempPlugin* c, char const* sensor_path, GetTempFunc get_temp);
static gboolean try_hwmon_sensors (CPUTempPlugin* c, const char *path);
static void find_hwmon_sensors (CPUTempPlugin* c);
static void find_sensors (CPUTempPlugin* c, char const* directory, char const* subdir_prefix, GetTempFunc get_temp);
static void check_sensors (CPUTempPlugin *c);
static gint get_temperature (CPUTempPlugin *c);
static char *get_string (char *cmd);
static int get_throttle (void);
static gboolean cpu_update (CPUTempPlugin *c);
static gboolean write_config (CPUTempPlugin *c);
static void validate_temps (CPUTempPlugin *c);

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

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

static char *get_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    size_t len = 0;
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

static int get_throttle (void)
{
    char *buf;
    unsigned int val;

    buf = get_string ("vcgencmd get_throttled");
    if (!buf) return 0;
    if (sscanf (buf, "throttled=0x%x", &val) != 1) val = 0;
    g_free (buf);
    return val;
}

/* Periodic timer callback */

static gboolean cpu_update (CPUTempPlugin *c)
{
    char *buffer;
    int temp, thr;
    float ftemp;

    if (g_source_is_destroyed (g_main_current_source ())) return FALSE;

    temp = get_temperature (c);

    buffer = g_strdup_printf ("%3d°", temp);

    validate_temps (c);

    ftemp = temp;
    ftemp -= c->lower_temp;
    ftemp /= (c->upper_temp - c->lower_temp);

    thr = 0;
    if (c->ispi)
    {
        temp = get_throttle ();
        if (temp & 0x08) thr = 2;
        else if (temp & 0x02) thr = 1;
    }

    graph_new_point (&(c->graph), ftemp, thr, buffer);

    g_free (buffer);
    return TRUE;
}

static gboolean write_config (CPUTempPlugin *c)
{
#ifdef LXPLUG
    (void) c;
#else
    char *strval, *user_file;
    GKeyFile *kf;
    gsize len;

    user_file = g_build_filename (g_get_user_config_dir (), "wf-panel-pi.ini", NULL);
    kf = g_key_file_new ();
    g_key_file_load_from_file (kf, user_file, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL);

    g_key_file_set_integer (kf, "cputemp", "low_temp", c->lower_temp);
    g_key_file_set_integer (kf, "cputemp", "high_temp", c->upper_temp);

    strval = g_key_file_to_data (kf, &len, NULL);
    g_file_set_contents (user_file, strval, len, NULL);

    g_free (strval);
    g_key_file_free (kf);
    g_free (user_file);
#endif

    return FALSE;
}

static void validate_temps (CPUTempPlugin *c)
{
    int lower, upper;

    lower = c->lower_temp;
    upper = c->upper_temp;

    if (c->lower_temp < 0 || c->lower_temp > 100) c->lower_temp = 40;
    if (c->upper_temp < 0 || c->upper_temp > 150) c->upper_temp = 90;
    if (c->upper_temp <= c->lower_temp)
    {
        c->lower_temp = 40;
        c->upper_temp = 90;
    }

    if (lower != c->lower_temp || upper != c->upper_temp) g_idle_add ((GSourceFunc) write_config, (gpointer) c);
}

/*----------------------------------------------------------------------------*/
/* wf-panel plugin functions                                                  */
/*----------------------------------------------------------------------------*/

/* Handler for system config changed message from panel */
void cputemp_update_display (CPUTempPlugin *c)
{
    validate_temps (c);
    graph_reload (&(c->graph), wrap_icon_size (c), c->background_colour, c->foreground_colour,
        c->low_throttle_colour, c->high_throttle_colour);
}

void cputemp_init (CPUTempPlugin *c)
{
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

    /* Allocate icon as a child of top level */
    graph_init (&(c->graph));
    gtk_container_add (GTK_CONTAINER (c->plugin), c->graph.da);

    /* Set up button */
#ifndef LXPLUG
    c->gesture = add_long_press (c->plugin, NULL, NULL);
#endif

    /* Set up variables */
    c->ispi = is_pi ();
    
    /* Find the system thermal sensors */
    check_sensors (c);

    /* Constrain temperatures */
    validate_temps (c);

    cputemp_update_display (c);

    /* Connect a timer to refresh the statistics. */
    c->timer = g_timeout_add (1500, (GSourceFunc) cpu_update, (gpointer) c);

    /* Show the widget and return. */
    gtk_widget_show_all (c->plugin);
}

void cputemp_destructor (gpointer user_data)
{
    CPUTempPlugin *c = (CPUTempPlugin *) user_data;

#ifndef LXPLUG
    if (c->gesture) g_object_unref (c->gesture);
#endif

    graph_free (&(c->graph));
    if (c->timer) g_source_remove (c->timer);

    g_free (c);
}

/*----------------------------------------------------------------------------*/
/* LXPanel plugin functions                                                   */
/*----------------------------------------------------------------------------*/
#ifdef LXPLUG

/* Constructor */
static GtkWidget *cpu_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    CPUTempPlugin *c = g_new0 (CPUTempPlugin, 1);

    /* Allocate top level widget and set into plugin widget pointer. */
    c->panel = panel;
    c->settings = settings;
    c->plugin = gtk_event_box_new ();
    lxpanel_plugin_set_data (c->plugin, c, cputemp_destructor);

    /* Set config defaults */
    gdk_rgba_parse (&c->foreground_colour, "dark gray");
    gdk_rgba_parse (&c->background_colour, "light gray");
    gdk_rgba_parse (&c->low_throttle_colour, "orange");
    gdk_rgba_parse (&c->high_throttle_colour, "red");
    c->lower_temp = 40;
    c->upper_temp = 90;

    /* Read config */
    conf_table[0].value = (void *) &c->foreground_colour;
    conf_table[1].value = (void *) &c->background_colour;
    conf_table[2].value = (void *) &c->low_throttle_colour;
    conf_table[3].value = (void *) &c->high_throttle_colour;
    conf_table[4].value = (void *) &c->lower_temp;
    conf_table[5].value = (void *) &c->upper_temp;
    lxplug_read_settings (c->settings, conf_table);

    cputemp_init (c);

    return c->plugin;
}

/* Handler for system config changed message from panel */
static void cpu_configuration_changed (LXPanel *, GtkWidget *plugin)
{
    CPUTempPlugin *c = lxpanel_plugin_get_data (plugin);
    cputemp_update_display (c);
}

/* Apply changes from config dialog */
static gboolean cpu_apply_configuration (gpointer user_data)
{
    CPUTempPlugin *c = lxpanel_plugin_get_data (GTK_WIDGET (user_data));

    validate_temps (c);

    lxplug_write_settings (c->settings, conf_table);

    cputemp_update_display (c);
    return FALSE;
}

/* Display configuration dialog */
static GtkWidget *cpu_configure (LXPanel *panel, GtkWidget *plugin)
{
    return lxpanel_generic_config_dlg_new (_(PLUGIN_TITLE), panel,
        cpu_apply_configuration, plugin,
        conf_table);
}

int module_lxpanel_gtk_version = 1;
char module_name[] = PLUGIN_NAME;

/* Plugin descriptor */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = PLUGIN_TITLE,
    .config = cpu_configure,
    .description = N_("Display CPU temperature"),
    .new_instance = cpu_constructor,
    .reconfigure = cpu_configuration_changed,
    .gettext_package = GETTEXT_PACKAGE
};
#endif

/* End of file */
/*----------------------------------------------------------------------------*/
