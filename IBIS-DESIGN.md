# Ibis Archive design plan

Reviewed with /plan-design-review on 2026-09-08 against the UI as built on
`ibis/main` (themes, identify birds panel, eBird export storage, branding).
This file is the design reference for the fork: what the surfaces are, what
was decided, and what is still open.

## What already exists (reuse, do not reinvent)

- darktable's UI kit: module headers, `dt_action_button_new`, bauhaus
  sliders/combos, `GtkEntry`, `dt_ui_section_label`,
  `dt_gui_collapsible_section`, `GtkGrid` two-column rows (as in image
  information). Every Ibis surface is built from these; nothing custom-drawn.
- One typeface app-wide, Times New Roman (`data/themes/chunk-ibis-fonts.css`).
- Two themes, `ibis-dark` (default) and `ibis-light`
  (`data/themes/ibis-*.css`): hairline borders, 3 px radii, canvases stay
  middle grey.
- Brand assets generated from the Ibis mark by `tools/ibis/make_brand_assets.py`.
- darktable's own states: job progress bar (bottom left), toast log
  (`dt_control_log`), tooltips.

## Surfaces owned by the fork

1. identify birds (lighttable, right panel) `src/libs/ibis_identify.c`
2. eBird checklist export storage (export module) `data/lua/ibis/ebird.lua`
3. Themes, branding, splash, about, window title

## Approved Mockups

| Screen/Section | Mockup Path | Direction | Notes |
|----------------|-------------|-----------|-------|
| identify birds panel | ~/.gstack/projects/ibisarchive-darktable/designs/identify-birds-panel-20260908/variant-C.png | act, then check, then settings: header, full-width primary button, one status line, review list with right-aligned percentages and a check on the current species, quiet "no bird here", collapsed "settings" disclosure with labeled rows | build from stock darktable widgets only; switch becomes darktable's toggle; "<1%" for sub-percent |

## Pass 1: Information Architecture (4/10 -> 9/10)

Decision 1A: the identify birds panel is rebuilt in the order of use, per
mockup C.

```
identify birds                                      [module header]
[ identify selected ]                               primary action, full width
Bjerkreim--Spodavoll (NO-11): 422 species . 31 tagged, 4 unsure   one status line
review: DSC03224.ARW . Red-breasted Merganser        hovered (else selected) frame
  v Red-breasted Merganser ................... 99%   candidate rows: name left,
    Common Merganser ......................... <1%   percentage right, check on
    Goosander ................................ <1%   the current species
  no bird here                                      quiet text button
> settings                                          dt_gui_collapsible_section, closed
  minimum score        [====o====] 0.50             bauhaus slider
  eBird API key        [.......................]    labeled row, masked entry
  fallback country     [NO]                         labeled row
  limit to region      [x]                          bauhaus toggle
```

Rules: labels are always visible (no placeholder-as-label); the status line
carries both the outcome of the last run and where the candidates came from;
nothing above the fold is a setting.

## Pass 2: Interaction State Coverage (3/10 -> 9/10)

Decision 2A: a first-run state and a full state table. Models are not in
the installer (1.2 GB); darktable's model registry (preferences > AI,
GitHub release assets) fetches them from an Ibis release.

```
FEATURE           | LOADING                      | EMPTY / FIRST RUN                         | ERROR                                   | SUCCESS                                  | PARTIAL
------------------|------------------------------|-------------------------------------------|-----------------------------------------|------------------------------------------|------------------------------
identify birds    | button disabled, label       | no models: status line "models not        | "AI is off in preferences" with a       | "Bjerkreim--Spodavoll (NO-11): 422       | "... 31 tagged, 4 unsure" and
panel             | "identifying 12 of 36";      | installed (1.2 GB)" + [download models]   | [open preferences] button; no key:      | species . 36 tagged"                     | a [review unsure] button that
                  | darktable job bar bottom-left| button (darktable registry); no selection:| "no eBird key: all species considered"  |                                          | filters the lighttable to
                  |                              | button disabled, status "select frames"   | (settings row highlighted)              |                                          | Birds|Species|Unidentified
review block      | -                            | "hover or select a frame"                 | -                                       | filename . species, 3 candidates, check  | species set, no candidates:
                  |                              |                                           |                                         |                                          | show species, hide rows
eBird export      | darktable export progress    | no species tags in selection: storage     | no position: written, status line says  | "checklist: 3 species, 36 frames ->      | some frames without species:
storage           |                              | panel says "0 species tagged; run         | "no position on the frames: add one in  | <folder>" toast + folder opens            | listed as "12 frames without
                  |                              | identify birds first", export disabled    | the map view"; eBird key missing: n/a   |                                          | a species (kept in Unidentified/)"
```

