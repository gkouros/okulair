<!--
SPDX-FileCopyrightText: 2026 Georgios Kouros
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Developing and trying OkulAIr
The experimental desktop assistant is enabled by default. Configure with
`-DOKULAIR_AI=OFF` to omit its UI and network adapters. Nothing is sent to a
provider on startup or simply by opening a document.

## Checkout

```bash
git clone https://github.com/gkouros/okulair.git
cd okulair
```

Run the build commands below from this checkout. The repository is `okulair`;
upstream executable and CMake target names such as `okular`, `okularpart`, and
`okularGenerator_poppler` remain unchanged.

If you rename or move an existing checkout, configure a fresh build directory:
CMake caches absolute source paths. The development launcher locates the checkout
relative to itself and needs no path adjustment.

## Ubuntu build environment

Install core build dependencies (the PDF backend is required for these tests):

```bash
sudo apt install --no-install-recommends \
  extra-cmake-modules qt6-base-dev qt6-base-private-dev libqt6svg6-dev \
  libkf6archive-dev libkf6bookmarks-dev libkf6completion-dev \
  libkf6config-dev libkf6configwidgets-dev libkf6coreaddons-dev \
  libkf6crash-dev libkf6i18n-dev libkf6kio-dev libkf6threadweaver-dev \
  libkf6windowsystem-dev libkf6xmlgui-dev libkf6iconthemes-dev \
  libkf6parts-dev libkf6colorscheme-dev libkf6textwidgets-dev \
  libkf6widgetsaddons-dev libpoppler-qt6-dev libssl-dev
```

For a desktop/PDF development build that treats additional backends as optional:

```bash
cmake -S . -B build \
  -DBUILD_TESTING=ON -DOKULAR_UI=desktop -DOKULAIR_AI=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" \
  '-DFORCE_NOT_REQUIRED_DEPENDENCIES=KF6Wallet;KF6DocTools;Qt6Qml;TIFF;LibSpectre;KExiv2Qt6;DjVuLibre;EPub;Discount' \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6TextToSpeech=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_KF6Purpose=ON
cmake --build build --target okular okularpart okularGenerator_poppler \
  aisessiontest aisessionstoretest aiassistanttest chatgptauthtest --parallel 4
./scripts/run-okulair-dev.sh /path/to/paper.pdf
```

The wrapper selects the build's plugins and uses a separate D-Bus session to
avoid forwarding the document to an installed upstream instance. It does not
install or replace system Okular. Set `OKULAIR_BUILD_DIR` to use a different build
folder. Runtime display names, package IDs, and the executable are still inherited
from Okular; full distribution rebranding is pending.

## Using the first prototype

1. Select **AI settings** in the left sidebar. The separate **AI chat** dock starts
   on the right; drag its title bar to the left, right, top, or bottom, or float it.
   Reopen it with **Show chat** or **Ctrl+Alt+A**. Dock layout is saved locally.
2. For subscription access, choose **ChatGPT (subscription sign-in)** and click
   **Continue with ChatGPT**. Complete account selection and plan-use consent in
   your system browser. Eligible account/workspace access is required. Available
   models load into the dropdown automatically; no API key or manual model ID is
   needed. You can add another account, switch accounts, or sign out. Plan/app
   limits apply; any credits enabled in your ChatGPT settings remain subject to
   those settings. OkulAIr never falls back to API-key billing.
3. Alternatively choose **DeepSeek**, **Google Gemini**, **Groq**, **Mistral**,
   **OpenRouter**, **xAI / Grok**, **OpenAI**, or **Anthropic / Claude**. These are
   API-key integrations with provider billing, not consumer-subscription login.
   Enter your key and click **Connect / load models**, then select a model.
   API model IDs can also be typed manually if discovery is unavailable. For a
   local/open-weight model, choose **Local / custom compatible server**, such as
   Ollama at `http://localhost:11434/v1`. Hosted API keys stay in memory for this
   window and are never saved in the PDF, exported history, or settings.
