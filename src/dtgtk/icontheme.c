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

#include "dtgtk/icontheme.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/utility.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <librsvg/rsvg.h>
#include <math.h>
#include <string.h>

/* variants of one glyph: the base drawing plus the four directions and the
   active state, in the order the map keys use */
typedef enum dt_icon_variant_t
{
  ICON_BASE = 0,
  ICON_UP,
  ICON_DOWN,
  ICON_LEFT,
  ICON_RIGHT,
  ICON_ACTIVE,
  ICON_VARIANTS
} dt_icon_variant_t;

static const char *_variant_names[ICON_VARIANTS]
  = { "", "up", "down", "left", "right", "active" };

typedef struct dt_icon_entry_t
{
  gchar *svg[ICON_VARIANTS]; // SVG basename per variant, NULL when unmapped
} dt_icon_entry_t;

typedef struct dt_icon_theme_t
{
  gboolean tried;       // init ran, even if it found no set
  gchar *dir;           // directory holding map.txt and the SVGs
  double scale;         // glyph size relative to the box
  GHashTable *map;      // paint function pointer -> dt_icon_entry_t
  GHashTable *surfaces; // "<svg>@<px>" -> cairo_surface_t
} dt_icon_theme_t;

static dt_icon_theme_t _theme = { 0 };