Every state is text the user can act on, never a folder path. Errors name
the fix and, where darktable has a screen for it, a button that opens it.

## Pass 3: User Journey and Emotional Arc (5/10 -> 9/10)

Decision 3A: a live preview in the export storage panel and a [review
unsure] button in the identify panel.

```
STEP | USER DOES                     | USER FEELS                        | SPECIFIED BY
-----|-------------------------------|-----------------------------------|------------------------------------------
1    | imports the outing folder     | "it knows where I was"            | map view / geotagging (darktable), region line
2    | selects all, identify selected| curiosity, then trust or doubt    | status line with counts; per-frame log
3    | reads "4 unsure"              | "which four?"                     | [review unsure] filters to Unidentified
4    | hovers a frame                | checking, in control              | review block: species, 3 candidates, one click to fix
5    | opens export, picks eBird     | certainty before committing       | storage preview: species, frames, date, place, position
6    | clicks export                 | done, knows where the files are   | toast with folder; folder opens
7    | uploads to eBird              | nothing to retype                 | Record Format CSV, names from eBird's own taxonomy
```

Time horizons: 5 seconds, the panel reads act / result / check; 5 minutes,
an outing goes from import to checklist without typing a species name; 5
years, corrected frames become the user's own ground truth (tags in XMP).

## Pass 4: AI Slop Risk (8/10 -> 9/10)

Classifier: APP UI. No hard rejections. Litmus: brand unmistakable (mark +
wordmark in the panel header), one visual anchor per view (the canvas),
sections have one job, no cards, no motion, no decorative shadows. Universal
rules: colors are tokens (theme CSS), no default font stack (Times New
Roman), labels visible (decision 1A). Fixes already decided: "0%" becomes
"<1%"; the two placeholder-labeled entries get visible labels.

## Pass 5: Design System Alignment (4/10 -> 9/10)

