/********************************************************************
 * gnc-html-graph-gog.c -- GNC/HTML Graphing support via GOG        *
 *                                                                  *
 * Copyright (C) 2005 Joshua Sled <jsled@asynchronous.org>          *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
 ********************************************************************/

#include "config.h"

#include <gtk/gtk.h>
#include <gtkhtml/gtkhtml.h>
#include <gtkhtml/gtkhtml-embedded.h>
#include <string.h>

#include "gnc-locale-utils.h"
#include "gnc-html-graph-gog.h"
#include "gnc-html-graph-gog-gtkhtml.h"
#include "gnc-html-graph-gog-extras.h"
#include "gnc-html.h"
#include "gnc-engine.h"
#include <goffice/goffice.h>
#include <goffice/graph/gog-chart.h>
#include <goffice/graph/gog-graph.h>
#include <goffice/graph/gog-object.h>
#include <goffice/graph/gog-renderer.h>
/* everything inside the following #ifndef can be safely removed when gnucash
requires libgoffice >= 0.7.5. */
#ifndef GOG_TYPE_GRAPH
#	define GOG_TYPE_GRAPH GOG_GRAPH_TYPE
#	define GOG_TYPE_RENDERER GOG_RENDERER_TYPE
#endif
#include <goffice/graph/gog-styled-object.h>
#include <goffice/graph/gog-plot.h>
#include <goffice/graph/gog-series.h>
#include <goffice/utils/go-color.h>
#include <goffice/utils/go-marker.h>
#include <goffice/graph/gog-data-set.h>
#include <goffice/data/go-data-simple.h>
#include <goffice/app/go-plugin.h>
#include <goffice/app/go-plugin-loader-module.h>

/**
 * TODO:
 * - scatter-plot marker selection
 * - series-color, piecharts (hard, not really supported by GOG)
 *   and scatters (or drop feature)
 * - title-string freeing (fixmes)
 * - general graph cleanup
 **/

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gnc.html.graph.gog.gtkhtml"

static int handle_piechart( GncHtml* html, gpointer eb, gpointer d );
static int handle_barchart( GncHtml* html, gpointer eb, gpointer d );
static int handle_linechart( GncHtml* html, gpointer eb, gpointer d );
static int handle_scatter( GncHtml* html, gpointer eb, gpointer d );

static void draw_print_cb(GtkHTMLEmbedded *eb, cairo_t *cr, gpointer graph);

static double * read_doubles(const char * string, int nvalues);

void
gnc_html_graph_gog_gtkhtml_init( void )
{
    gnc_html_graph_gog_init();

    gnc_html_register_object_handler( GNC_CHART_PIE, handle_piechart );
    gnc_html_register_object_handler( GNC_CHART_BAR, handle_barchart );
    gnc_html_register_object_handler( GNC_CHART_SCATTER, handle_scatter );
    gnc_html_register_object_handler( GNC_CHART_LINE, handle_linechart );
}

static double *
read_doubles(const char * string, int nvalues)
{
    int    n;
    gchar *next;
    double * retval = g_new0(double, nvalues);

    // guile is going to (puts ...) the elements of the double array
    // together. In non-POSIX locales, that will be in a format that
    // the locale-specific sscanf will not be able to parse.
    gnc_push_locale(LC_NUMERIC, "C");
    {
        for (n = 0; n < nvalues; n++)
        {
            retval[n] = strtod(string, &next);
            string = next;
        }
    }
    gnc_pop_locale(LC_NUMERIC);

    return retval;
}

static char **
read_strings(const char * string, int nvalues)
{
    int n;
    int choffset = 0;
    int accum = 0;
    char ** retval = g_new0(char *, nvalues);
    char thischar;
    const char * inptr = string;
    int escaped = FALSE;

    for (n = 0; n < nvalues; n++)
    {
        retval[n] = g_new0(char, strlen(string + accum) + 1);
        retval[n][0] = 0;
        choffset = 0;
        while ((thischar = *inptr) != 0)
        {
            if (thischar == '\\')
            {
                escaped = TRUE;
                inptr++;
            }
            else if ((thischar != ' ') || escaped)
            {
                retval[n][choffset] = thischar;
                retval[n][choffset+1] = 0;
                choffset++;
                escaped = FALSE;
                inptr++;
            }
            else
            {
                /* an unescaped space */
                escaped = FALSE;
                inptr++;
                break;
            }
        }
        accum += choffset;
        /* printf("retval[%d] = '%s'\n", n, retval[n]); */
    }

    return retval;
}

