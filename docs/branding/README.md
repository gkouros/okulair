<!--
SPDX-FileCopyrightText: 2026 Georgios Kouros
SPDX-License-Identifier: GPL-2.0-or-later
-->

# OkulAIr branding

**Name:** OkulAIr (preserve the capital `AI`).
**Tagline:** A new lens on your documents.
**Attribution:** Independent fork of Okular.

The logo shows a white document beneath a teal magnifying glass, with enlarged
lines and three sparkles in the lens. The banner retains the wordmark and
tagline on a single warm ivory background.

| File | Use |
| --- | --- |
| [okulair-icon.png](okulair-icon.png) | 512 × 512 transparent-background logo |
| [okulair-icon.svg](okulair-icon.svg) | SVG wrapper embedding the PNG artwork |
| [okulair-banner.svg](okulair-banner.svg) | Banner with editable text and embedded logo |
| [okulair-banner.png](okulair-banner.png) | 1680 × 480 README banner |

The logo is raster artwork embedded in the SVG files; it is not an editable
vector drawing. Earlier vector designs are preserved in [archive/](archive/).
The application icon files in `icons/` use the same artwork; existing Okular
icon names are retained for compatibility.

The banner SVG embeds the raster logo and uses DejaVu Sans for its text. Some SVG renderers may not support embedded images; use the PNG for predictable display. To rebuild the PNG, render the SVG with a renderer that supports embedded PNG images:

```bash
inkscape docs/branding/okulair-banner.svg --export-type=png --export-width=1680 --export-filename=docs/branding/okulair-banner.png
```

Artwork is offered under `GPL-2.0-or-later`; software license permissions do not
grant trademark rights or imply name clearance.