/* the built-in glyphs by name; generated from paint.h */
static const struct
{
  const char *name;
  DTGTKCairoPaintIconFunc paint;
} _paints[] = {
  { "empty", dtgtk_cairo_paint_empty },
  { "triangle", dtgtk_cairo_paint_triangle },
  { "solid_triangle", dtgtk_cairo_paint_solid_triangle },
  { "arrow", dtgtk_cairo_paint_arrow },
  { "solid_arrow", dtgtk_cairo_paint_solid_arrow },
  { "line_arrow", dtgtk_cairo_paint_line_arrow },
  { "sortby", dtgtk_cairo_paint_sortby },
  { "store", dtgtk_cairo_paint_store },
  { "reset", dtgtk_cairo_paint_reset },
  { "presets", dtgtk_cairo_paint_presets },
  { "flip", dtgtk_cairo_paint_flip },
  { "switch", dtgtk_cairo_paint_switch },
  { "switch_inactive", dtgtk_cairo_paint_switch_inactive },
  { "switch_on", dtgtk_cairo_paint_switch_on },
  { "switch_off", dtgtk_cairo_paint_switch_off },
  { "switch_deprecated", dtgtk_cairo_paint_switch_deprecated },
  { "plusminus", dtgtk_cairo_paint_plusminus },
  { "plus", dtgtk_cairo_paint_plus },
  { "square_plus", dtgtk_cairo_paint_square_plus },
  { "sorting", dtgtk_cairo_paint_sorting },
  { "plus_simple", dtgtk_cairo_paint_plus_simple },
  { "minus_simple", dtgtk_cairo_paint_minus_simple },
  { "multiply_small", dtgtk_cairo_paint_multiply_small },
  { "treelist", dtgtk_cairo_paint_treelist },
  { "invert", dtgtk_cairo_paint_invert },
  { "color", dtgtk_cairo_paint_color },
  { "eye", dtgtk_cairo_paint_eye },
  { "eye_toggle", dtgtk_cairo_paint_eye_toggle },
  { "timer", dtgtk_cairo_paint_timer },
  { "filmstrip", dtgtk_cairo_paint_filmstrip },
  { "directory", dtgtk_cairo_paint_directory },
  { "refresh", dtgtk_cairo_paint_refresh },
  { "perspective", dtgtk_cairo_paint_perspective },
  { "structure", dtgtk_cairo_paint_structure },
  { "draw_structure", dtgtk_cairo_paint_draw_structure },
  { "cancel", dtgtk_cairo_paint_cancel },
  { "aspectflip", dtgtk_cairo_paint_aspectflip },
  { "label", dtgtk_cairo_paint_label },
  { "label_sel", dtgtk_cairo_paint_label_sel },
  { "local_copy", dtgtk_cairo_paint_local_copy },
  { "reject", dtgtk_cairo_paint_reject },
  { "remove", dtgtk_cairo_paint_remove },
  { "star", dtgtk_cairo_paint_star },
  { "unratestar", dtgtk_cairo_paint_unratestar },
  { "altered", dtgtk_cairo_paint_altered },
  { "tags", dtgtk_cairo_paint_tags },
  { "audio", dtgtk_cairo_paint_audio },
  { "label_flower", dtgtk_cairo_paint_label_flower },
  { "colorpicker", dtgtk_cairo_paint_colorpicker },
  { "colorpicker_set_values", dtgtk_cairo_paint_colorpicker_set_values },
  { "showmask", dtgtk_cairo_paint_showmask },
  { "alignment", dtgtk_cairo_paint_alignment },
  { "text_label", dtgtk_cairo_paint_text_label },
  { "messages", dtgtk_cairo_paint_messages },
  { "styles", dtgtk_cairo_paint_styles },
  { "help", dtgtk_cairo_paint_help },
  { "info", dtgtk_cairo_paint_info },
  { "grouping", dtgtk_cairo_paint_grouping },
  { "preferences", dtgtk_cairo_paint_preferences },
  { "overlays", dtgtk_cairo_paint_overlays },
  { "intersection", dtgtk_cairo_paint_intersection },
  { "union", dtgtk_cairo_paint_union },
  { "andnot", dtgtk_cairo_paint_andnot },
  { "dropdown", dtgtk_cairo_paint_dropdown },
  { "bracket", dtgtk_cairo_paint_bracket },
  { "lock", dtgtk_cairo_paint_lock },
  { "check_mark", dtgtk_cairo_paint_check_mark },
  { "overexposed", dtgtk_cairo_paint_overexposed },
  { "rawoverexposed", dtgtk_cairo_paint_rawoverexposed },
  { "bulb", dtgtk_cairo_paint_bulb },
  { "bulb_mod", dtgtk_cairo_paint_bulb_mod },
  { "warning", dtgtk_cairo_paint_warning },
  { "softproof", dtgtk_cairo_paint_softproof },
  { "display", dtgtk_cairo_paint_display },
  { "display2", dtgtk_cairo_paint_display2 },
  { "rect_landscape", dtgtk_cairo_paint_rect_landscape },
  { "rect_portrait", dtgtk_cairo_paint_rect_portrait },
  { "polygon", dtgtk_cairo_paint_polygon },
  { "zoom", dtgtk_cairo_paint_zoom },
  { "multiinstance", dtgtk_cairo_paint_multiinstance },
  { "grid", dtgtk_cairo_paint_grid },
  { "focus_peaking", dtgtk_cairo_paint_focus_peaking },
  { "camera", dtgtk_cairo_paint_camera },
  { "histogram_scope", dtgtk_cairo_paint_histogram_scope },
  { "waveform_scope", dtgtk_cairo_paint_waveform_scope },
  { "vectorscope", dtgtk_cairo_paint_vectorscope },
  { "split_waveform_vectorscope", dtgtk_cairo_paint_split_waveform_vectorscope },
  { "linear_scale", dtgtk_cairo_paint_linear_scale },
  { "logarithmic_scale", dtgtk_cairo_paint_logarithmic_scale },
  { "waveform_overlaid", dtgtk_cairo_paint_waveform_overlaid },
  { "rgb_parade", dtgtk_cairo_paint_rgb_parade },
  { "luv", dtgtk_cairo_paint_luv },
  { "jzazbz", dtgtk_cairo_paint_jzazbz },
  { "ryb", dtgtk_cairo_paint_ryb },
  { "color_harmony", dtgtk_cairo_paint_color_harmony },
  { "clock", dtgtk_cairo_paint_clock },
  { "modulegroup_active", dtgtk_cairo_paint_modulegroup_active },
  { "modulegroup_favorites", dtgtk_cairo_paint_modulegroup_favorites },
  { "modulegroup_basics", dtgtk_cairo_paint_modulegroup_basics },
  { "modulegroup_basic", dtgtk_cairo_paint_modulegroup_basic },
  { "modulegroup_tone", dtgtk_cairo_paint_modulegroup_tone },
  { "modulegroup_color", dtgtk_cairo_paint_modulegroup_color },
  { "modulegroup_correct", dtgtk_cairo_paint_modulegroup_correct },
  { "modulegroup_effect", dtgtk_cairo_paint_modulegroup_effect },
  { "modulegroup_grading", dtgtk_cairo_paint_modulegroup_grading },
  { "modulegroup_technical", dtgtk_cairo_paint_modulegroup_technical },
  { "map_pin", dtgtk_cairo_paint_map_pin },
  { "masks_eye", dtgtk_cairo_paint_masks_eye },
  { "masks_circle", dtgtk_cairo_paint_masks_circle },
  { "masks_ellipse", dtgtk_cairo_paint_masks_ellipse },
  { "masks_gradient", dtgtk_cairo_paint_masks_gradient },
  { "masks_path", dtgtk_cairo_paint_masks_path },
  { "masks_uniform", dtgtk_cairo_paint_masks_uniform },
  { "masks_drawn", dtgtk_cairo_paint_masks_drawn },
  { "masks_parametric", dtgtk_cairo_paint_masks_parametric },
  { "masks_drawn_and_parametric", dtgtk_cairo_paint_masks_drawn_and_parametric },
  { "masks_raster", dtgtk_cairo_paint_masks_raster },
  { "masks_brush", dtgtk_cairo_paint_masks_brush },
  { "masks_vertgradient", dtgtk_cairo_paint_masks_vertgradient },
  { "masks_brush_and_inverse", dtgtk_cairo_paint_masks_brush_and_inverse },
  { "masks_object", dtgtk_cairo_paint_masks_object },
  { "masks_multi", dtgtk_cairo_paint_masks_multi },
  { "masks_inverse", dtgtk_cairo_paint_masks_inverse },
  { "masks_union", dtgtk_cairo_paint_masks_union },
  { "masks_intersection", dtgtk_cairo_paint_masks_intersection },
  { "masks_difference", dtgtk_cairo_paint_masks_difference },
  { "masks_sum", dtgtk_cairo_paint_masks_sum },
  { "masks_exclusion", dtgtk_cairo_paint_masks_exclusion },
  { "masks_used", dtgtk_cairo_paint_masks_used },
  { "tool_clone", dtgtk_cairo_paint_tool_clone },
  { "tool_heal", dtgtk_cairo_paint_tool_heal },
  { "tool_fill", dtgtk_cairo_paint_tool_fill },
  { "tool_blur", dtgtk_cairo_paint_tool_blur },
  { "paste_forms", dtgtk_cairo_paint_paste_forms },
  { "cut_forms", dtgtk_cairo_paint_cut_forms },
  { "display_wavelet_scale", dtgtk_cairo_paint_display_wavelet_scale },
  { "auto_levels", dtgtk_cairo_paint_auto_levels },
  { "compass_star", dtgtk_cairo_paint_compass_star },
  { "wand", dtgtk_cairo_paint_wand },
  { "lt_mode_grid", dtgtk_cairo_paint_lt_mode_grid },
  { "lt_mode_zoom", dtgtk_cairo_paint_lt_mode_zoom },
  { "lt_mode_culling_fixed", dtgtk_cairo_paint_lt_mode_culling_fixed },
  { "lt_mode_culling_dynamic", dtgtk_cairo_paint_lt_mode_culling_dynamic },
  { "lt_mode_fullpreview", dtgtk_cairo_paint_lt_mode_fullpreview },
  { "link", dtgtk_cairo_paint_link },
  { "shortcut", dtgtk_cairo_paint_shortcut },
  { "pin", dtgtk_cairo_paint_pin },
  { "filtering_menu", dtgtk_cairo_paint_filtering_menu },
  { "snapshots_restore", dtgtk_cairo_paint_snapshots_restore },
};

