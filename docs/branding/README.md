<!--
SPDX-FileCopyrightText: 2026 Georgios Kouros
SPDX-License-Identifier: GPL-2.0-or-later
-->

# OkulAIr branding

**Name:** OkulAIr (preserve the capital `AI`).
**Tagline:** A new lens on your documents.
**Attribution in project documentation:** Independent fork of Okular. Keep this
in the README and notices, not on the logo or wordmark.
**Plain identifier for future integration:** `okulair`.

The mark combines a gently tilted document with a teal lens. Warm ivory and
muted green keep the design approachable; simple geometry and a level wordmark
keep it restrained. There are no character faces or decorative sparkles.
Ivory `#FAF7F1` is the banner background, ink `#293C49` provides outlines and
lettering, teal `#52BCA8` colors the lens, and `#238A7A` emphasizes `AI`.
The banner uses system DejaVu Sans; no font is bundled. The icon uses only
vector shapes.

| File | Use |
| --- | --- |
| [okulair-icon.svg](okulair-icon.svg) | Editable/scalable standalone icon |
| [okulair-icon.png](okulair-icon.png) | 512 × 512 transparent-corner PNG export |
| [okulair-banner.svg](okulair-banner.svg) | Editable wordmark and tagline, without fork text |
| [okulair-banner.png](okulair-banner.png) | 1680 × 480 README banner |

Earlier vector designs, including the character exploration, are preserved in
[archive/](archive/). The active README uses the restrained design above.

These are original vector assets for this fork, not traced or derived from
upstream Okular/KDE logos. Artwork is offered under `GPL-2.0-or-later`; SVGs carry
SPDX notices and PNGs have adjacent `.license` files. Copyright permissions do not
grant another party's trademark rights or imply name clearance.

Rebuild PNGs with Inkscape:

```bash
inkscape docs/branding/okulair-icon.svg --export-type=png --export-width=512 --export-filename=docs/branding/okulair-icon.png
inkscape docs/branding/okulair-banner.svg --export-type=png --export-width=1680 --export-filename=docs/branding/okulair-banner.png
```

SVG text remains editable, so rendering can vary if DejaVu Sans is absent. Use
the PNG for predictable README display. Application icons/install rules have not
been replaced; runtime branding is a separate integration step before distributing
an OkulAIr binary.
