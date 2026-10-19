# Guest trust anchors

`cert.pem` is the unmodified curl PEM conversion of Mozilla's certificate
store dated 2026-09-25. `source.json` records its pinned download URL and the
publisher's SHA-256 checksum, checked before import. The conversion contains
121 roots. The original certificate/source notices remain in the file.

This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. You can obtain the license at <https://mozilla.org/MPL/2.0/>. The source
bundle and conversion details are available from
<https://curl.se/docs/caextract.html>.

The default guest path is `/etc/ssl/cert.pem`. Replace that file deliberately
to change guest trust; the launcher must preserve locally installed roots.
`htget --cacert FILE` selects a separate bundle for one request. Refresh the
pinned bundle and metadata together when updating trust anchors.
