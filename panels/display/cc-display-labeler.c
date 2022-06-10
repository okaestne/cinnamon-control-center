/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * cc-rr-labeler.c - Utility to label monitors to identify them
 * while they are being configured.
 *
 * Copyright 2008, Novell, Inc.
 *
 * This file is part of the Gnome Library.
 *
 * The Gnome Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * The Gnome Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Federico Mena-Quintero <federico@novell.com>
 */

#include <config.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <X11/Xproto.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <gdk/gdkx.h>

#include "cc-display-labeler.h"

struct _CcDisplayLabelerPrivate {
	CcDisplayConfig *config;

	int num_outputs;

	GdkRGBA *palette;
	GtkWidget **windows;

	GdkScreen  *screen;
	Atom        workarea_atom;
};

enum {
	PROP_0,
	PROP_CONFIG,
	PROP_LAST
};

G_DEFINE_TYPE (CcDisplayLabeler, cc_display_labeler, G_TYPE_OBJECT);

static void cc_display_labeler_finalize (GObject *object);
static void setup_from_config (CcDisplayLabeler *labeler);

static GdkFilterReturn
screen_xevent_filter (GdkXEvent      *xevent,
		      GdkEvent       *event,
		      CcDisplayLabeler *labeler)
{
	XEvent *xev;

	xev = (XEvent *) xevent;

	if (xev->type == PropertyNotify &&
	    xev->xproperty.atom == labeler->priv->workarea_atom) {
		/* update label positions */
		if (labeler->priv->windows != NULL) {
			cc_display_labeler_hide (labeler);
			cc_display_labeler_show (labeler);
		}
	}

	return GDK_FILTER_CONTINUE;
}

static void
cc_display_labeler_init (CcDisplayLabeler *labeler)
{
	GdkWindow *gdkwindow;

	labeler->priv = G_TYPE_INSTANCE_GET_PRIVATE (labeler, GNOME_TYPE_RR_LABELER, CcDisplayLabelerPrivate);

	labeler->priv->workarea_atom = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
						    "_NET_WORKAREA",
						    True);

	labeler->priv->screen = gdk_screen_get_default ();
	/* code is not really designed to handle multiple screens so *shrug* */
	gdkwindow = gdk_screen_get_root_window (labeler->priv->screen);
	gdk_window_add_filter (gdkwindow, (GdkFilterFunc) screen_xevent_filter, labeler);
	gdk_window_set_events (gdkwindow, gdk_window_get_events (gdkwindow) | GDK_PROPERTY_CHANGE_MASK);
}

static void
cc_display_labeler_set_property (GObject *gobject, guint property_id, const GValue *value, GParamSpec *param_spec)
{
	CcDisplayLabeler *self = CC_DISPLAY_LABELER (gobject);

	switch (property_id) {
	case PROP_CONFIG:
		self->priv->config = CC_DISPLAY_CONFIG (g_value_dup_object (value));
		return;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, property_id, param_spec);
	}
}

static GObject *
cc_display_labeler_constructor (GType type, guint n_construct_properties, GObjectConstructParam *construct_properties)
{
	CcDisplayLabeler *self = (CcDisplayLabeler*) G_OBJECT_CLASS (cc_display_labeler_parent_class)->constructor (type, n_construct_properties, construct_properties);

	setup_from_config (self);

	return (GObject*) self;
}