static void
add_pixbuf_graph_widget( GtkHTMLEmbedded *eb, GdkPixbuf* buf )
{
    GtkWidget *widget;
    gboolean update_status;
    GogGraph *graph = GOG_GRAPH(g_object_get_data( G_OBJECT(buf), "graph" ));

    widget = gtk_image_new_from_pixbuf (buf);
    gtk_widget_set_size_request (widget, eb->width, eb->height);
    gtk_widget_show_all (widget);
    gtk_container_add (GTK_CONTAINER (eb), widget);

    // blindly copied from gnc-html-guppi.c..
    gtk_widget_set_size_request (GTK_WIDGET (eb), eb->width, eb->height);

    g_object_set_data_full (G_OBJECT (eb), "graph", graph, g_object_unref);
    g_signal_connect (G_OBJECT (eb), "draw_print",
                      G_CALLBACK (draw_print_cb), NULL);
}

/*
 * Handle the following parameters:
 * title: text
 * subtitle: text
 * datasize: (length data), sscanf( .., %d, (int)&datasize )
 * data: (foreach (lambda (datum) (push datum) (push " ")) data)
 * colors: string; space-seperated?
 * labels: string; space-seperated?
 * slice_urls_[123]: ?
 * legend_urls_[123]: ?
 */
static gboolean
handle_piechart( GncHtml* html, gpointer eb, gpointer unused )
{
    GncHtmlPieChartInfo pieChartInfo;

    // parse data from the text-ized params.
    {
        const char *datasizeStr, *dataStr, *labelsStr, *colorStr;

        datasizeStr = gnc_html_get_embedded_param(eb, "datasize");
        dataStr = gnc_html_get_embedded_param(eb, "data" );
        labelsStr = gnc_html_get_embedded_param(eb, "labels");
        colorStr = gnc_html_get_embedded_param(eb, "colors");
        g_return_val_if_fail( datasizeStr != NULL
                              && dataStr != NULL
                              && labelsStr != NULL
                              && colorStr != NULL, FALSE );
        pieChartInfo.datasize = atoi( datasizeStr );
        pieChartInfo.data = read_doubles( dataStr, pieChartInfo.datasize );
        pieChartInfo.labels = read_strings( labelsStr, pieChartInfo.datasize );
        pieChartInfo.colors = read_strings( colorStr, pieChartInfo.datasize );
    }

    pieChartInfo.title = (const char *)gnc_html_get_embedded_param(eb, "title");
    pieChartInfo.subtitle = (const char *)gnc_html_get_embedded_param(eb, "subtitle");
    pieChartInfo.width = ((GtkHTMLEmbedded*)eb)->width;
    pieChartInfo.height = ((GtkHTMLEmbedded*)eb)->height;

    add_pixbuf_graph_widget( eb, gnc_html_graph_gog_create_piechart( &pieChartInfo ) );

    return TRUE;
}

/**
 * data_rows:int
 * data_cols:int
 * data:doubles[], data_rows*data_cols
 * x_axis_label:string
 * y_axis_label:string
 * row_labels:string[]
 * col_labels:string[]
 * col_colors:string[]
 * rotate_row_labels:boolean
 * stacked:boolean
 **/