static void _entry_free(gpointer p)
{
  dt_icon_entry_t *e = p;
  for(int i = 0; i < ICON_VARIANTS; i++) g_free(e->svg[i]);
  g_free(e);
}

static void _surface_free(gpointer p)
{
  cairo_surface_destroy((cairo_surface_t *)p);
}

/* on Windows a function pointer taken inside another DLL points at an import
   thunk (jmp [rip+disp32]), not at the function itself; follow it so the
   buttons made by the plugin DLLs match the table built in the core */
static DTGTKCairoPaintIconFunc _canonical(DTGTKCairoPaintIconFunc fn)
{
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
  for(int hop = 0; hop < 2 && fn; hop++)
  {
    const guint8 *p = (const guint8 *)(gpointer)fn;
    if(p[0] != 0xFF || p[1] != 0x25) break;
    gint32 disp;
    memcpy(&disp, p + 2, sizeof(disp));
    gpointer *slot = (gpointer *)(p + 6 + disp);
    fn = (DTGTKCairoPaintIconFunc)*slot;
  }
#endif
  return fn;
}

static DTGTKCairoPaintIconFunc _paint_by_name(const char *name)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_paints); i++)
    if(!strcmp(_paints[i].name, name)) return _canonical(_paints[i].paint);
  return NULL;
}

