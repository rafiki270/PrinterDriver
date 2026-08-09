# Receipt DSL — the Document Tier, Fully Grown

Design (2026-08-08) for the "any receipt imaginable" description system. Extends
[api.md](api.md) §3 Tier 2.

**How to reach it (M19).** From C++, `pd::dsl::renderDocumentJson` (`dsl/pipeline.hpp`) is
parse → bind → render in one call, and `pd::dsl::applyDocumentMeta` folds a document's
`meta` into `JobOptions` under the precedence below. From every other language it is
`pd_render_document` / `pd_print_document_json` and their wrapper surfaces
`printer.renderDocument(...)` / `printer.printDocument(...)` — see [api.md](api.md) §3,
tier 2b. All three walk the same pipeline, so `pdctl print --template` and an app's
`printDocument` cannot disagree about what a document means.

## Core decisions

1. **A receipt is a serializable document, not code.** The canonical form is a plain
   data model (JSON-representable): design once, store, version, send POS→agent over
   the wire, render anywhere. Builder APIs (`addLine("x", style)`) are per-language
   sugar that construct this model.
2. **Block flow, not free canvas.** Thermal receipts are a vertical flow of blocks;
   the model is a list of blocks + named styles. No absolute positioning.
3. **Two rendering paths, one description.**
   - **Hardware path**: blocks map to ESC/POS commands (fast, crisp, printer fonts).
   - **Raster path**: the same document rasterized to an image (any TTF font, true
     italic, emoji, exact preview) and printed via the raster tier. Raster-text can be
     produced core-side (vendored font rasterizer, later) or wrapper-side (CoreText /
     Android / Skia render hook, sooner). **This also gives on-screen print preview for
     free** — render the document to an image and show it.
   The renderer picks per block: hardware where faithful, raster where the style
   demands it. Styles unsupported by the active path degrade *declared*, never
   silently (same philosophy as ConfidenceLevel).

## Block types

| Block | Params |
|---|---|
| `text` | content (UTF-8), style ref or inline style |
| `columns` | cells: `[{content, width: chars\|dots\|flex(n), align, style, wrap}]` — item/price rows, 2–4 column layouts; per-cell wrapping |
| `image` | source (raster ref/inline), width: dots\|percent, align, dither: fs\|threshold\|none |
| `qr` | payload bytes, size 1–16, ecLevel L/M/Q/H, align |
| `barcode` | symbology: upcA\|upcE\|ean13\|ean8\|code39\|itf\|codabar\|code93\|code128\|gs1_128\|pdf417\|datamatrix, height dots, moduleWidth, hri: none\|above\|below, align |
| `divider` | style: solid\|dashed\|double\|char("="), thicknessDots |
| `feed` | lines or dots |
| `cut` | partial\|full (mid-receipt allowed; final cut normally implicit via JobOptions) |
| `drawerKick` | pin, pulse |
| `raw` | escape-hatch bytes (no cuts/status inside — core owns framing) |

## TextStyle — the full parameter set

```
font:        printerA (12×24) | printerB (9×17) | named raster font ("Inter-600")
widthScale:  1–8      heightScale: 1–8          (GS ! multipliers on hardware path)
bold:        bool     italic: bool (raster path only — no hardware italic; degrades to declared fallback)
underline:   none|single|double
inverse:     bool     (white-on-black, GS B)
upsideDown:  bool     rotate90: bool (hardware-dependent; else raster)
align:       left|center|right
lineSpacingDots, letterSpacingDots
wrap:        word|char|clip|ellipsis
indentDots / marginLeftDots / marginRightDots
```

Named styles with inheritance: a document carries `styles: {h1: {...}, item: {...}}`
plus a `default`; blocks reference by name and may override inline. Media width comes
from the printer profile at render time — the document declares *content*, the renderer
fits it to 384/420/576/832 dots.

## Builder sugar (per wrapper, same model)

```swift
let receipt = Receipt()
  .style("h1", .init(bold: true, widthScale: 2, heightScale: 2, align: .center))
  .style("total", .init(bold: true, widthScale: 1, heightScale: 2))
  .line("MY RESTAURANT", style: "h1")
  .line("Order 7F3A-92C1 · Table 4")
  .divider(.dashed)
  .columns([.cell("2× Pilsner", .flex(1)), .cell("9.00", .chars(8), align: .right)])
  .columns([.cell("1× Goulash", .flex(1)), .cell("11.50", .chars(8), align: .right)])
  .divider(.solid)
  .columns([.cell("TOTAL", .flex(1), style: "total"), .cell("20.50", .chars(8), align: .right, style: "total")])
  .feed(1)
  .qr("7F3A-92C1", size: 6, ec: .m, align: .center)
  .image(logo, width: .percent(60), align: .center)
```

JSON canonical form (what travels and gets stored):

```json
{ "v": 1,
  "styles": { "h1": { "bold": true, "widthScale": 2, "heightScale": 2, "align": "center" } },
  "blocks": [
    { "text": "MY RESTAURANT", "style": "h1" },
    { "columns": [ { "content": "2× Pilsner", "width": "flex" },
                   { "content": "9.00", "width": { "chars": 8 }, "align": "right" } ] },
    { "qr": "7F3A-92C1", "size": 6, "ec": "M", "align": "center" } ] }
```

## Degradation rules (closed, declared)

Every render returns alongside the job: `renderReport: [{block, requested, delivered,
path: hardware|raster}]`. Italic on hardware path → delivered as bold-or-plain +
`degraded=true`; raster font on hardware path → raster path for that block; unsupported
barcode symbology on a given printer → raster-rendered barcode. Nothing silently
disappears — the same honesty contract as printing itself.