static gboolean
handle_barchart( GncHtml* html, gpointer eb, gpointer unused )
{
    GncHtmlBarChartInfo barChartInfo;

    // parse data from the text-ized params
    // series => bars [gnc:cols]
    // series-elements => segments [gnc:rows]
    {
        const char *data_rows_str, *data_cols_str, *data_str, *col_labels_str, *row_labels_str;
        const char *col_colors_str, *rotate_row_labels_str = NULL, *stacked_str = NULL;

        data_rows_str         = gnc_html_get_embedded_param (eb, "data_rows");
        data_cols_str         = gnc_html_get_embedded_param (eb, "data_cols");
        data_str              = gnc_html_get_embedded_param (eb, "data" );
        row_labels_str        = gnc_html_get_embedded_param (eb, "row_labels");
        col_labels_str        = gnc_html_get_embedded_param (eb, "col_labels");
        col_colors_str        = gnc_html_get_embedded_param (eb, "col_colors");
        rotate_row_labels_str = gnc_html_get_embedded_param (eb, "rotate_row_labels");
        stacked_str           = gnc_html_get_embedded_param (eb, "stacked");

        barChartInfo.rotate_row_labels     = (gboolean) atoi (rotate_row_labels_str);
        barChartInfo.stacked               = (gboolean) atoi (stacked_str);

#if 0 // too strong at the moment.
        g_return_val_if_fail (data_rows_str != NULL
                              && data_cols_str != NULL
                              && data_str != NULL
                              && col_labels_str != NULL
                              && row_labels_str != NULL
                              && col_colors_str != NULL, FALSE );
#endif // 0
        barChartInfo.data_rows = atoi (data_rows_str);
        barChartInfo.data_cols = atoi (data_cols_str);
        barChartInfo.data = read_doubles (data_str, barChartInfo.data_rows * barChartInfo.data_cols);
        barChartInfo.row_labels = read_strings (row_labels_str, barChartInfo.data_rows);
        barChartInfo.col_labels = read_strings (col_labels_str, barChartInfo.data_cols);
        barChartInfo.col_colors = read_strings (col_colors_str, barChartInfo.data_cols);
    }

    barChartInfo.title = (const char *)gnc_html_get_embedded_param(eb, "title");
    barChartInfo.subtitle = (const char *)gnc_html_get_embedded_param(eb, "subtitle");
    barChartInfo.width = ((GtkHTMLEmbedded*)eb)->width;
    barChartInfo.height = ((GtkHTMLEmbedded*)eb)->height;
    barChartInfo.x_axis_label = gnc_html_get_embedded_param(eb, "x_axis_label"),
                 barChartInfo.y_axis_label = gnc_html_get_embedded_param(eb, "y_axis_label");

    add_pixbuf_graph_widget( eb, gnc_html_graph_gog_create_barchart( &barChartInfo ) );

    g_debug("barchart rendered.");
    return TRUE;
}


/**
 * data_rows:int
 * data_cols:int
 * data:doubles[], data_rows*data_cols
 * x_axis_label:string
 * y_axis_label:string
 * row_labels:string[]
 * col_labels:string[]
 * col_colors:string[]
 * rotate_row_labels:boolean
 * stacked:boolean
 * markers:boolean
 * major_grid:boolean
 * minor_grid:boolean
 **/