4. On the first AI action, a popup identifies the destination and explains that
   selected PDF content, conversation and chat attachments will be sent to it. Hosted services
   receive this outside your device and may process/retain it under their
   policies; **Current view** also sends a document image. Choose **Allow and
   remember** to approve, or **Not now** to send nothing. Approval is remembered
   locally for this PDF and service across reopen/restart; **Reset PDF sharing
   permission** in AI settings revokes it. A different PDF or service asks again.
   Local servers may proxy cloud models. Changing an endpoint clears its API key.
5. Choose the **Document**, **Selection**, or **View** icon-and-text button above
   the prompt. Document uses the entire paper; View includes an image. Select text with Okular's text selection tool and
   right-click **Ask AI about selection**, or click an existing highlight and
   choose the selection scope. Current view uses the visible PDF area, including
   figures, without a manual screenshot. It requires an image-capable model.
   There is no fallback that silently sends the whole paper if a selection is
   missing. The scope is captured when you submit; scrolling later does not
   change the request. Enter sends, Shift+Enter inserts a newline, and Escape
   stops generation. **Overview** and **Highlight** use the same chosen scope.
   Highlights use locally validated exact quotes, apply page by page, and stay out of chat history.
   Applied highlights have category colors, a reason, **AI assistant · model**
   author, and creation/modification timestamps. Each is separately undoable.
6. Press **Ctrl+S** (the normal Save action) to embed the history and annotations.
   This saves the entire modified document, including manual edits. Normal save
   prompts/restrictions apply to signed, remote, or read-only files. Autosave is
   not implemented yet.

The prototype stores versioned JSON in a standard text-note annotation on page
one, visibly attributed to OkulAIr. This is portable PDF content, not a sidecar
requirement. Reopen the saved PDF with this build to restore the chat; another
reader may display the note's JSON. Keep that annotation intact. A future storage
backend will use a document-level attachment after Poppler integration work.

The conversation travels with the PDF when shared. Local recovery copies have owner-only permissions and are offered on reopen
when they extend the embedded history for the same file size/modification time.
Divergent or externally changed files are not automatically overwritten. Recovery
is not a substitute for saving the PDF; stale recovery copies are retained for
manual inspection under the app data directory's `okulair-recovery/` folder.

Whole-paper mode sends the extracted text with the conversation. Scoped modes
send the selected/visible evidence instead. Input over 120,000 characters is
rejected rather than silently truncated.
The provider/model can impose a smaller context limit. Scanned pages need OCR;
whole-paper mode is text-only. Current-view mode can interpret figures and tables
through an image-capable model. Page links are model-proposed
navigation aids, not proof of claim support: verify the source text. Interrupted
output is retained as incomplete and is not replayed as completed assistant text.
The session-note limit is 32 MiB / 2,000 messages. Retrieval
for long papers, semantic claim verification, editable overviews, grouped undo,
and automatic saving remain follow-up work.

## Tests

```bash
QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH="$PWD/build/bin" \
  ctest --test-dir build \
  -R '^(aisessiontest|aisessionstoretest|aiassistanttest|chatgptauthtest)$' --output-on-failure
```

The tests use fake HTTP/SSE providers on loopback, without real API keys or paid
requests. Socket-restricting sandboxes must allow localhost for the network tests.
Use temporary `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_CACHE_HOME` directories
for isolated test settings and recovery files. The PDF integration test saves and
moves a PDF, deletes the original, and reopens it in a new Document instance while
checking both AI history and a manual annotation. UI workflows exercise consent,
chat, summaries, quote validation, undo, safe rendering, and document switching.

