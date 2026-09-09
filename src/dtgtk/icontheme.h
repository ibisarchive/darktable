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
#pragma once

#include "dtgtk/paint.h"
#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

/* Ibis Archive icon sets.

   an icon set is a directory themes/icons/<name>/ holding SVG files and a
   map.txt that pairs the built-in cairo glyphs with them, one line each:

     presets = MenuHamburger
     solid_arrow:right = ChevronRight
     eye_toggle:active = VisibilityOff
     @scale 1.0

   the key is the paint function name without the dtgtk_cairo_paint_ prefix,
   optionally followed by :up, :down, :left, :right (CPF_DIRECTION_*) or
   :active (CPF_ACTIVE).  a glyph that is not in the map keeps its cairo
   drawing, so a set can be partial.  the SVG is used as a mask and painted
   with the source the caller set, so the CSS color rules keep working.

   the set in use is conf ui/icon_theme (empty = built-in glyphs); a copy in
   <configdir>/themes/icons/<name>/ wins over the installed one. */

/* paint the themed icon for `paint` into the box; returns FALSE when the
   set has no icon for it and the caller should draw the cairo glyph */
gboolean dtgtk_icon_theme_paint(DTGTKCairoPaintIconFunc paint,
                                cairo_t *cr,
                                const gint x,
                                const gint y,
                                const gint w,
                                const gint h,
                                const gint flags);

/* drop the loaded map and the rendered surfaces */
void dtgtk_icon_theme_cleanup(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
