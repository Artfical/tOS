title: How tOS protects your system
lede: Signed packages, accounts and permissions, programs kept apart, checked system calls, and a way to report problems. This page explains what tOS does, what it does not, and how to report a vulnerability.
---
## What tOS is, and is not

tOS is a hobby system for learning and experiments. It takes security seriously where it matters (below), but it is **not hardened for production use**. Do not rely on it where a security failure would cost you.

## Signed packages

Every package on the server is signed with an **ECDSA** key (curve P-256) that only the server holds. The signature covers a SHA-256 of the package's name, version, size and hash. tOS has the matching public key built in, and `tpkg install` checks the signature **before it unpacks anything**. A package that has been changed, or is not signed by the server, is refused.

- `tpkg info <name>` shows the signature.
- `--unsigned` installs a package without a valid signature, after a warning. It is meant for packages you made yourself.

See [Packages](/tos/packages).

## Accounts and passwords

- Accounts live in `/etc/passwd`, `/etc/shadow` and `/etc/group`.
- Passwords are stored **hashed** (PBKDF2-HMAC-SHA256) in `/etc/shadow`, which only root can read.
- `sudo` is for root and for members of the `sudo` group, and remembers the password for 5 minutes.
- The first user you create, in the installer, belongs to the `sudo` group.

![The login window](shot:login.png)

## Permissions

Files and folders have an owner, a group and read, write and execute bits for owner, group and everyone else. The sticky bit and setuid/setgid work as on Linux, with one safety rule: a setuid or setgid program that other users can write to has the bit ignored. These are checked on the file systems that store owners (ramfs, tFS, ext2/3/4, XFS, Btrfs). FAT, exFAT and NTFS have no owners.

Changing or deleting anything under `/system` needs root **and** the password, once per command.

![The password dialog for sudo shows the app that is asking](shot:password.png)

## Programs are kept apart

- Each program runs in **its own address space**, with its own file table and working folder, as the user who started it.
- A program that crashes ends **alone**: the kernel prints a message and the system carries on.
- Every pointer a program passes to the kernel is **checked** against that program's own page tables; a pointer into the kernel or into a hole is refused.
- The kernel heap checks itself for overruns and double frees.

## What is not protected

- The desktop and its apps run **inside the kernel**, so a bug there is a kernel bug. A fault in the kernel is a panic, not a program crash.
- Native tOS programs (`.t`) are not isolated as strongly as Linux programs. Permissions are not a hard wall against hostile native code.
- tOS is tested in virtual machines; real hardware has not had the same attention.

## Network

The kernel and tFire do TLS 1.2 and check server certificates against the list of trusted roots; `tls verify off` and `wget -k` turn that off for the session, and you should only do that for a server you know. A packet filter with connection tracking and NAT is built in (see [Network tools](/tos/network-tools)).

## Reporting a vulnerability

**Please do not open a public issue** for a security problem.

Report it privately:

- by email to **security@artfical.com**, or
- on GitHub: the [Security tab of the tOS repository](https://github.com/Artfical/tOS/security) and **Report a vulnerability**.

Say what is affected (the kernel, a driver, the network stack, tpkg, the update server, an app, a package), how to reproduce it (steps, a program, or a packet capture) and the build you used, and what an attacker could do with it. You can expect an answer within a week, and updates as the fix progresses.

### Artfical Security Intel

Every vulnerability, once it is fixed, is published in **Artfical Security Intel**: what it was, which builds were affected, which build fixes it, and credit to whoever reported it, if they wish. Nothing is kept quiet.

### What is in scope

- The kernel and drivers: memory safety, privilege escalation, a program reading or changing another's memory or files, a denial of service by an unprivileged program.
- The network stack and TLS handling.
- `tpkg` and the update path: signature checking, package integrity, how updates are fetched and installed.
- The login, `sudo` and permission system.

Only the **latest build** gets fixes: use Software Update. Please give us a reasonable time to fix a problem before you tell others. The full policy is in [SECURITY.md](https://github.com/Artfical/tOS/blob/main/SECURITY.md).

## Third-party code

The code in tOS that comes from other projects, with its licences, is listed in `THIRD_PARTY_NOTICES.md`. Every copy of tOS carries that file, and tFire shows it at `tfire:licenses`. If the problem is in someone else's code (a package, something in `third_party/`), report it to their project as well.
