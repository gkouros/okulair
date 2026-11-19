<!--
SPDX-FileCopyrightText: 2026 Georgios Kouros
SPDX-License-Identifier: GPL-2.0-or-later
Fork README introduced 2026-11-17. Original preserved in README.upstream.md.
-->

<p align="center">
  <img src="docs/branding/okulair-banner.png" alt="OkulAIr — A new lens on your documents. A document viewed through a teal lens." width="840">
</p>

# OkulAIr

**A new lens on your documents.**

OkulAIr is an **independent fork of [Okular](https://okular.kde.org/)**, the
universal document viewer developed by the KDE community. It builds on Okular's
document viewing and annotation capabilities, with the goal of adding persistent,
document-grounded AI assistance for scientific papers and other documents.

**This is not an official KDE project or an official Okular release.** OkulAIr is
not affiliated with or endorsed by KDE or the upstream Okular maintainers.
Credit for the existing viewer, backends, annotations, and other inherited work
belongs to the upstream authors and contributors. See [NOTICE](NOTICE) and the
preserved [upstream README](README.upstream.md).

## Project status

**Experimental desktop prototype.** A native AI sidebar now supports streaming
tabbed chat, paper overviews, figure questions, background reading cues, and PDF-embedded conversations.
The application still retains upstream executable names and installation
identifiers; application/package rebranding must precede a separately branded
binary release. See [development and usage instructions](docs/development.md).

The [architecture and implementation plan](docs/design/ai-assistance.md) describes
the integration points, PDF persistence design, provider authentication, and
acceptance gates.

## AI assistance in the prototype

| Capability | Intended experience |
| --- | --- |
| Ask about a document | A movable chat dock with separate message cards, controllable streaming scroll, and Document / Selection / View icon buttons; sharing approval remembered per PDF and service |
| Choose a provider | ChatGPT browser sign-in; presets for OpenAI, Claude, DeepSeek, Gemini, Groq, Mistral, OpenRouter, Grok, and local servers; model discovery |
| Guide your reading | Light / Balanced / Detailed cues: editable categories and colors; page-by-page background application, category filters and targeted regeneration; optional explanations for manual highlights |
| Organize your reading | Ordinary named chat tabs; double-click to rename; ask about selected text or figures; close and restore chats without losing content |
| Follow evidence | Exact supporting quotes become links to briefly outlined source passages; selection actions explain, define, unpack equations or critique in the current/new tab |
| Get a paper overview | A scannable guide: one-sentence idea, goal, approach, key takeaways, caveats, and why it matters; short bullets with page citations, saved in chat |
| Attach supporting material | Paste screenshots or attach images, UTF-8 text and text-based PDFs; extracted text and resized images are saved with the conversation, with screenshot thumbnails in prompt cards |
| Keep your work | Save the PDF to embed conversations, tab contexts and notes in a page-one note, alongside standard highlights; local recovery copies protect completed turns |

The first PDF save/reopen portability tests pass using a standard text note;
a document-level storage backend is future work. Inputs over 120,000 characters
are rejected rather than silently truncated. OCR and autosave are not implemented yet. Current-view
questions can include figures through image-capable models; whole-paper input
remains text-only. ChatGPT credentials are saved atomically in owner-only local
files; other provider API keys remain in memory. Press **Ctrl+S** to make work portable.
ChatGPT uses eligible plan access through **Continue with ChatGPT**, subject to
account/workspace availability and usage limits. Gemini, GitHub Copilot and Qwen account presets use the installed official CLI
and its own sign-in flow; these initial bridges support text only. Claude stays
API-only here: Anthropic does not permit Claude.ai subscription login inside
third-party applications. Other hosted presets use API keys and provider billing.
Provider adapters and sign-in validation are tested with local fixtures; live-provider/model testing
is still needed. See [usage and limitations](docs/development.md) and the
[full architecture plan](docs/design/ai-assistance.md).

## Source and issue reporting

- **This fork:** [gkouros/okulair](https://github.com/gkouros/okulair).
- **Fork issues and proposals:** [this repository's issue tracker](https://github.com/gkouros/okulair/issues).
- **Upstream Okular:** [KDE source repository](https://invent.kde.org/graphics/okular).

Report OkulAIr-specific issues in this fork. Report an issue upstream only after
reproducing it with an unmodified upstream Okular build, identifying the version
tested. Upstream support channels do not maintain this fork.

There are no OkulAIr binary releases documented here yet. Downloads from the
[Okular website](https://okular.kde.org/download.php) are upstream Okular, without
the planned OkulAIr AI features.

## Building and installing

The desktop build requires CMake 3.22+, Qt 6.8+, KDE Frameworks 6.13+, and
Poppler 25.03+ for PDF support. Your distribution must provide these versions.

On Ubuntu/Debian, clone the checkout and run the installer:

```bash
git clone https://github.com/gkouros/okulair.git
cd okulair
./scripts/install-okulair.sh --install-deps
```

`--install-deps` uses apt and sudo to install build dependencies. Omit that flag
if dependencies are already installed. Run the script as your normal user; only
the dependency installation needs sudo.

The script builds a Release version with AI and PDF support in `build-install/`
and installs into `~/.local/opt/okulair`. Handbook generation is disabled to avoid
requiring KDE DocTools translation data. Launch the installed application with:

```bash
"$HOME/.local/opt/okulair/bin/okulair" /path/to/paper.pdf
```

The `okulair` launcher sets runtime paths and starts a separate D-Bus session so
PDFs are not forwarded to an already running system Okular. The underlying
executable, plugins, and resources still use upstream names; keep a separate
installation prefix.

To rebuild and update the installation after changing or updating the source:

```bash
./scripts/install-okulair.sh
```

Use `--prefix PATH`, `--build-dir PATH`, or `--jobs N` to customize the install
location, build directory, or parallelism (default: 4 jobs). Choose a writable
installation prefix. Run `./scripts/install-okulair.sh --help` for all options.

For manual CMake builds, the development launcher, and tests, see
[development instructions](docs/development.md). The installer disables tests;
use the documented development configuration to build and run them. See also the
[upstream build notes](README.upstream.md) and
[formatting instructions](README.clang-format).

## Contributing

Open proposals and pull requests against this fork. Preserve existing copyright
and license headers; add an appropriate SPDX notice to new files. Fork-specific
code changes should normally use `GPL-2.0-or-later`, consistent with the main
application, while edits to inherited files must respect their existing terms.
Keep the project status accurate as features land.

## License and attribution

OkulAIr retains Okular's existing free-software licensing. The main application
source is predominantly **GPL-2.0-or-later**; other files have their own GPL,
LGPL, BSD, MIT, X11, documentation, or metadata terms. **Per-file notices govern.**
This fork does not relicense upstream work or remove upstream credits.

Read [LICENSE.md](LICENSE.md) for the licensing summary and distribution guidance,
[COPYING](COPYING) for the GPL version 2 text, [LICENSES/](LICENSES/) for the
existing collected license texts, and [NOTICE](NOTICE) for fork attribution.
The software is provided without warranty, subject to the applicable licenses.

The new [OkulAIr artwork](docs/branding/README.md) uses original vector shapes,
not the KDE logo or an adapted upstream Okular icon. Software license permissions
do not by themselves grant trademark rights or imply endorsement.

Requests appear below the chat toolbar with an **×** to cancel each running or
queued task. Tasks run sequentially; queued selection/view context is captured
when submitted. **Highlights** opens density, category and color controls, with
no preview step. Settings includes **Explain my new highlights automatically**.