static gboolean
handle_linechart( GncHtml* html, gpointer eb, gpointer unused )
{
    GncHtmlLineChartInfo lineChartInfo;

    // parse data from the text-ized params
    // series => lines [gnc:cols]
    // series-elements => segments [gnc:rows]
    {
        const char *data_rows_str, *data_cols_str, *data_str, *col_labels_str, *row_labels_str;
        const char *col_colors_str, *rotate_row_labels_str = NULL, *stacked_str = NULL, *markers_str = NULL;
        const char *major_grid_str = NULL, *minor_grid_str = NULL;

        data_rows_str         = gnc_html_get_embedded_param (eb, "data_rows");
        data_cols_str         = gnc_html_get_embedded_param (eb, "data_cols");
        data_str              = gnc_html_get_embedded_param (eb, "data" );
        row_labels_str        = gnc_html_get_embedded_param (eb, "row_labels");
        col_labels_str        = gnc_html_get_embedded_param (eb, "col_labels");
        col_colors_str        = gnc_html_get_embedded_param (eb, "col_colors");
        rotate_row_labels_str = gnc_html_get_embedded_param (eb, "rotate_row_labels");
        stacked_str           = gnc_html_get_embedded_param (eb, "stacked");
        markers_str           = gnc_html_get_embedded_param (eb, "markers");
        major_grid_str        = gnc_html_get_embedded_param (eb, "major_grid");
        minor_grid_str        = gnc_html_get_embedded_param (eb, "minor_grid");

        lineChartInfo.rotate_row_labels     = (gboolean) atoi (rotate_row_labels_str);
        lineChartInfo.stacked               = (gboolean) atoi (stacked_str);
        lineChartInfo.markers               = (gboolean) atoi (markers_str);
        lineChartInfo.major_grid            = (gboolean) atoi (major_grid_str);
        lineChartInfo.minor_grid            = (gboolean) atoi (minor_grid_str);

        lineChartInfo.data_rows = atoi (data_rows_str);
        lineChartInfo.data_cols = atoi (data_cols_str);
        lineChartInfo.data = read_doubles (data_str, lineChartInfo.data_rows * lineChartInfo.data_cols);
        lineChartInfo.row_labels = read_strings (row_labels_str, lineChartInfo.data_rows);
        lineChartInfo.col_labels = read_strings (col_labels_str, lineChartInfo.data_cols);
        lineChartInfo.col_colors = read_strings (col_colors_str, lineChartInfo.data_cols);
    }

    lineChartInfo.title = (const char *)gnc_html_get_embedded_param(eb, "title");
    lineChartInfo.subtitle = (const char *)gnc_html_get_embedded_param(eb, "subtitle");
    lineChartInfo.width = ((GtkHTMLEmbedded*)eb)->width;
    lineChartInfo.height = ((GtkHTMLEmbedded*)eb)->height;
    lineChartInfo.x_axis_label = gnc_html_get_embedded_param(eb, "x_axis_label"),
                  lineChartInfo.y_axis_label = gnc_html_get_embedded_param(eb, "y_axis_label");

    add_pixbuf_graph_widget( eb, gnc_html_graph_gog_create_linechart( &lineChartInfo ) );

    g_debug("linechart rendered.");
    return TRUE;
}


static gboolean
handle_scatter( GncHtml* html, gpointer eb, gpointer unused )
{
    GncHtmlScatterPlotInfo scatterPlotInfo;

    {
        const char *datasizeStr, *xDataStr, *yDataStr;

        datasizeStr = gnc_html_get_embedded_param( eb, "datasize" );
        scatterPlotInfo.datasize = atoi( datasizeStr );

        xDataStr = gnc_html_get_embedded_param( eb, "x_data" );
        scatterPlotInfo.xData = read_doubles( xDataStr, scatterPlotInfo.datasize );

        yDataStr = gnc_html_get_embedded_param( eb, "y_data" );
        scatterPlotInfo.yData = read_doubles( yDataStr, scatterPlotInfo.datasize );

        scatterPlotInfo.marker_str = gnc_html_get_embedded_param(eb, "marker");
        scatterPlotInfo.color_str = gnc_html_get_embedded_param(eb, "color");
    }

    scatterPlotInfo.title = (const char *)gnc_html_get_embedded_param(eb, "title");
    scatterPlotInfo.subtitle = (const char *)gnc_html_get_embedded_param(eb, "subtitle");
    scatterPlotInfo.width = ((GtkHTMLEmbedded*)eb)->width;
    scatterPlotInfo.height = ((GtkHTMLEmbedded*)eb)->height;
    scatterPlotInfo.x_axis_label = gnc_html_get_embedded_param(eb, "x_axis_label"),
                    scatterPlotInfo.y_axis_label = gnc_html_get_embedded_param(eb, "y_axis_label");

    add_pixbuf_graph_widget( eb, gnc_html_graph_gog_create_scatterplot( &scatterPlotInfo ) );

    return TRUE;
}

static void
draw_print_cb(GtkHTMLEmbedded *eb, cairo_t *cr, gpointer unused)
{
    GogGraph *graph = GOG_GRAPH(g_object_get_data(G_OBJECT(eb), "graph"));
    GogRenderer *rend = g_object_new(GOG_TYPE_RENDERER, "model", graph, NULL);

    /* assuming pixel size is 0.5, cf. gtkhtml/src/htmlprinter.c */
    cairo_scale(cr, 0.5, 0.5);

    cairo_translate(cr, 0, -eb->height);

    gog_renderer_render_to_cairo(rend, cr, eb->width, eb->height);
    g_object_unref(rend);
}
