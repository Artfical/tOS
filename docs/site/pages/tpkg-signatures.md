title: Signatures
lede: Why packages are signed, the exact scheme, what is signed, where the public key lives, key id, how tpkg verifies, what 'Signature:' in `tpkg info` means, --unsigned and its warning, what an attacker could and could not do, how the server signs on demand, key rotation limits.
---
## Why sign packages?

Without signatures, anyone who can tamper with the download (a compromised mirror, a man-in-the-middle, a hacked server) could slip malicious code into a package. tOS uses **ECDSA P-256 over SHA-256** so that only the holder of the private key (the update server) can produce a valid signature.

## What is signed

The message that is signed is a single line:

```
tOS-tpkg-v1|<name>|<version>|<size>|<sha256>
```

| Field | Example |
|---|---|
| `name` | `vim` |
| `version` | `9.2.1254` |
| `size` | `1234567` (bytes of the .tpkg file) |
| `sha256` | `a1b2c3d4...` (SHA-256 of the .tpkg file) |

The vertical bars are literal separators. The string is hashed with SHA-256, then signed with **ECDSA using the P-256 curve (secp256r1)**.

## Where the public key lives

The public key is compiled into the kernel binary (`kernel/shell/commands/cmd_tpkg.c`). It is a raw P-256 point (x, y) encoded as an uncompressed point (0x04 || x || y). The key id is `tos-tpkg-1`.

The kernel also serves it via the update server:

```
https://update.artfical.com/pkg/api/key
```

returns the PEM-encoded public key.

## Verification in tpkg

When `tpkg install` downloads a `.tpkg`, it:

1. Computes `sha256(file)`.
2. Builds the message string `tOS-tpkg-v1|name|version|size|sha256`.
3. Calls the kernel's ECDSA verify routine (in `cmd_tpkg.c`) with the embedded public key.
3. If verification fails, the install aborts with:

```
signature verification failed (ECDSA P-256)
```

No files are unpacked, no commands are registered.

## The `Signature:` field in `tpkg info`

```
tpkg info vim
# ...
Signature:   valid (ECDSA P-256)
```

If you run `tpkg info` on a package you built yourself (not from the server), it shows `Signature: none` or `unsigned`.

## `--unsigned`

```bash
sudo tpkg install mypackage --unsigned
```

Skips the signature check. **Use only for packages you built yourself.** You will see:

```
WARNING: installing unsigned package!
mypackage installed.
```

An attacker who can serve a fake package cannot bypass this without also compromising the tOS kernel (to replace the public key) or the update server (to sign with the private key).

## What an attacker could and could not do

| Attack | With signature check | Without (using --unsigned) |
|---|---|---|
| Modify package on server | Blocked (sig invalid) | Succeeds |
| MITM on download | Blocked (sig invalid) | Succeeds |
| Replace package on disk before install | Blocked (sig invalid) | Succeeds |
| Install a package you built | Works with `--unsigned` | Works |

**Nothing is kept quiet.** Every vulnerability, once fixed, is published in Artfical Security Intel.

## Server-side signing

The update server (`/root/tos-update/app.py`) signs on demand:

1. Build process creates `.tpkg` and computes its SHA-256.
2. Server builds the message `tOS-tpkg-v1|...`.
3. Server signs with the private key (ECDSA P-256, stored in `/root/tos-update/keys/tpkg-signing.pem`).
3. Signature is stored alongside the `.tpkg` in the package directory.
4. Client fetches both the `.tpkg` and the signature (via the `sigs` API).

The private key never leaves the server. The backup is at `/root/tpkg-signing-key.BACKUP.pem`.

## Key rotation

The public key is embedded in the kernel. To rotate keys:

1. Generate a new key pair.
2. Embed the new public key in a new kernel release.
3. Re-sign all existing packages with the new key.
4. Deploy the new kernel; old kernels will reject new packages.

There is no automated key rotation yet. The key id (`tos-tpkg-1`) would change to `tos-tpkg-2` etc.

## Related

- [What happens when you install](/tos/tpkg-install) — the install pipeline.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [Security](/tos/security) — broader security policy.
