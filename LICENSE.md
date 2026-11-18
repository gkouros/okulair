<!--
SPDX-FileCopyrightText: 2026 Georgios Kouros
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Licensing and attribution

OkulAIr is a modified version/fork of Okular. It preserves the inherited licensing
and copyright notices. This guide does not replace per-file terms or constitute
a complete dependency or release compliance audit.

## Existing licenses

The main application source, including `shell/main.cpp` and `aboutdata.h`, carries
`SPDX-License-Identifier: GPL-2.0-or-later`. A scan of source headers also found
LGPL-2.0-only, LGPL-2.0-or-later, BSD-2-Clause, BSD-3-Clause, MIT, X11, CC0-1.0,
and alternative GPL expressions. Documentation and inherited assets can have
separate terms. Do not apply one license label indiscriminately to every file.

- [COPYING](COPYING) contains the unchanged GNU GPL version 2 text.
- [LICENSES/](LICENSES/) retains the existing collected license texts.
- The `or-later` permission comes from the file's license notice, not a different
  GPL version 2 license body.
- Per-file SPDX identifiers, copyright notices, exceptions, and accompanying
  notices determine the terms. A missing header is not permission to assume
  ownership or arbitrarily relicense a file.
- `LicenseRef-KDE-Accepted-GPL` is an existing alternative grant; retain its
  collected text where that expression is used.

No inherited files or license texts have been relicensed by this branding change.
The author list in `aboutdata.h` and source headers are retained. The original
README is preserved in [README.upstream.md](README.upstream.md).

## New fork material

The fork README, this guide, AI architecture document, branding guide, and new
OkulAIr artwork are offered under **GPL-2.0-or-later**, with SPDX notices in
text/vector files or adjacent `.license` files for PNG exports. Those notices
apply only to the new material and do not claim ownership of upstream work.
[NOTICE](NOTICE) records the fork relationship and dated modifications.

New application code should normally use GPL-2.0-or-later unless a compatible,
deliberate choice is documented. Preserve existing terms when editing inherited
files. Imported dependencies, models, fonts, and artwork need their own license
and provenance review before inclusion. API access does not grant redistribution
rights to model weights or provider branding.

## When distributing a build

The GPL permits forks and commercial distribution, with conditions. Before a
binary release:

1. Preserve applicable copyright, license, attribution, and no-warranty notices,
   including interactive notices where required. Mark modified files and record
   relevant modification dates; retain upstream credits alongside fork credits.
2. Supply applicable license texts and required third-party notices with the
   distribution. Include this guide and NOTICE in release materials; inherited
   install rules have not yet been updated to package them.
3. Meet the GPL source-distribution requirements for the exact binary and chosen
   license version. A straightforward approach is to accompany each downloadable
   binary with its complete corresponding source archive, including fork changes
   and build/install scripts. A generic upstream URL is not corresponding source
   for a modified binary.
4. Preserve recipients' rights to modify and redistribute under applicable GPL
   terms. Do not impose an incompatible EULA or additional restrictions.
5. Review the actual dependency/backend combination and bundled assets. Most
   files using GPL-2.0-or-later does not establish license compatibility for every
   possible build; some files have alternative GPL versions or other terms.
6. Finish runtime/package rebranding: display name, About dialog, icons,
   desktop/AppStream metadata, application IDs, executable/plugin/resource paths
   where necessary, and bug-report destinations. A separate development install
   prefix does not complete distribution branding or collision prevention.

Private modifications do not themselves require publication to everyone.
Distributing copies triggers the relevant conditions; consult the actual license
for the selected distribution method. Software is supplied without warranty to
the extent specified by its licenses. Sources: [GNU GPL version 2][gpl2],
[GNU license FAQ][faq].

## Names, logos, and endorsement

Software licenses and trademark permissions are different matters. OkulAIr uses
its own document/lens mark and identifies its origin as an independent fork. It
does not use the KDE gear logo as its brand or claim KDE endorsement. KDE's
[published logo guidance][kde-logo] distinguishes copyright licensing from
trading/branding permission.

The OkulAIr name has **not** received a trademark availability search or legal
clearance. Attribution and original artwork reduce confusion but do not guarantee
name availability in every jurisdiction. Obtain a trademark review before public
launch if clearance is needed. This update is not a legal opinion or a guarantee
of compliance for future binaries, integrations, or commercial use.

[gpl2]: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
[faq]: https://www.gnu.org/licenses/gpl-faq.html
[kde-logo]: https://kde.org/stuff/clipart/

## Authentication cryptography dependency

AI-enabled builds now link the system OpenSSL 3 Crypto library for ID-token
signature verification. OpenSSL 3 uses [Apache-2.0](https://www.openssl-library.org/source/license/); retain its notices when bundling
it. [Apache-2.0 compatibility](https://www.gnu.org/licenses/license-compatibility.en.html)
requires using the GPLv3 option for GPL-2.0-or-later
code in a distributed combined build. This does not relicense inherited files;
review the exact selected dependencies and any GPL-2.0-only components before
releasing binaries. The current private installation uses system libcrypto and
does not bundle OpenSSL or model weights.
