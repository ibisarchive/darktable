# Ibis Archive design system

One page. Every Ibis surface is measured against it. darktable's own
modules are the reference implementation of the widget vocabulary; Ibis
changes how they look (tokens) and adds surfaces built from the same kit.

## Principles

1. The picture is the anchor. Panels are quiet; canvases stay middle grey.
2. Two tones, like the mark: ink and paper. Color appears only where
   darktable already uses it for meaning (color labels, stars, histogram).
3. Utility language: orientation, status, action. Lowercase, no exclamation
   marks, no folder paths in user-facing text, every error names its fix.
4. Stock widgets only. If darktable does not have the widget, the design
   changes, not the toolkit.
5. Act, then check, then settings: a module reads in the order it is used.

## Type

- One family everywhere: Times New Roman; fallbacks Liberation Serif,
  Tinos, Nimbus Roman, serif (`data/themes/chunk-ibis-fonts.css`).
- Sizes come from darktable's scale (base 1em ≈ 13 px at 100% dpi): body
  1em, module headers 1.05em, status lines 1em, the version string as
  darktable sets it. Never below 0.85em for text a user must read.
- No bold for emphasis; Times renders it heavy. Emphasis is position and
  size, or the check mark.

## Color tokens

Set in `data/themes/ibis-dark.css` and `ibis-light.css` as
`@define-color`. The grey ramp is mirrored between the two themes so any
darktable rule that names a grey inverts with it.

| Role                 | dark      | light     |
|----------------------|-----------|-----------|
| bg_color (panels)    | `#141414` | `#efede9` |
| plugin_bg_color      | `#1c1c1c` | `#f7f6f3` |
| collapsible_bg_color | `#242424` | `#e6e4df` |
| border_color         | `#2d2d2d` | `#cfccc5` |
| fg_color (text)      | `#dedede` | `#1e1c19` |
| plugin_label_color   | `#b3b3b3` | `#3d3b34` |
| button_bg            | `#242424` | `#ffffff` |
| button_hover_bg      | `#dedede` | `#1e1c19` |
| field_bg             | `#0b0b0b` | `#ffffff` |
| darkroom canvas      | `#777777` | `#777777` |
| lighttable canvas    | `#383838` | `#8a8a8a` |
| wordmark (pixmap)    | `#8f8f8f` | `#8f8f8f` |

Contrast floor: 4.5:1 for text against its panel in both themes (dark
`#dedede`/`#141414` = 13:1; light `#1e1c19`/`#efede9` = 14:1; secondary
`#b3b3b3`/`#141414` = 8:1, `#3d3b34`/`#efede9` = 9:1).

## Shape and space

- Borders 1 px, radii 3 px, everywhere darktable draws a box.
- Spacing follows darktable's `DT_PIXEL_APPLY_DPI` steps; module content
  padding as darktable's; one blank row (4 px) between groups inside a
  module, never decorative dividers except the collapsible section header.
- No shadows, no gradients, no icons in circles, no cards.

## Widget vocabulary (allowed)

module header, `dt_action_button_new` (actions), `GtkButton` with a
left/right label pair (candidate rows), bauhaus slider / combobox /
toggle, `GtkEntry` with a visible label in a two-column `GtkGrid`,
`dt_ui_section_label`, `dt_gui_collapsible_section`, `GtkLabel` with
ellipsis and tooltip for long status. Disclosure closed by default for
settings.

## Copy

- Status lines: `<place> (<region>): <n> species · <n> tagged, <n> unsure`
- Errors: what is missing, then the fix: `no eBird key: all species considered`
- Buttons are verbs: `identify selected`, `review unsure`, `download models`,
  `no bird here`
- Percentages: integers, `<1%` below one.

## Brand

- Mark: white bird on black square (`data/ibis/brand/ibis-mark-square.png`);
  black and white marks for print. Never recolored, never stretched.
- Wordmark: "Ibis Archive" set in Times New Roman, rendered to pixmaps by
  `tools/ibis/make_brand_assets.py`; mid grey `#8f8f8f` so it reads on both
  themes.
- Application name: Ibis Archive. Lowercase only inside the interface, as
  darktable does with its own strings.

## Don't

- No purple, no gradients, no emoji, no centered blocks of text.
- No placeholder text as the only label.
- No path or code identifier in a message a user must act on.
- No new widget when a darktable one exists.
