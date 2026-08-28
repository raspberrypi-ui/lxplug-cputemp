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

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

#define PLUGIN_TITLE N_("CPU Temperature")

#define MAX_NUM_SENSORS 10

typedef gint (*GetTempFunc) (char const *);

typedef struct
{
    GtkWidget *plugin;
    GtkGesture *gesture;
    PluginGraph graph;
    guint timer;                            /* Timer for periodic update */
    int numsensors;
    char *sensor_array[MAX_NUM_SENSORS];
    GetTempFunc get_temperature[MAX_NUM_SENSORS];
    gint temperature[MAX_NUM_SENSORS];
    gboolean ispi;
    int lower_temp;                         /* Temperature of bottom of graph */
    int upper_temp;                         /* Temperature of top of graph */
    GdkRGBA foreground_colour;              /* Foreground colour for drawing area */
    GdkRGBA background_colour;              /* Background colour for drawing area */
    GdkRGBA low_throttle_colour;            /* Colour for bars with ARM freq cap */
    GdkRGBA high_throttle_colour;           /* Colour for bars with throttling */
    LXPLUG_VARS
} CPUTempPlugin;

extern conf_table_t conf_table[7];

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

extern void cputemp_init (CPUTempPlugin *up);
extern void cputemp_set_values (CPUTempPlugin *up);
extern void cputemp_update_display (CPUTempPlugin *up);
extern void cputemp_destructor (gpointer user_data);
extern void validate_temps (CPUTempPlugin *c);

/* End of file */
/*----------------------------------------------------------------------------*/