Decision 4A: `DESIGN.md` written (principles, type, color tokens for both
themes with contrast floors, shape and space, widget vocabulary, copy,
brand, don'ts). The two theme files implement it; new surfaces are
measured against it. New component in this plan: the candidate row (a
button with a left label and a right-aligned percentage). It fits the
vocabulary (GtkButton + two labels) and is documented there.

## Pass 6: Responsive and Accessibility (7/10)

Desktop-only application; panel width follows darktable's resizable side
panel, and every Ibis widget wraps or ellipsizes with a tooltip. Keyboard:
`identify selected` and `review unsure` are dt_action buttons, so they take
darktable shortcuts; candidate rows and `no bird here` are GtkButtons in
tab order. Contrast floors are in DESIGN.md and met by both themes. Touch is
out of scope for darktable. No issues found beyond what decisions 1A and 2A
already cover; moving on.

## Pass 7: Unresolved Design Decisions

```
DECISION NEEDED                                  | RESOLVED AS                                        | IF DEFERRED
-------------------------------------------------|----------------------------------------------------|----------------------------------------
where models come from on a fresh install        | 5A: GitHub release assets (ibisarchive/ibis-models)| first-run state (2A) cannot be built;
                                                 | through darktable's model registry                 | users copy 700 MB into a hidden folder
what "no bird here" does                         | removes the species and the candidates, adds       | reviewers guess whether it means
                                                 | nothing; frame is left untagged, not Unidentified  | "skip" or "unknown"
sub-percent candidates                           | shown as "<1%", never "0%"                         | three rows that all say 0% look broken
wordmark on the light theme                      | mid grey #8f8f8f pixmap, one file for both themes  | "Ibis Archive" vanishes on paper
settings disclosure default                      | closed; opens itself when a required value is      | first run hides the key field the
                                                 | missing (no key, no models)                        | error message points at
```

## NOT in scope

- Mobile or touch layouts: darktable is a desktop application.
- A web or Electron shell: rejected earlier today; GTK themes and stock
  widgets are the design surface.
- Restyling darktable's own modules beyond tokens: upstream owns them; the
  theme is the only lever, and it is enough.
- An in-app taxonomy browser or species search field: candidates plus the
  tagging module cover correction; revisit if review shows typing is needed.
- Localisation of species names: eBird's English names are what the
  checklist needs; the taxonomy carries other languages if wanted later.

## Light theme audit (done in this review)

Screenshots on 2026-09-08 of ibis-light in the lighttable, darkroom (export
module expanded) and map. Panels, module headers, bauhaus fields, entries,
tooltips and the filmstrip read correctly; canvases stay middle grey; the
wordmark is now a mid grey that reads on paper. No direct-grey rule was
found inverted wrongly in these views. Preferences and import dialogs were
not captured (no scripted way to open them); check them when they are next
open.

## Implementation Tasks
Synthesized from this review's findings. Each task derives from a specific
finding above. Run with Claude Code or Codex; checkbox as you ship.

- [ ] **T1 (P1, human: ~2h / CC: ~15min)** — identify birds panel — rebuild gui_init in mockup order: button, one status line, review block, collapsed settings with labeled rows
  - Surfaced by: Pass 1 — decision 1A
  - Files: src/libs/ibis_identify.c
  - Verify: screenshot of the panel matches variant-C.png top to bottom; labels visible with empty fields
- [ ] **T2 (P1, human: ~1.5 days / CC: ~1.5h)** — identify birds panel — first-run state: "models not installed (1.2 GB)" + [download models] through darktable's model registry; button disabled with "select frames" when nothing is selected; error states name the fix and open preferences where relevant
  - Surfaced by: Pass 2 — decision 2A, Pass 7 — decision 5A
  - Files: src/libs/ibis_identify.c, data/ai_models.json (Ibis entries with github_asset), ibisarchive/ibis-models release with classify-birds-global.dtmodel and detect-animals.dtmodel
  - Verify: fresh --configdir with an empty models folder shows the first-run state; download completes; identify runs
- [ ] **T3 (P1, human: ~4h / CC: ~30min)** — identify birds panel — [review unsure] button filters the lighttable to Birds|Species|Unidentified; status line merges outcome and candidate set
  - Surfaced by: Pass 2 table (PARTIAL), Pass 3 — decision 3A
  - Files: src/libs/ibis_identify.c (dt_collection_* query), data/darktableconfig.xml.in if a conf key is needed
  - Verify: after a run with unsure frames, one click shows only those frames
- [ ] **T4 (P1, human: ~1 day / CC: ~45min)** — eBird export storage — live preview in the storage panel: species count, frame count, date, place, position yes/no for the current selection; export disabled with "0 species tagged; run identify birds first" when nothing is tagged
  - Surfaced by: Pass 3 — decision 3A, Pass 2 table (EMPTY, ERROR)
  - Files: data/lua/ibis/ebird.lua (selection-changed event -> label refresh)
  - Verify: select 36 frames, panel reads "3 species · 36 frames · 11 Jan 2023 · Bjerkreim--Spodavoll · position: yes"
- [ ] **T5 (P2, human: ~30min / CC: ~5min)** — identify birds panel — candidate rows show "<1%" instead of "0%"; percentage right-aligned; check mark on the current species
  - Surfaced by: Pass 4, Pass 7
  - Files: src/libs/ibis_identify.c (_review_update)
  - Verify: hover a frame whose runners-up are below 1%
- [ ] **T6 (P2, human: ~1h / CC: ~10min)** — identify birds panel — settings disclosure opens itself when a required value is missing (no key, no models), so the error points at a visible field
  - Surfaced by: Pass 7 decisions table
  - Files: src/libs/ibis_identify.c
  - Verify: clear the key, run identify: settings open and the key row is highlighted
- [ ] **T7 (P3, human: ~1h / CC: ~10min)** — themes — cite DESIGN.md at the top of ibis-dark.css and ibis-light.css; check preferences and import dialogs in ibis-light when next open
  - Surfaced by: Pass 5 — decision 4A, light theme audit
  - Files: data/themes/ibis-dark.css, data/themes/ibis-light.css
  - Verify: both files reference DESIGN.md; dialog screenshots reviewed

_No new tasks from Pass 6 (Responsive and Accessibility)._

## GSTACK REVIEW REPORT

| Review | Trigger | Why | Runs | Status | Findings |
|--------|---------|-----|------|--------|----------|
| CEO Review | `/plan-ceo-review` | Scope & strategy | 0 | — | — |
| Codex Review | `/codex review` | Independent 2nd opinion | 0 | — | Codex unavailable on this machine |
| Eng Review | `/plan-eng-review` | Architecture & tests (required) | 0 | — | — |
| Design Review | `/plan-design-review` | UI/UX gaps | 1 | clean | score: 5/10 → 8/10, 5 decisions, 7 tasks |
| DX Review | `/plan-devex-review` | Developer experience gaps | 0 | — | — |

- **VERDICT:** DESIGN CLEARED — eng review required before the panel rebuild lands.

NO UNRESOLVED DECISIONS

## Whole-app flow review (added 2026-09-08 after the panel review; requested because the overall layout and setup were not convincing)

darktable ships every module for every photographer. Ibis owns three levers
that darktable exposes as data: which modules are visible per view
(`plugins/<view>/<module>_visible` conf keys, defaults in
darktableconfig.xml.in), the darkroom module-group preset (defined in
modulegroups.c, one small fork addition), and view defaults (layout,
overlays). Module order inside a panel is fixed by each module's position
number; only the Ibis module's number is ours to set.

### The birding journey, view by view

```
STEP | VIEW        | USER DOES                          | NEEDS ON SCREEN                              | TODAY
-----|-------------|------------------------------------|----------------------------------------------|----------------------------------------------
1    | lighttable  | import the card / folder            | import, collections                          | + recent collections, filters, image info (fine)
2    | lighttable  | cull: stars, reject, compare        | thumbnails with stars visible, culling mode  | overlays hidden until hover; culling via key
3    | lighttable  | identify birds on the keepers       | identify birds at the TOP of the right panel | 7th of 11 modules, between styles and neural restore
4    | lighttable  | review: hover, fix species          | review block in the same module              | present, buried
5    | map         | drop frames without GPS on the map  | map, collections                             | fine (map settings, locations rarely needed)
6    | lighttable  | export the checklist                | export with the eBird storage                | generic export at the bottom; storage picked by hand
7    | darkroom    | edit a keeper                       | exposure, crop, denoise, sharpen, color      | 60+ modules in scene-referred groups
```

Modules on the lighttable right panel today, top to bottom: selection,
actions on selection, history stack, styles, metadata editor, tagging,
geotagging, identify birds, neural restore, export. A birder uses four of
them every outing.

### Decision: the Ibis workspace (2026-09-08, whole-app flow)

Implemented through darktable's own levers, no new UI kit:

```
LEVER                         | MECHANISM                                   | IBIS SETTING
------------------------------|---------------------------------------------|------------------------------------------------------
module visibility per view    | Lua: dt.gui.libs[x].visible (workspace.lua) | lighttable right: identify birds, geotagging, tagging,
                              | applied once, user changes then persist     |   metadata editor, export, selection
                              |                                             | hidden: styles, history stack, actions on selection,
                              |                                             |   neural restore, recent collections
                              |                                             | darkroom: neural restore hidden; rest as darktable
module order                  | position() in our module                    | identify birds 900 = top of the right panel
module expanded by default    | Lua: dt.gui.libs[x].expanded                | identify birds open; tagging, geotagging, export closed
thumbnail overlays            | conf default                                | stars and labels always shown (culling reads at a glance)
darkroom module groups        | modulegroups.c preset "workflow: birds"     | basics: exposure, crop, rotate, white balance, tone
                              | + conf default                              | detail: denoise, sharpen, lens; color: color balance,
                              |                                             |   color equalizer; everything else via search
theme                         | conf default ui_last/theme                  | ibis-dark
density                       | ibis-*.css                                  | +2 px row padding, +4 px module header padding
first run                     | darktable's own welcome + lighttable        | unchanged; models state handled by identify birds (2A)
```

Everything a birder does not use stays one click away in the module
visibility menu; nothing is removed from the application.