static void
cc_display_labeler_class_init (CcDisplayLabelerClass *klass)
{
	GObjectClass *object_class;

	g_type_class_add_private (klass, sizeof (CcDisplayLabelerPrivate));

	object_class = (GObjectClass *) klass;

	object_class->set_property = cc_display_labeler_set_property;
	object_class->finalize = cc_display_labeler_finalize;
	object_class->constructor = cc_display_labeler_constructor;

	g_object_class_install_property (object_class, PROP_CONFIG, g_param_spec_object ("config",
											 "Configuration",
											 "RandR configuration to label",
											 CC_TYPE_DISPLAY_CONFIG,
											 G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
											 G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
}

static void
cc_display_labeler_finalize (GObject *object)
{
	CcDisplayLabeler *labeler;
	GdkWindow      *gdkwindow;

	labeler = CC_DISPLAY_LABELER (object);

	gdkwindow = gdk_screen_get_root_window (labeler->priv->screen);
	gdk_window_remove_filter (gdkwindow, (GdkFilterFunc) screen_xevent_filter, labeler);

	if (labeler->priv->config != NULL) {
		g_object_unref (labeler->priv->config);
	}

	if (labeler->priv->windows != NULL) {
		cc_display_labeler_hide (labeler);
		g_free (labeler->priv->windows);
	}

	g_free (labeler->priv->palette);

	G_OBJECT_CLASS (cc_display_labeler_parent_class)->finalize (object);
}

static int
count_outputs (CcDisplayConfig *config)
{
	int i;
	GList *outputs = cc_display_config_get_ui_sorted_monitors (config);

	i = g_list_length (outputs);

    return i;
}

static void
make_palette (CcDisplayLabeler *labeler)
{
	/* The idea is that we go around an hue color wheel.  We want to start
	 * at red, go around to green/etc. and stop at blue.
	 *
	 */
	double start_hue;
	double end_hue;
	int i;

	g_assert (labeler->priv->num_outputs > 0);

	labeler->priv->palette = g_new (GdkRGBA, labeler->priv->num_outputs);

	start_hue = 0.0; /* red */
	end_hue   = 2.0/3; /* blue */

	for (i = 0; i < labeler->priv->num_outputs; i++) {
		double h, s, v;
		double r, g, b;

		h = start_hue + (end_hue - start_hue) / labeler->priv->num_outputs * i;
		s = 0.6;
		v = 1.0;

		gtk_hsv_to_rgb (h, s, v, &r, &g, &b);

		labeler->priv->palette[i].red   = r;
		labeler->priv->palette[i].green = g;
		labeler->priv->palette[i].blue  = b;
		labeler->priv->palette[i].alpha  = 1.0;
	}
}

static void
rounded_rectangle (cairo_t *cr,
                   gint     x,
                   gint     y,
                   gint     width,
                   gint     height,
                   gint     x_radius,
                   gint     y_radius)
{
	gint x1, x2;
	gint y1, y2;
	gint xr1, xr2;
	gint yr1, yr2;

	x1 = x;
	x2 = x1 + width;
	y1 = y;
	y2 = y1 + height;

	x_radius = MIN (x_radius, width / 2.0);
	y_radius = MIN (y_radius, width / 2.0);

	xr1 = x_radius;
	xr2 = x_radius / 2.0;
	yr1 = y_radius;
	yr2 = y_radius / 2.0;

	cairo_move_to    (cr, x1 + xr1, y1);
	cairo_line_to    (cr, x2 - xr1, y1);
	cairo_curve_to   (cr, x2 - xr2, y1, x2, y1 + yr2, x2, y1 + yr1);
	cairo_line_to    (cr, x2, y2 - yr1);
	cairo_curve_to   (cr, x2, y2 - yr2, x2 - xr2, y2, x2 - xr1, y2);
	cairo_line_to    (cr, x1 + xr1, y2);
	cairo_curve_to   (cr, x1 + xr2, y2, x1, y2 - yr2, x1, y2 - yr1);
	cairo_line_to    (cr, x1, y1 + yr1);
	cairo_curve_to   (cr, x1, y1 + yr2, x1 + xr2, y1, x1 + xr1, y1);
	cairo_close_path (cr);
}

#define LABEL_WINDOW_EDGE_THICKNESS 1
#define LABEL_WINDOW_PADDING 12
#define LABEL_CORNER_RADIUS 0
#define LABEL_WINDOW_MARGIN 5

static void
label_draw_background_and_frame (GtkWidget *widget, cairo_t *cr)
{
	GdkRGBA *rgba;
	GtkAllocation allocation;

	rgba = g_object_get_data (G_OBJECT (widget), "rgba");
	gtk_widget_get_allocation (widget, &allocation);

	cairo_save (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);

	/* edge outline */
	cairo_set_source_rgba (cr, 0, 0, 0, 1.0);

	rounded_rectangle (cr,
	                   LABEL_WINDOW_EDGE_THICKNESS / 2.0,
	                   LABEL_WINDOW_EDGE_THICKNESS / 2.0,
	                   allocation.width - LABEL_WINDOW_EDGE_THICKNESS,
	                   allocation.height - LABEL_WINDOW_EDGE_THICKNESS,
	                   LABEL_CORNER_RADIUS, LABEL_CORNER_RADIUS);
	cairo_set_line_width (cr, LABEL_WINDOW_EDGE_THICKNESS);
	cairo_stroke (cr);

	/* fill */
	rgba->alpha = 0.90;
	gdk_cairo_set_source_rgba (cr, rgba);

	rounded_rectangle (cr,
	                   LABEL_WINDOW_EDGE_THICKNESS,
	                   LABEL_WINDOW_EDGE_THICKNESS,
	                   allocation.width - LABEL_WINDOW_EDGE_THICKNESS * 2,
	                   allocation.height - LABEL_WINDOW_EDGE_THICKNESS * 2,
	                   LABEL_CORNER_RADIUS - LABEL_WINDOW_EDGE_THICKNESS / 2.0,
			   LABEL_CORNER_RADIUS - LABEL_WINDOW_EDGE_THICKNESS / 2.0);
	cairo_fill (cr);

	cairo_restore (cr);
}

static gboolean
label_window_draw_event_cb (GtkWidget *widget, cairo_t *cr, gpointer data)
{
	/* clear any content */
	cairo_save (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba (cr, 0, 0, 0, 0);
	cairo_paint (cr);
	cairo_restore (cr);

	gtk_widget_shape_combine_region (widget, NULL);
	label_draw_background_and_frame (widget, cr);

	return FALSE;
}

static void
position_window (CcDisplayLabeler  *labeler,
		 GtkWidget       *window,
		 int              x,
		 int              y)
{
	GdkRectangle    workarea;
	GdkRectangle    monitor;
	int             monitor_num, scale_factor;

    scale_factor = gtk_widget_get_scale_factor (GTK_WIDGET (window));
	monitor_num = gdk_screen_get_monitor_at_point (labeler->priv->screen, x / scale_factor, y / scale_factor);

	gdk_screen_get_monitor_workarea (labeler->priv->screen, monitor_num, &workarea);
	gdk_screen_get_monitor_geometry (labeler->priv->screen,
                                         monitor_num,
                                         &monitor);
	gdk_rectangle_intersect (&monitor, &workarea, &workarea);

	gtk_window_move (GTK_WINDOW (window), workarea.x + LABEL_WINDOW_MARGIN, workarea.y + LABEL_WINDOW_MARGIN);
}

static void
label_window_realize_cb (GtkWidget *widget)
{
	cairo_region_t *region;

	/* make the whole window ignore events */
	region = cairo_region_create ();
	gtk_widget_input_shape_combine_region (widget, region);
	cairo_region_destroy (region);

	gtk_widget_shape_combine_region (widget, NULL);
}

static void
label_window_composited_changed_cb (GtkWidget *widget, CcDisplayLabeler *labeler)
{
	if (gtk_widget_get_realized (widget))
		gtk_widget_shape_combine_region (widget, NULL);
}

static GtkWidget *
create_label_window (CcDisplayLabeler *labeler, CcDisplayMonitor *output, GdkRGBA *rgba, gint num)
{
	GtkWidget *window;
	GtkWidget *widget;
	char *str;
	const char *display_name, *output_name;
	GdkRGBA black = { 0, 0, 0, 1.0 };
	int x, y;
	GdkScreen *screen;
	GdkVisual *visual;

	window = gtk_window_new (GTK_WINDOW_POPUP);
	gtk_window_set_resizable (GTK_WINDOW (window), FALSE);
	gtk_widget_set_app_paintable (window, TRUE);
	screen = gtk_widget_get_screen (window);
	visual = gdk_screen_get_rgba_visual (screen);

	if (visual != NULL)
		gtk_widget_set_visual (window, visual);

	gtk_container_set_border_width (GTK_CONTAINER (window), LABEL_WINDOW_PADDING + LABEL_WINDOW_EDGE_THICKNESS);

	/* This is semi-dangerous.  The color is part of the labeler->palette
	 * array.  Note that in cc_display_labeler_finalize(), we are careful to
	 * free the palette only after we free the windows.
	 */
	g_object_set_data (G_OBJECT (window), "rgba", rgba);

	g_signal_connect (window, "draw",
			  G_CALLBACK (label_window_draw_event_cb), labeler);
	g_signal_connect (window, "realize",
			  G_CALLBACK (label_window_realize_cb), labeler);
	g_signal_connect (window, "composited-changed",
			  G_CALLBACK (label_window_composited_changed_cb), labeler);

	if (cc_display_config_is_cloning (labeler->priv->config)) {
		/* Translators:  this is the feature where what you see on your
		 * laptop's screen is the same as your external projector.
		 * Here, "Mirrored" is being used as an adjective.  For example,
		 * the Spanish translation could be "Pantallas en Espejo".
		 */
		display_name = _("Mirrored Displays");
		str = g_strdup_printf ("<b>%s</b>", display_name);
	}
	else {
		display_name = cc_display_monitor_get_display_name (output);
		output_name = cc_display_monitor_get_connector_name (output);
		str = g_strdup_printf ("<b>%d  %s</b>\n%s", num, display_name, output_name);
	}

	widget = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (widget), str);
	gtk_label_set_justify (GTK_LABEL (widget), GTK_JUSTIFY_CENTER);
	g_free (str);

	gtk_widget_override_color (widget, gtk_widget_get_state_flags (widget), &black);

	gtk_container_add (GTK_CONTAINER (window), widget);

	cc_display_monitor_get_geometry (output, &x, &y, NULL, NULL);
	position_window (labeler, window, x, y);

	gtk_widget_show_all (window);

	return window;
}

static void
setup_from_config (CcDisplayLabeler *labeler)
{
	labeler->priv->num_outputs = count_outputs (labeler->priv->config);

	make_palette (labeler);

	cc_display_labeler_show (labeler);
}

/**
 * cc_display_labeler_new:
 * @config: Configuration of the screens to label
 *
 * Create a GUI element that will display colored labels on each connected monitor.
 * This is useful when users are required to identify which monitor is which, e.g. for
 * for configuring multiple monitors.
 * The labels will be shown by default, use cc_display_labeler_hide to hide them.
 *
 * Returns: A new #CcDisplayLabeler
 */
CcDisplayLabeler *
cc_display_labeler_new (CcDisplayConfig *config)
{
	g_return_val_if_fail (CC_IS_DISPLAY_CONFIG (config), NULL);

	return g_object_new (GNOME_TYPE_RR_LABELER, "config", config, NULL);
}

/**
 * cc_display_labeler_show:
 * @labeler: A #CcDisplayLabeler
 *
 * Show the labels.
 */
void
cc_display_labeler_show (CcDisplayLabeler *labeler)
{
    gint i;
	gboolean created_window_for_clone;
	GList *outputs, *l;

	g_return_if_fail (GNOME_IS_RR_LABELER (labeler));

	if (labeler->priv->windows != NULL)
		return;

	labeler->priv->windows = g_new (GtkWidget *, labeler->priv->num_outputs);

	created_window_for_clone = FALSE;

	outputs = cc_display_config_get_ui_sorted_monitors (labeler->priv->config);

    for (l = outputs, i = 0; l != NULL; l = l->next, i++) {
        CcDisplayMonitor *output = CC_DISPLAY_MONITOR (l->data);

		if (!created_window_for_clone && cc_display_monitor_is_active (output)) {
			labeler->priv->windows[i] = create_label_window (labeler, output, labeler->priv->palette + i, i + 1);

			if (cc_display_config_is_cloning (labeler->priv->config))
				created_window_for_clone = TRUE;
		} else
			labeler->priv->windows[i] = NULL;
	}
}

/**
 * cc_display_labeler_hide:
 * @labeler: A #CcDisplayLabeler
 *
 * Hide ouput labels.
 */
void
cc_display_labeler_hide (CcDisplayLabeler *labeler)
{
	int i;
	CcDisplayLabelerPrivate *priv;

	g_return_if_fail (GNOME_IS_RR_LABELER (labeler));

	priv = labeler->priv;

	if (priv->windows == NULL)
		return;

	for (i = 0; i < priv->num_outputs; i++)
		if (priv->windows[i] != NULL) {
			gtk_widget_destroy (priv->windows[i]);
			priv->windows[i] = NULL;
	}
	g_free (priv->windows);
	priv->windows = NULL;
}

/**
 * cc_display_labeler_get_rgba_for_output:
 * @labeler: A #CcDisplayLabeler
 * @output: Output device (i.e. monitor) to query
 * @rgba_out: (out): Color of selected monitor.
 *
 * Get the color used for the label on a given output (monitor).
 */
void
cc_display_labeler_get_rgba_for_output (CcDisplayLabeler *labeler, CcDisplayMonitor *output, GdkRGBA *rgba_out)
{
	int i;
	GList *outputs, *l;

	g_return_if_fail (GNOME_IS_RR_LABELER (labeler));
	g_return_if_fail (CC_IS_DISPLAY_MONITOR (output));
	g_return_if_fail (rgba_out != NULL);

	outputs = cc_display_config_get_ui_sorted_monitors (labeler->priv->config);

    for (l = outputs, i = 0; l != NULL; l = l->next, i++) {
        CcDisplayMonitor *o = CC_DISPLAY_MONITOR (l->data);

		if (o == output) {
			*rgba_out = labeler->priv->palette[i];
			return;
		}
    }

	g_warning ("trying to get the color for unknown CcDisplayMonitor %p; returning magenta!", output);

	rgba_out->red   = 1.0;
	rgba_out->green = 0;
	rgba_out->blue  = 1.0;
	rgba_out->alpha  = 1.0;
}