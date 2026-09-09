# Ibis Archive: interface hierarchy

Status: approved 2026-09-08 as a three-rung plan. Rung 1 is being built;
rungs 2 and 3 are the plan for later.

## The ladder

| rung | contents | look | effort (my hours) |
|---|---|---|---|
| 1 (built) | Lightroom-measured tonal order, Lightroom sizing (10 pt, 300 px panels, 2 px gutters), Lightroom cells (name, badge, stars only when set, selection outline), flat sections, Source Sans for the interface, hierarchy tables below, solo mode, flat cells with the format badge top-right, hairline module headers, dim level-C icons, dark title bar on Windows, brand strings | about 80 percent | 10 to 12 |
| 2 (done 2026-09-09) | **done 2026-09-09:** Spectrum 2 Workflow icons through `dtgtk/icontheme.c` + `themes/icons/spectrum/map.txt` (a Spectrum-style power glyph included), version string and brand mark out of the top panel, top bar rebuilt on Lightroom CC (header row: centred search field, icon cluster, view switcher; filter row: rating, colour labels, sort with hairlines; `ui/combined_toolbar` kept as an option), Spectrum grey tokens under the dark theme, one module look in every view (no stacked chunks on an ibis theme), Lightroom-style bottom bar (layout and zoom left, rating and colour labels centre, view tools right), the right-edge icon rail (`ui/panel_rail`: module-group tabs in the darkroom, one button per right-panel module in the lighttable and the map, `src/libs/ibis_rail.c`). rung 2 is complete. | 85 to 90 percent | plus 8 to 10 |
| 3 | culling layout chrome, header bar carrying our own controls, the remaining cell details that need C (badge row layout, hover states) | 90 to 95 percent | plus 15 to 20 |

Out of reach on GTK3 at any rung: uppercase transforms, blur and shadows,
animation, and the four-panel frame itself.

## Reference palette (measured from Lightroom, 2026-09-08)

| surface | value |
|---|---|
| canvas behind photos and grid | `#1c1c1c` |
| panels, toolbars, icon rail | `#2d2d2d` |
| inset sections, filmstrip band | `#242424` |
| deepest (fields, selection wells) | `#0f0f0f` |
| hairlines | `#3f3f3f` |
| slider track | `#555555` |
| icons at rest | `#777777` |
| secondary text | `#a4a4a4` to `#bbbbbb` |
| primary text | `#f4f4f4` |
| the one accent (accepted, synced) | `#15a46e` |

Evidence: the full-app capture of `ibis/main` (both themes, 19 frames:
lighttable at rest, left open, right open, culling, darkroom at rest and
open, map, slideshow, preferences, import) in
`~/.gstack/projects/ibisarchive-darktable/designs/design-audit-20260908/screenshots/`,
and the three level maps `levelmap-lighttable.png`, `levelmap-darkroom.png`,
`levelmap-map.png` drawn over the dark frames.

## Diagnosis

Everything is rendered at one weight. Counted on the dark lighttable at
rest, with nothing hovered:

| what competes | count |
|---|---|
| module headers, each with a preset and a reset icon | 10 headers, 20 icons |
| controls in the filter bar | 15 |
| controls in the bottom toolbar | 22 |
| chrome items per grid cell (extension, stars, color dot, group icon) | 4 × 20 cells = 80 |
| rows in image information when opened | 40 |
| rules in collection filters when opened | 3, each with pin, power and options |

All of it in the same grey, the same size, the same Times. The darkroom
adds a canvas frame brighter than the panels, the lighttable's filter bar
repeated above the photo, and eight utility modules on the left. The map
adds a light pan-and-zoom control and the 40-row ledger again.

## Reference: Lightroom Classic

What we borrow, expressed in darktable's own means:

- **Module picker** top right, large, the only text in the top bar besides
  the identity plate. darktable already does this; we keep it and empty
  the rest of the top bar.
- **Solo mode**: one panel open per side at a time. darktable calls it
  "expand a single utility module at a time" (`lighttable/ui/single_module`,
  `darkroom/ui/single_module`).
- **Panel headers** as a slightly lighter strip with a disclosure triangle,
  no icons at rest. darktable's header icons stay, faded until hover.
- **Grid cells** that are the photo on a flat cell with no badges at rest
  except rating and flag. No file-extension label.
- **One toolbar under the content**, six to eight controls chosen per view.
- **Filmstrip** at the bottom in every working view.
- **Right panel = the task panels of this view only.**

What we do not copy: the accent blue, the sans, the histogram in the
library view.

## Three levels

| level | what belongs here | treatment |
|---|---|---|
| **A** | the picture and its state: photo, selection, rating, species mark | full foreground (`#dedede` dark, `#1e1c19` light); the only place color appears |
| **B** | this view's flow: the modules and controls a bird photographer touches on every session | always visible, one row each, mid tone (`#b3b3b3` / `#3d3b34`), headers 1.05em |
| **C** | everything else | dim tone (`#6e6e6e` / `#8a877f`), rises to B on hover; collapsed, or hidden from the view; at most eight C items visible at rest per view |

