/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Ibis Archive: the icon rail at the right edge of the lighttable and the
   map. one button per module of the right panel; a click opens that module
   (solo mode closes the others), like the tool rail in Lightroom. the
   darkroom's rail is the module-group tabs, moved there by modulegroups.c. */

#include "common/darktable.h"
#include "control/conf.h"
#include "control/signal.h"
#include "dtgtk/expander.h"
#include "dtgtk/icontheme.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"

#include <math.h>
#include <string.h>

DT_MODULE(1)

typedef struct dt_lib_ibis_rail_t
{
  GtkWidget *box;
  gboolean syncing; // the toggles are being set from the modules' state
} dt_lib_ibis_rail_t;

/* the glyph for each right-panel module, by plugin name; unknown modules get
   Properties. names are files of the icon set (themes/icons/<set>/) */
static const struct
{
  const char *plugin;
  const char *svg;
} _icons[] = {
  { "ibis_identify", "Binoculars" },
  { "select", "SelectMulti" },
  { "metadata", "Edit" },
  { "tagging", "Tag" },
  { "geotagging", "Location" },
  { "export", "Export" },
  { "location", "Search" },
  { "map_settings", "Settings" },
  { "map_locations", "Folder" },
  { "collect", "Folder" },
  { "styles", "Effects" },
  { "image", "Image" },
  { "copy_history", "Copy" },
  { "history", "History" },
  { "metadata_view", "InfoCircle" },
  { "print_settings", "Print" },
};

const char *name(dt_lib_module_t *self)
{
  return _("rail");
}

const char *description(dt_lib_module_t *self)
{
  return _("one icon per module of the right panel;\n"
           "click to open that module");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_RAIL;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}

static const char *_svg_for(const char *plugin)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_icons); i++)
    if(!strcmp(_icons[i].plugin, plugin)) return _icons[i].svg;
  return "Properties";
}

/* a dtgtk paint function that draws the named SVG of the icon set; with no
   set loaded it leaves a dot, so the rail still shows where the buttons are */
static void _paint(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                   const gint flags, void *data)
{
  if(dtgtk_icon_theme_paint_svg((const char *)data, cr, x, y, w, h)) return;
  cairo_arc(cr, x + w / 2.0, y + h / 2.0, MIN(w, h) / 6.0, 0, 2 * M_PI);
  cairo_fill(cr);
}

static void _sync(dt_lib_module_t *self)
{
  dt_lib_ibis_rail_t *d = self->data;
  d->syncing = TRUE;
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->box));
  for(GList *l = children; l; l = g_list_next(l))
  {
    dt_lib_module_t *m = g_object_get_data(G_OBJECT(l->data), "ibis-module");
    if(m) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l->data), dt_lib_gui_get_expanded(m));
  }
  g_list_free(children);
  d->syncing = FALSE;
}

static void _toggled(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_ibis_rail_t *d = self->data;
  if(d->syncing) return;
  dt_lib_module_t *m = g_object_get_data(G_OBJECT(button), "ibis-module");
  if(!m) return;
  dt_lib_gui_set_expanded(m, gtk_toggle_button_get_active(button));
  // solo mode may have closed the others
  _sync(self);
}

static gint _by_position(gconstpointer a, gconstpointer b)
{
  const dt_lib_module_t *ma = a, *mb = b;
  // higher positions sit higher in the panel
  return mb->position(mb) - ma->position(ma);
}

static void _expanded_changed(GObject *revealer, GParamSpec *pspec, dt_lib_module_t *self);

static void _rebuild(dt_lib_module_t *self)
{
  dt_lib_ibis_rail_t *d = self->data;
  dt_gui_container_destroy_children(GTK_CONTAINER(d->box));

  const dt_view_t *view = dt_view_manager_get_current_view(darktable.view_manager);
  if(!view) return;

  GList *modules = NULL;
  for(GList *l = darktable.lib->plugins; l; l = g_list_next(l))
  {
    dt_lib_module_t *m = l->data;
    if(m == self || !m->expander || !m->expandable(m)) continue;
    if(m->container(m) != DT_UI_CONTAINER_PANEL_RIGHT_CENTER) continue;
    if(!dt_lib_is_visible_in_view(m, view) || !gtk_widget_get_visible(m->expander)) continue;
    modules = g_list_prepend(modules, m);
  }
  modules = g_list_sort(modules, _by_position);

  for(GList *l = modules; l; l = g_list_next(l))
  {
    dt_lib_module_t *m = l->data;
    GtkWidget *b = dtgtk_togglebutton_new(_paint, 0, (gpointer)_svg_for(m->plugin_name));
    gtk_widget_set_tooltip_text(b, m->name(m));
    g_object_set_data(G_OBJECT(b), "ibis-module", m);
    g_signal_connect(b, "toggled", G_CALLBACK(_toggled), self);
    gtk_box_pack_start(GTK_BOX(d->box), b, FALSE, FALSE, 0);
    GtkWidget *revealer = DTGTK_EXPANDER(m->expander)->frame;
    if(revealer)
    {
      g_signal_handlers_disconnect_by_func(revealer, G_CALLBACK(_expanded_changed), self);
      g_signal_connect(revealer, "notify::reveal-child", G_CALLBACK(_expanded_changed), self);
    }
  }
  g_list_free(modules);
  gtk_widget_show_all(d->box);
  _sync(self);
}

/* a module opened or closed from its header: the expander's revealer tells */
static void _expanded_changed(GObject *revealer, GParamSpec *pspec, dt_lib_module_t *self)
{
  _sync(self);
}

static void _view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view,
                          dt_lib_module_t *self)
{
  _rebuild(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ibis_rail_t *d = g_malloc0(sizeof(dt_lib_ibis_rail_t));
  self->data = d;
  d->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(d->box, "rail-modules");
  self->widget = d->box;
  // the other modules exist once the first view is shown; build then
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, _view_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT_ALL(self, "ibis_rail");
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
