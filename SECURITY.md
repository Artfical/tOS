# Security Policy

## Supported versions

tOS is released continuously: every commit on `main` becomes a build. Only the **latest build** gets security fixes. Use Software Update, or the newest ISO from the [Releases](https://github.com/Artfical/tOS/releases) page.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem.

Report it privately: open the repository's **Security** tab and choose **Report a vulnerability** (GitHub private vulnerability reporting). Say:

- what is affected (kernel, a driver, the network stack, tpkg, the update server, an app, a package),
- how to reproduce it (steps, a program, or a packet capture), and the build you used,
- what an attacker could do with it.

You can expect an answer within a week, and updates as the fix progresses. When it is fixed we will say so in the release notes and, if you wish, thank you by name.

## What is in scope

- The kernel and drivers: memory safety, privilege escalation, a program reading or changing another's memory or files, denial of service by an unprivileged program.
- The network stack and TLS handling.
- `tpkg` and the update path: signature checking, package integrity, how updates are fetched and installed.
- The login, `sudo` and permission system.

## Good to know

- tOS is a hobby system for learning; it is not hardened for production. Reports are still welcome.
- Packages are signed with an ECDSA P-256 key held by the update server. If you can install a package that is unsigned or has a wrong signature without `--unsigned`, that is a vulnerability.
- Third-party code (`third_party/`, packages) should be reported to its own project too, if the problem is in their code.

Please give us a reasonable time to fix a problem before you tell others about it.