Provider protocol references used for the initial implementation:
[OpenAI streaming](https://developers.openai.com/api/docs/guides/streaming-responses),
[Anthropic streaming](https://platform.claude.com/docs/en/build-with-claude/streaming),
and [Ollama compatibility](https://docs.ollama.com/api/openai-compatibility).
Live-provider interoperability still needs testing with the user's chosen models.

## ChatGPT connection storage and validation

The browser flow follows OpenAI's documented native open-source
[registration and sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in)
and [models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference).
It uses loopback callbacks, state, nonce, PKCE, issuer/audience checks, and
OpenSSL 3 signature verification. Each issued client registration retains its
verified identity. Tokens rotate atomically under a cross-process lock.

Credentials are stored outside PDFs at
`$XDG_CONFIG_HOME/okulair/chatgpt/accounts.json` (normally
`~/.config/okulair/chatgpt/accounts.json`), with owner-only permissions (0600)
in a private directory (0700). This is protected local storage, not encrypted
KWallet storage. Never share or commit that file. Sign-out clears local tokens
and attempts remote revocation when discovery is available; a notice explains
when revocation could not be confirmed. Registration and host identifiers remain
for future sign-in. Disconnect the app in ChatGPT settings to revoke remotely.

A saved ChatGPT connection may refresh credentials and load the model catalog on
startup. This does not send document text or start inference. Document sharing
still requires consent and an explicit action. Tests cover signed-token
validation, rejected callbacks, protected storage, model discovery, visible setup
errors, and plan-limit errors. Real browser authorization and vendor inference
have not been exercised by the automated fixtures; availability is determined by
the provider and the user's account/workspace.

Other preset endpoints follow their providers' documented compatible APIs:
[DeepSeek](https://api-docs.deepseek.com/en/),
[Gemini](https://ai.google.dev/gemini-api/docs/openai),
[Groq](https://console.groq.com/docs/openai),
[Mistral](https://docs.mistral.ai/api/endpoint/chat),
[OpenRouter](https://openrouter.ai/docs/quickstart), and
[xAI](https://docs.x.ai/developers/rest-api-reference/inference).

Model catalogs have a separate 8 MiB download limit because account catalogs
include extensive metadata; authentication responses remain capped at 512 KiB.
Download-limit, transport, and JSON errors are reported separately from HTTP
permission errors. Regression fixtures cover catalogs larger than 512 KiB and
verify that both limits remain enforced.

## Focused context and image privacy

Selected/visible text, physical page numbers, and normalized source rectangles
are stored with the chat turn so its scope survives PDF save/reopen. View images
are encoded in memory from the PDF viewport only, bounded to 1600 pixels on the
long edge and 4 MiB, and sent only for an explicit Current-view action after
sharing consent. No desktop or other application is captured, and no screenshot
file is created. Images are not embedded in the chat note or recovery journal.
Later requests retain earlier scoped text but do not reattach earlier images;
the model is told when a prior image is absent. Re-select Current view for visual
follow-up questions. Text-only models cannot answer visual questions; a rejected
image request offers the choice of an image-capable model or text selection.

The chat toolbar contains Overview, Highlights, and Settings. Saving stays with
Okular's standard actions (Ctrl+S), and the former Export chat button is removed.
Old highlight payloads in existing chat notes are preserved for compatibility but
are hidden from the conversation and excluded from requests. New highlight
requests create no chat turns or session notes; their results persist as regular
PDF highlights, including author/model attribution, reason, and dates.

Current-view readiness checks use the visible page rectangles at their actual
render dimensions, including tiled rendering at high zoom. Offscreen/unrendered
parts of a page do not prevent a ready viewport from being sent. Integration
regressions exercise both ordinary page bitmaps and zoomed tiled pages.

Background chat-note creation and updates preserve the reader's current page,
zoom, and pan position, including undo/redo of history changes. Manual annotation
commands retain their normal navigation behavior. View position is not restored
from a stale request snapshot when an answer arrives.

## Chat reading and layout

Messages appear as separate cards: tinted, indented question cards and distinct
assistant cards labeled with the model. Context and timestamps are shown where
available, and page citations remain clickable. Model text is escaped before
rendering; message cards do not load external images or execute model HTML.

The scope selector sits immediately above the prompt. Status/error messages sit
below the prompt, above Ask/Stop. While generating, the transcript follows the
latest text until you scroll up. Earlier output stays at your chosen scroll
position as tokens arrive and when generation finishes. Scroll back to the
bottom to resume following. Sending a new prompt follows the new turn.

Sharing approval is a local preference keyed by the PDF's canonical file location
and provider protocol/endpoint. It is not embedded in the PDF and cannot grant
permission on someone else's computer. Moving/copying a PDF or changing the
service asks again; ordinary saving/annotation updates at the same path retain
approval. Only the chosen context and conversation are sent; the popup does not
mean that the original PDF file is uploaded wholesale.

## Chat attachments and controls

Use **Attach**, paste a screenshot with Ctrl+V or the prompt's Paste action, or
paste local file URLs. Images, UTF-8 text/code files, and text-based PDFs are
supported. Pending attachments can be removed before sending and are never
uploaded merely by adding them. The existing per-PDF/service sharing approval
covers chat attachments as well as document context.

At most four attachments are accepted per message. Source files must be at most
10 MiB. PDF text extraction is limited to 200 pages, respects copy restrictions,
and accepts at most 80,000 extracted characters per attachment; scanned PDFs need
screenshots or OCR outside the app. PDF figures are not extracted from attachments.
The combined conversation/evidence input limit remains 120,000 characters; nothing
is silently truncated. Images are resized to at most 1600 pixels and reduced to
fit 4 MiB per image. Their resized PNGs, filenames, and extracted attachment text
are embedded in the conversation note, and included on subsequent chat turns.
Original binary PDFs/text files and source filesystem paths are not embedded.
All saved attachments travel with the PDF. The total 32 MiB session limit still
applies; the UI reports a storage error before sending if it would be exceeded.
Image attachments require a model/provider that supports image input.

**Clear chat** in AI settings cancels generation and clears the active tab’s
conversation, preserving other tabs and notes, with annotation undo and normal Ctrl+S saving. **Clear AI highlights**
removes only AI-authored highlights, preserving manual highlights and locked
annotations. **Clear user highlights** separately removes manual highlights while
keeping AI highlights and locked annotations. Each affected page is undoable without
moving the PDF view. Active context buttons use the same teal as explanation highlights.
Status messages sit below Attach/Ask/Stop. Message cards and the prompt have
slightly rounded corners. Streaming appends deltas in 40 ms batches, preserving
the existing document instead of rebuilding it for every provider event.

The **Overview** action gives a quick reading guide rather than a rewritten
abstract: In one sentence, Goal, How it works, Key takeaways, Caveats, and Why it
matters. The prompt asks for short bullets and plain language, with supporting
page citations and explicit separation of author-stated and inferred limitations.
Selection/View overviews are limited to the supplied excerpt. Overview headings
and emphasis use a small escaped-text formatter; provider HTML is never executed.
Existing saved summaries remain available; use Overview again for the new format.

## Reading workspaces

The AI panel opens with **Main chat**. Click **+** immediately after the last tab,
or the blank space to its right, to create **Chat**. All tabs are ordinary chats.
Double-click a tab to rename it inline; Escape cancels. The **⋮** menu also lets
you close, restore or permanently delete chats. Main chat stays open.
There are at most 32 tabs, including closed ones, within the shared 32 MiB note
limit. **Clear chat** affects only the active conversation.

Right-click selected text to ask in the current chat or a new chat. Figure
questions capture a region image and its extracted text. A removable context chip
shows the selected target. Switching to Document or View clears that target;
there is no pin-context dropdown. Each chat sends its own history and attachments.
Captured figure images are resized to at most 1600 pixels / 4 MiB, subject to
the overall storage limit. Document copy restrictions apply.

Version 1 conversations load into Main chat. Version 2 tabs remain compatible:
old scratchpad notes become a saved note message; captured contexts and custom
tab titles are retained. Ctrl+S saves chats and other annotations in the PDF.
Unsaved recovery and annotation undo/redo preserve chat changes. Storage failures
report an error and preserve the preceding saved state.

## Evidence and selection actions

Textual claims are requested with citations of the form
`[p. N | short exact supporting quote]`. The UI validates the quote against the
physical page; only a unique exact match becomes an **evidence** link. Clicking
it centers and briefly outlines/highlights the exact text independently of the
reader's text selection and normal search. An unmatched/ambiguous quote is marked
**quote not located**. Plain `[p. N]` references remain page links and are not
presented as exact evidence. Quote-location verification does not prove that the
quote supports the model's interpretation. Figures without extractable text use
page references instead of pretending to provide a textual quote anchor.

Right-click selected PDF text → **Ask AI about selection** or **Ask AI about
selection in a new chat**. Then type your question. No preset reading-tools submenu
is shown. Right-click a page → **Ask AI about screenshot**, **Ask AI about
screenshot in a new chat**, or **Copy screenshot to clipboard**, then drag a box
around the figure. The new-chat action opens a fresh chat with the captured region. This explicit region
selection works for raster images and vector plots, whose boundaries are often
not identified as image objects by PDF backends. Escape cancels. Copying creates
an image locally without sending anything to a model; asking uses the normal
sharing/connection checks and requires an image-capable model for visual details.
After successful submission, a captured screenshot is removed from the active
Selection context and the composer returns to Document mode. The submitted user
message retains its image reference and displays a local thumbnail, including
after saving and reopening. Image attachments also show thumbnails; no remote
image URLs from model output are rendered.

Transient source/text-selection highlights use a soft additive blue blend on
dark pages so bright text retains its original color, with a readable blue border.

## Reading highlight categories and density

**Highlights** opens a compact control/legend view:

| Category | Color | Intended span |
| --- | --- | --- |
| Keyword | Soft yellow `#F6D86B` | 1–4 words for scanning |
| Core contribution | Soft coral `#F28B82` | 1–6 words naming a method, contribution, mechanism |
| Explanation | Soft teal `#64E0CA` | A definition/short clause, at most 25 words |

Fills use 65% annotation opacity. Each annotation keeps a stable category label,
model attribution, creation/modification timestamps and a plain-language reason.
Light / Balanced / Detailed are ceilings of 4 / 7 / 11 cues per text page,
respectively; the model is asked to use fewer when appropriate, with at most
1 / 1 / 2 core cues. Repeated terms, overlaps, references and boilerplate are
avoided. Long or non-verbatim proposals are rejected locally. Repeated keywords
need a unique verbatim anchor to resolve their occurrence.

Whole-document highlighting processes one text page per request, with up to
200 pages / two million extracted characters total and a 120,000-character batch
input limit. Each completed batch is parsed, validated against the source and
applied immediately, without a review dialog. The document remains interactive. Highlighting has its own provider worker and captured
scope, independent of chat requests, tab changes, and the chat Stop button. The
setup dialog closes when generation starts and never reopens on a page or job
completion. Its own × in Requests cancels only highlighting.
Cancelling or a failed later page retains already-applied highlights; undo and
Clear AI highlights remain available. No highlight JSON or prompts enter chat.

Category checkboxes select the categories used by both **Highlight in background**
and **Regenerate checked categories**, and control their visibility in the current
view. Each background job captures the checked set; returned proposals for
unchecked categories are rejected locally. Regeneration replaces only checked
categories, retaining unchecked, manual and locked annotations. At least one
category must be checked to start either action.

Use **+** below the category rows to add a category. Double-click a category name
to open its editor, including **Delete category**; Enter or F2 also opens it from
the keyboard. There is no category dropdown, All categories button, or separate
Add/Edit/Delete button row. **Reset categories to defaults** restores the original
three categories, names, guidance, colors and checked state, removing custom
category preferences. Existing PDF annotations keep their saved contents and
colors; density and manual-explanation preferences are retained. Built-in and
custom categories can be edited or deleted in any order. Choose a name, guidance,
maximum span (1–25 words) and
color. A color button updates the matching editable highlights in the current
PDF and the default for new highlights. Edits are undoable. Deleting a category
removes its generation preference, retaining existing PDF annotations. Category
preferences are saved locally; saved annotations retain their actual colors,
category identifiers and explanatory contents in the PDF. Opening a document does
not automatically recolor it. Regeneration replaces the checked AI
categories, page by page; manual and locked annotations are kept.

AI-generated notes contain the category and concise explanation, without copying
the selected text. **Explain my new highlights automatically** in the left settings
is off by default. Enabling it queues explanations for newly created manual
highlights. Existing personal notes are preserved; automatic copies of the quoted
passage are replaced with the explanation. Disabling the option cancels its queued
explanations. A deleted, locked or edited target is not overwritten when a response
arrives. These requests use ordinary service validation and sharing permission,
remain out of the chat transcript, and are cancellable in the request list.

## Requests

A collapsible **Requests** list below the chat toolbar shows running chat and background-highlight tasks and
up to 32 queued chat tasks, each with an **×**. Chat requests run sequentially while highlighting proceeds independently, using a
snapshot of selection/view context captured when queued. Changing the service,
account or model cancels incompatible queued requests. Cancelling a queued task
sends nothing; cancelling a running task aborts its network/client process and
keeps already-applied highlights or any incomplete chat response. Pending tasks
are local in-memory state and are discarded when the document changes.

## Account-based services

ChatGPT retains its existing browser account connection. Additional account
presets invoke installed, unmodified official clients: `gemini`, `copilot` or
`qwen`. Install the provider's official CLI, select the corresponding account
preset, and click **Sign in using official client** to open its own terminal and
browser flow. Leave Model as `auto` or choose a model supported by that client.
No provider credentials are copied into OkulAIr or the PDF. Account quotas apply;
API-key environment variables are removed from these requests. No API fallback is
performed. These initial bridges support text only and reject image requests
explicitly rather than omitting the images. Requests use bounded output/time,
private temporary working directories and disabled tool execution/customizations.
Live account interoperability is not covered by the local executable-stub tests.

Official setup/authentication references:

- [Gemini account authentication](https://geminicli.com/docs/get-started/authentication/)
- [Copilot CLI setup](https://docs.github.com/en/copilot/how-tos/copilot-cli/set-up-copilot-cli/install-copilot-cli)
- [Copilot programmatic interface](https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-programmatic-reference)
- [Qwen account authentication](https://qwenlm.github.io/qwen-code-docs/en/users/configuration/auth/)
- [Qwen headless mode](https://qwenlm.github.io/qwen-code-docs/en/users/features/headless/)

Claude remains API-only in OkulAIr. Anthropic's current [authentication policy](https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use)
does not permit offering Claude.ai subscription login in third-party applications
or intermediating its credentials. Other API presets do not automatically gain
subscription access because a provider offers a consumer chat plan.

## Saving after bulk highlight removal

Bulk clearing uses grouped undo commands. Backing-file replacement after saving
now walks those groups and refreshes every annotation command's page references,
instead of rejecting the group and losing the open document. Tests cover clearing
both user/AI highlights, repeated in-place saving, undo/redo and PDF reopening.

Screenshots sent as chat attachments are embedded in the PDF conversation, and
unsaved edits have a private recovery copy in the application's local data folder
(`okulair-recovery`). Clipboard-only capture does not add an image to the chat.
Replacing an unsent captured context or clearing a chat now prunes unreferenced
image assets; images referenced by retained messages remain available. Session
limits are application memory safeguards, not PDF format or provider-plan limits.


Chat rendering creates the generating answer card before the first token, then
inserts only new text into that card. Rounded-card painting clears the damaged
viewport background first, preventing old glyphs from showing through during
scrolling and layout changes. Reader-controlled scrolling remains independent of
streaming updates.
