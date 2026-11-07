title: How tOS protects your system
lede: Signed packages, accounts and permissions, separate programs, and a way to report problems.
---
## Signed packages

Every package on the server is signed with an ECDSA (P-256) key that only the server holds. tOS has the public key built in, and `tpkg install` checks the signature before it unpacks anything. A package that has been changed, or is not signed by the server, is refused. `tpkg info <name>` shows the signature, and `--unsigned` is the way to install something you built yourself, after a warning. See [Packages](/tos/packages).

## Accounts and permissions

![The login window](shot:login.png)

- Passwords are stored hashed (PBKDF2-HMAC-SHA256) in `/etc/shadow`, which only root can read.
- Files have owner, group and other bits, and folders need the execute bit to be entered. The sticky bit and setuid/setgid work as on Linux (a setuid program that others can write to has the bit ignored).
- `sudo` is for root and members of the `sudo` group, with a 5-minute password cache.
- Changing anything under `/system` needs root and the password.

## Programs are separate

A Linux program runs in its own address space with its own file table and working folder, as the user who started it. A program that crashes ends alone; the system carries on. Pointers a program passes to the kernel are checked before they are used.

> tOS is a hobby system for learning and is not hardened for production use. Do not rely on it for anything where a security failure would cost you.

## Reporting a vulnerability

Do not open a public issue. Write to **security@artfical.com**, or use GitHub's private vulnerability reporting on the [tOS repository](https://github.com/Artfical/tOS/security). Say what is affected, how to reproduce it and which build you used.

Every vulnerability, once fixed, is published in **Artfical Security Intel**: what it was, which builds were affected, which one fixes it, and credit to whoever reported it if they wish.

The full policy is in [SECURITY.md](https://github.com/Artfical/tOS/blob/main/SECURITY.md).

## Third-party code

The code tOS includes from other projects, with its licences, is listed in `THIRD_PARTY_NOTICES.md`. Every copy of tOS carries that file, and tFire shows it at `tfire:licenses`.