/* find themes/icons/<set>/map.txt in the config dir, then the data dir */
static gchar *_find_set_dir(const char *set)
{
  char confdir[PATH_MAX] = { 0 };
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  const char *roots[] = { confdir, datadir };
  for(int i = 0; i < 2; i++)
  {
    gchar *dir = g_build_filename(roots[i], "themes", "icons", set, NULL);
    gchar *map = g_build_filename(dir, "map.txt", NULL);
    const gboolean found = g_file_test(map, G_FILE_TEST_IS_REGULAR);
    g_free(map);
    if(found) return dir;
    g_free(dir);
  }
  return NULL;
}

static void _parse_map(const char *path)
{
  gchar *content = NULL;
  if(!g_file_get_contents(path, &content, NULL, NULL)) return;

  gchar **lines = g_strsplit(content, "\n", -1);
  for(gchar **l = lines; *l; l++)
  {
    gchar *line = g_strstrip(*l);
    if(!*line || *line == '#') continue;

    if(g_str_has_prefix(line, "@scale"))
    {
      const double s = g_ascii_strtod(line + 6, NULL);
      if(s > 0.2 && s < 3.0) _theme.scale = s;
      continue;
    }

    gchar **kv = g_strsplit(line, "=", 2);
    if(kv[0] && kv[1])
    {
      gchar *key = g_strstrip(kv[0]);
      gchar *svg = g_strstrip(kv[1]);
      gchar *colon = strchr(key, ':');
      int variant = ICON_BASE;
      if(colon)
      {
        *colon = '\0';
        variant = -1;
        for(int i = 1; i < ICON_VARIANTS; i++)
          if(!strcmp(colon + 1, _variant_names[i])) variant = i;
      }
      DTGTKCairoPaintIconFunc paint = _paint_by_name(key);
      if(paint && variant >= 0 && *svg)
      {
        dt_icon_entry_t *e = g_hash_table_lookup(_theme.map, paint);
        if(!e)
        {
          e = g_malloc0(sizeof(dt_icon_entry_t));
          g_hash_table_insert(_theme.map, (gpointer)paint, e);
        }
        g_free(e->svg[variant]);
        e->svg[variant] = g_strdup(svg);
      }
      else
        dt_print(DT_DEBUG_ALWAYS, "[icon theme] ignoring map line '%s'", line);
    }
    g_strfreev(kv);
  }
  g_strfreev(lines);
  g_free(content);
}