## Implementation phasing

1. Document model + JSON schema + hardware-path renderer in core (extends the M1
   encoder; columns/divider/styles resolution).
2. Wrapper builders (Swift/Kotlin/Dart) generating the model.
3. Raster path via wrapper-side text rendering hook; preview API.
4. Core-side raster fonts (vendored rasterizer) if/when wrapper-side proves limiting.

## Templates + parameter models

A **ReceiptTemplate** is the same document model with binding expressions; printing
binds a plain params model (JSON) into it — design once, print many:

```json
{ "v": 1, "template": true,
  "styles": { "h1": { "bold": true, "widthScale": 2, "align": "center" } },
  "blocks": [
    { "text": "{{venue.name}}", "style": "h1" },
    { "text": "Order {{order.id}} · Table {{order.table}}" },
    { "divider": "dashed" },
    { "each": "order.items", "block":
      { "columns": [ { "content": "{{qty}}× {{name}}", "width": "flex" },
                     { "content": "{{price}}", "width": { "chars": 8 }, "align": "right" } ] } },
    { "divider": "solid" },
    { "columns": [ { "content": "TOTAL", "width": "flex", "style": "total" },
                   { "content": "{{order.total}}", "width": { "chars": 8 }, "align": "right", "style": "total" } ] },
    { "if": "order.note", "block": { "text": "NOTE: {{order.note}}", "style": "bold" } },
    { "qr": "{{order.id}}", "size": 6, "align": "center" } ] }
```

Binding rules (deliberately small — logic-less, Mustache-like): `{{path.to.value}}`
substitution with escaping; `each` repeats a block per array element (scope = element);
`if`/`unless` on truthiness; missing path → declared render-report warning + empty
string, never a crash mid-receipt. No expressions, no arithmetic — the app prepares the
model; the template only lays it out.

Wrapper sugar:

```swift
let kitchen = try ReceiptTemplate.load(jsonTemplate)      // or built with the builder
let job = printer.send(kitchen, model: order.printModel,  // Encodable → JSON
                       key: "order-\(order.id)#kitchen-1") { result in ... }
```

Templates live wherever the app wants — bundled, fetched from a backend, cached by the
agent — and bind identically on every platform because binding happens in the core.

## Repeating groups (multi-line, nested) and value formatters

`each` repeats a **block list**, not just one block — so a line item can be a full
group: title, subtitle, modifier lines, amount×price row. Groups nest (modifiers under
items):

```json
{ "each": "order.items", "blocks": [
    { "text": "{{title}}", "style": "itemTitle" },
    { "if": "subtitle", "block": { "text": "{{subtitle}}", "style": "muted" } },
    { "each": "modifiers", "blocks": [ { "text": "  + {{name}}", "style": "muted" } ] },
    { "columns": [ { "content": "{{qty}} × {{unitPrice|number:2}}", "width": "flex" },
                   { "content": "{{total|money}}", "width": { "chars": 10 }, "align": "right" } ] },
    { "feed": 1 } ] }
```

**Formatters** — declarative filters after `|`, still logic-less; locale/currency come
from document meta (`"meta": { "locale": "cs-CZ", "currency": "CZK", "tz": "Europe/Prague" }`)
with per-use override:

```
{{v|number:2}}          1234.5 → "1 234,50"  (locale grouping/decimal)
{{v|money}}             → "1 234,50 Kč"      ({{v|money:EUR}} to override)
{{v|date:short}}        ISO input → "08/08/2026"  (named: short|medium|long|time|datetime)
{{v|date:HH:mm}}        pattern form
{{v|pad:8:right}}       fixed-width padding   {{v|upper}} {{v|lower}} {{v|trunc:20}}
```

Inputs stay plain JSON (numbers, ISO-8601 strings); formatting happens in the core at
bind time so every platform renders identically. Unknown formatter or unformattable
value → declared render-report warning + raw value printed, never a crash.

## Cut control (enabled by default)

The trailing cut is on by default and controllable at three levels — most specific wins:

1. `JobOptions.cut` (caller): `profile` (default) | `partial` | `full` | `none`
2. Document/template meta: `"meta": { "cut": false }` or `"cut": "full"` — a no-cut
   kitchen-summary template carries that fact itself
3. Printer profile: the native cut variant used when the above say "profile"

`{ "cut": "partial" }` blocks mid-document still work independently (multi-ticket
documents). `cut:none` ends the job after the final feed; completion fencing is
unaffected (the fence never depended on cutting — see techspec §5.3).

## Margins: whitespace before and after the print

Blank paper around the content is a first-class setting, with the same three-level
precedence as cut control (caller > document meta > profile):

```json
"meta": { "margins": { "topDots": 40, "bottomDots": 160 } }
```

- **Top margin** (`topDots` / `topMm`): fed before the first content line — tear-off
  clearance, presentation space. Default 0.
- **Bottom margin** (`bottomDots` / `bottomMm`): the *total* visible whitespace between
  the last content and the cut. Contract: the engine feeds
  `max(head_to_cutter_feed_dots, bottomDots)` — you can always ask for more space than
  the hardware minimum, never less; the blade-clearance floor
  ([capability profile media](capability-profiles.md)) is unconditional, so no margin
  setting can ever reintroduce the clipped-QR defect.
- `mm` variants convert via the profile's dpi (203 dpi ⇒ 8 dots/mm).
- Per-call override: `JobOptions.topFeedDots` / `JobOptions.bottomFeedDots` win over
  document meta.
