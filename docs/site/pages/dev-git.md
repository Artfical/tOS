title: Git and SSH
lede: Git, curl, wget and OpenSSH work on tOS: you can clone over HTTPS and SSH, push and pull, and log in to other machines (or log in to tOS).
---
## Git

```
sudo tpkg install git
git --version
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
```

`git` needs `curl`, `zlib` and the certificates, and tpkg installs them with it.

### A repository

```
mkdir project && cd project
git init
echo "hello" > README
git add README
git commit -m "first commit"
git log --oneline
```

### Cloning

```
git clone https://github.com/Artfical/tOS.git
git clone --depth 1 https://github.com/Artfical/tOS.git     # a shallow clone, much smaller
```

HTTPS clones use the certificates of the `ca-certificates` package. SSH clones (`git@github.com:...`) need OpenSSH and a key (below).

A big repository takes memory: everything lives in RAM. Prefer shallow clones (`--depth 1`) of large projects, and give the machine 2 GB or more.

## OpenSSH

```
sudo tpkg install openssh
```

brings the client tools `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-add`, `ssh-agent` and `ssh-keyscan`, and the `sshd` server.

### Log in to another machine

```
ssh user@host
ssh -p 2222 user@host
scp file.txt user@host:/tmp/
```

### Keys

```
ssh-keygen -t ed25519
cat ~/.ssh/id_ed25519.pub
```

Add the public key to the other machine's `authorized_keys`, or to GitHub.

### Log in to tOS over SSH

`sshd` needs host keys and a configuration, and a user account. Once it is running, you log in with any SSH client and get a real terminal: window size, `Ctrl+C`, and in the shell `Ctrl+Z` and job control (`set -m` in busybox's shell, then `fg` and `bg`). Commands run over SSH without a terminal work too (`ssh host 'ls /tmp'`).

In QEMU, forward a host port to the guest to reach it: add `-nic user,model=e1000,hostfwd=tcp:127.0.0.1:2200-:22`, then `ssh -p 2200 user@127.0.0.1`.

## curl and wget

```
sudo tpkg install curl
curl -I https://example.com
curl -O https://example.com/file.tar.gz
```

GNU `wget` is its own package (`wget`) with HTTPS: `wget https://example.com/file`. The built-in `wget` command of the shell is simpler. See [Networking](/tos/networking).

## rsync

`rsync` copies and synchronises files, locally and over SSH:

```
sudo tpkg install rsync
rsync -a src/ dst/
rsync -a --delete src/ user@host:/backup/
```

## Git internals on tOS

The `git` package brings the standard Git binaries: `git`, `git-shell`, `git-upload-pack`, `git-receive-pack`, `git-upload-archive`. They run as Linux programs through tOS's compatibility layer, using the same sockets, file system and threads as on Linux. The package depends on `curl` (for HTTPS), `zlib` (compression), and the `ca-certificates` package for server verification.

## SSH keys and GitHub/GitLab

SSH is the recommended way to push. Generate an Ed25519 key:

```bash
ssh-keygen -t ed25519 -C "you@example.com"
cat ~/.ssh/id_ed25519.pub
```

Copy the printed public key to GitHub (Settings → SSH and GPG keys → New SSH key) or GitLab (Preferences → SSH Keys). Then clone with SSH:

```bash
git clone git@github.com:Artfical/tOS.git
cd tOS
```

The first connection asks you to verify the host's fingerprint. Type `yes` to add it to `~/.ssh/known_hosts`.

## HTTPS clones and credentials

For HTTPS, Git uses the `ca-certificates` package for server verification:

```bash
git clone https://github.com/Artfical/tOS.git
```

If the repository is private, Git prompts for a username and a **personal access token** (not your password). Create one on GitHub (Settings → Developer settings → Personal access tokens) or GitLab (Access tokens), give it `repo` scope, and use it as the password.

Shallow clones save memory:

```bash
git clone --depth 1 https://github.com/Artfical/tOS.git
```

A full clone of a large repository can use hundreds of megabytes of RAM (everything lives in tOS's ramfs). Shallow clones are much lighter.

## SSH agent and keys

```bash
eval $(ssh-agent)
ssh-add ~/.ssh/id_ed25519
```

Now `git push` and `ssh host` work without re-typing the passphrase.

## Remotes and workflows

```bash
git remote add upstream https://github.com/Artfical/tOS.git
git fetch upstream
git rebase upstream/main
git push origin main
```

For feature branches:

```bash
git checkout -b feature/xyz
# work work work
git commit -am "feat: add xyz"
git push origin feature/xyz
# open a pull request on GitHub/GitLab
```

## Submodules

```bash
git submodule add https://github.com/Artfical/tos-sdk.git sdk
git submodule update --init --recursive
```

## Configuration tips

```bash
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
git config --global init.defaultBranch main
git config --global pull.rebase true
git config --global core.autocrlf input
```

## Related

- [Networking](/tos/networking) and [Developing on tOS](/tos/developing).
- [Security](/tos/security) for how packages are checked.
