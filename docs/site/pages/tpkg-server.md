title: The package server
lede: update.artfical.com, the endpoints clients use (list, files, signatures, key), what is public, caching, how a package is published, availability and what tpkg does when the server is unreachable.
---
## The server

The package server runs at **update.artfical.com**. It is a small Flask + Waitress application (`/root/tos-update/app.py`) that serves:

- The package catalogue (`/pkg/api/list?build=N`).
- Package files (`.tpkg`, source tarballs).
- Signatures (`/pkg/api/sigs?build=N`).
- The public signing key (`/pkg/api/key`).

## API endpoints

| Endpoint | Method | Parameters | Returns |
|---|---|---|---|
| `/api/check?build=N` | GET | `build` | `{ "latest": N }` (Software Update) |
| `/pkg/api/list?build=N` | GET | `build` | `name|version|licence|desc` lines |
| `/pkg/api/sigs?build=N` | GET | `build` | `name|base64(signature)` lines |
| `/pkg/api/key` | GET | — | PEM public key |
| `/pkg/<name>/<file>` | GET | — | The file (`.tpkg`, source tarball, licence) |

The `build` parameter gates packages by `min_build`. The server only lists packages with `min_build <= build`.

## Caching

- The client (`tpkg`) caches the index for 1 hour in `/tmp/tpkg-index-<build>`.
- `tpkg update` forces a refresh.
- Package files are not cached locally after install (they are unpacked and the archive is discarded).

## How a package is published

1. Maintainer writes a spec JSON (`spec-<name>.json`) with name, version, licence, description, dependencies, commands, source URLs.
2. Runs `python3 tools/tos-update/pkg/make_pkg.py spec-<name>.json` on a build machine.
3. The script stages files in `/system/pkg/<name>`, runs `make_pkg.py`, produces `.tpkg`, `meta.json`, licences, source tarball.
4. Maintainer copies the output to `/root/tos-update/pkg/<name>/` on the server.
5. Server signs the `.tpkg` on demand (when a client requests `/pkg/api/sigs`).

The signing key is at `/root/tos-update/keys/tpkg-signing.pem` (private) and `/root/tos-update/keys/tpkg-signing.pem.pub` (public, also in the kernel).

## Availability

The server runs behind Cloudflare. It is the same machine that hosts `docs.artfical.com` and the update check endpoint. Downtime is rare but possible.

If the server is unreachable:

- `tpkg look` and `tpkg search` fail with "server unreachable".
- `tpkg install` fails at the download step.
- `tpkg update` fails.
- Installed packages continue to work (they are local).

## Caching and mirrors

There is no official mirror. The server sets `Cache-Control: max-age=3600` on the index. You can run a local mirror by rsyncing `/root/tos-update/pkg/` and pointing `TPKG_HOST` to it (not officially supported).

## Related

- [What happens when you install](/tos/tpkg-install) — the install pipeline.
- [Signatures](/tos/tpkg-signatures) — how signing works.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
