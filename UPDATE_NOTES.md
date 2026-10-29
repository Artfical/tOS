# Git, and tpkg packages that need other packages

## terminal | Git
- Git 2.56 runs on tOS: sudo tpkg install git. It clones over HTTPS (git clone https://github.com/...), commits, branches, merges and rebases.
- Git needs curl, OpenSSL, zlib and a list of trusted certificates; tpkg installs those first, as their own packages (curl, openssl, zlib, ca-certificates).

## terminal | tpkg
- Packages can say which others they need: tpkg install installs those first, tpkg remove will not remove a package another one needs (add --force to), and tpkg info shows what a package needs and what needs it.
- tpkg update and tpkg look say which installed packages have a newer version on the server, and tpkg upgrade installs them.
- tpkg info <name>, tpkg search <text> and tpkg which <command> work without sudo.
- Node.js 24 and 22 now come with a NOTICE (what was changed, licenses, sources).

## settings | Files
- Files from an archive keep their execute bit, so programs find the helpers they start.
- Paths with a double or trailing slash (dir/, a//b, ./x) work everywhere.