New tokens in both themes: `fg_a`, `fg_b`, `fg_c`; everything in the
tables maps to one of them.

## Lighttable

Flow of this view: filter, look, identify, review, rate, export.

| element | level | at rest | mechanism |
|---|---|---|---|
| frames | A | as now; selected cell one step above the table | — |
| stars, color dot on a cell | B | dim until set; set stars full tone | T |
| extension label, group and altered icons on a cell | C | hidden; shown on hover | T |
| grey band above and below the thumbnail | C | cell background = table background | T |
| view switcher | B | as now | — |
| brand mark and wordmark | B | as now; version string dimmed to C | T |
| filter bar: "filter", collection combobox, rating range, count | B | as now | — |
| filter bar: color circles, sort combobox and order | C | dim | T |
| six global icons (grouping, overlays, help, shortcuts, preferences) | C | dim, hover brightens | T |
| identify birds | B | open | already |
| export | B | collapsed; second module in the right panel | C-touch: position |
| selection, metadata editor, tagging, geotagging | C | collapsed, below export | W |
| import | C (B on an empty library) | collapsed | W |
| collections | B | collapsed | W |
| collection filters | C | collapsed; one rule (rating) instead of three | K |
| image information | C | collapsed; ten fields (filename, date, camera, lens, focal length, exposure, aperture, ISO, position, tags) | K |
| solo mode | — | on, both panels | K |
| bottom toolbar: rating and color labels | B | as now | — |
| bottom toolbar: layout, zoom, focus peaking, display profile, overlays | C | dim | T |
| scripts (Lua script manager) | C | hidden | W |
| timeline | C | hidden; the bottom panel keeps the toolbar only | W |

## Darkroom

Flow of this view: look, adjust with the birds group, compare in the filmstrip.

| element | level | at rest | mechanism |
|---|---|---|---|
| the photo | A | as now | — |
| canvas frame around the photo | C | one step above the panels (decision D1) | T |
| histogram, module groups, module search, modules | B | as now | — |
| power, instance, reset, preset icons on module headers | C | dim, hover brightens | T |
| navigation preview | B | as now | — |
| history | B | collapsed | W |
| snapshots | C | collapsed | W |
| duplicate manager, color picker, tagging, image information, mask manager, export | C | hidden from the darkroom | W |
| filter bar | C | hidden in the darkroom | W |
| bottom toolbar: exposure line | B | as now | — |
| bottom toolbar: twelve icons | C | dim | T |
| module order | C | dim header | T |
| filmstrip | B | as now | — |

## Map

Flow of this view: find where, see what was shot there, geotag a trip.

| element | level | at rest | mechanism |
|---|---|---|---|
| map and pins | A | as now | — |
| pan-and-zoom control | C | off; wheel and drag do the same | K |
| scale and coordinates | C | dim | T |
| find location | B | open | W |
| geotagging | B | collapsed | W |
| map settings | C | collapsed | W |
| tagging | C | hidden from the map | W |
| collection filters, image information | C | hidden from the map | W |
| collections | B | collapsed | W |
| filter bar | C | hidden in the map | W |
| filmstrip | B | as now | — |

## Other surfaces

- **Slideshow**: the photo alone. Nothing to change.
- **Preferences**: title reads "darktable preferences"; rename to
  "Ibis Archive preferences" (one string, C-touch). The rest is standard.
- **Import dialog**: standard GTK; leave.
- **Print, tethering**: not built on Windows; unchanged elsewhere.

## Mechanisms

T = theme CSS (`data/themes/ibis-*.css`), W = workspace script
(`data/lua/ibis/workspace.lua`), K = conf default
(`data/darktableconfig.xml.in`), C-touch = a documented one-line change in
core (`README-IBIS.md` lists them).

The pass needs two C-touches: the export module's position so it sits
under identify birds, and the preferences title. Everything else is theme,
workspace and defaults.

## Decisions

- **D1 darkroom frame, dark theme.** Decided: follow Lightroom, canvas
  `#1c1c1c`, the darkest surface on screen. darktable's middle-grey theme
  stays selectable for color judgment. The light theme keeps `#777`.
- **D2 solo mode on by default.** Recommendation: yes, both side panels.
  Ctrl+click on a header still opens several.
- **D3 stars at rest.** Dim stars always visible (recommended, culling
  needs them) or hover-only like Lightroom's compact cells.
- **D4 filter bar in darkroom and map.** Decided: hidden.
- **D5 type.** Decided: Source Sans 3 (OFL, bundled in `data/ibis/fonts`)
  for the interface, Times New Roman for the brand voice: wordmark and view
  switcher.

## Effort and verification

Prototype of the dark lighttable to check the direction: 30 minutes.
Full pass over the three views, both themes: two to three hours, plus
half an hour for the two C-touches. Verification is the same capture
harness; the acceptance check is the count of C items visible at rest per
view (target eight or fewer) and the 4.5:1 contrast floor on B text.