static void _init(void)
{
  _theme.tried = TRUE;
  _theme.scale = 1.0;

  gchar *set = dt_conf_get_string("ui/icon_theme");
  if(!set || !*set)
  {
    g_free(set);
    return;
  }

  _theme.dir = _find_set_dir(set);
  if(!_theme.dir)
  {
    dt_print(DT_DEBUG_ALWAYS, "[icon theme] no icon set '%s' under themes/icons", set);
    g_free(set);
    return;
  }

  _theme.map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _entry_free);
  _theme.surfaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _surface_free);

  gchar *map = g_build_filename(_theme.dir, "map.txt", NULL);
  _parse_map(map);
  g_free(map);

  dt_print(DT_DEBUG_ALWAYS, "[icon theme] '%s': %d glyphs mapped from %s",
           set, g_hash_table_size(_theme.map), _theme.dir);
  g_free(set);
}

/* render the SVG once per pixel size; the surface carries the device scale
   so hi-dpi screens get a crisp mask */
static cairo_surface_t *_get_surface(const char *svg, const int px)
{
  gchar *key = g_strdup_printf("%s@%d", svg, px);
  cairo_surface_t *surface = g_hash_table_lookup(_theme.surfaces, key);
  if(surface)
  {
    g_free(key);
    return surface;
  }

  const double ppd = darktable.gui ? darktable.gui->ppd : 1.0;
  const int dev = (int)ceil(px * ppd);
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dev, dev);
  cairo_surface_set_device_scale(surface, ppd, ppd);

  gchar *file = g_strdup_printf("%s.svg", svg);
  gchar *path = g_build_filename(_theme.dir, file, NULL);
  GError *error = NULL;
  RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
  if(handle)
  {
    cairo_t *cr = cairo_create(surface);
    dt_render_svg(handle, cr, px, px, 0, 0);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    g_object_unref(handle);
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[icon theme] can't load '%s': %s",
             path, error ? error->message : "unknown error");
    g_clear_error(&error);
  }
  g_free(path);
  g_free(file);

  g_hash_table_insert(_theme.surfaces, key, surface);
  return surface;
}

gboolean dtgtk_icon_theme_paint(DTGTKCairoPaintIconFunc paint,
                                cairo_t *cr,
                                const gint x,
                                const gint y,
                                const gint w,
                                const gint h,
                                const gint flags)
{
  if(!_theme.tried) _init();
  if(!_theme.map || !paint) return FALSE;

  const dt_icon_entry_t *e = g_hash_table_lookup(_theme.map, (gpointer)_canonical(paint));
  if(!e) return FALSE;

  /* the most specific variant that is mapped wins; a missing direction
     or active state falls back to the base drawing */
  const char *svg = NULL;
  if((flags & CPF_ACTIVE) && e->svg[ICON_ACTIVE]) svg = e->svg[ICON_ACTIVE];
  else if((flags & CPF_DIRECTION_UP) && e->svg[ICON_UP]) svg = e->svg[ICON_UP];
  else if((flags & CPF_DIRECTION_DOWN) && e->svg[ICON_DOWN]) svg = e->svg[ICON_DOWN];
  else if((flags & CPF_DIRECTION_LEFT) && e->svg[ICON_LEFT]) svg = e->svg[ICON_LEFT];
  else if((flags & CPF_DIRECTION_RIGHT) && e->svg[ICON_RIGHT]) svg = e->svg[ICON_RIGHT];
  else svg = e->svg[ICON_BASE];
  if(!svg) return FALSE;

  const int px = MAX(1, (int)lround(MIN(w, h) * _theme.scale));
  cairo_surface_t *surface = _get_surface(svg, px);
  if(!surface) return FALSE;

  cairo_save(cr);
  cairo_mask_surface(cr, surface, x + (w - px) / 2.0, y + (h - px) / 2.0);
  cairo_restore(cr);
  return TRUE;
}

void dtgtk_icon_theme_cleanup(void)
{
  if(_theme.map) g_hash_table_destroy(_theme.map);
  if(_theme.surfaces) g_hash_table_destroy(_theme.surfaces);
  g_free(_theme.dir);
  memset(&_theme, 0, sizeof(_theme));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
