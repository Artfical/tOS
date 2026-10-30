# SSH, an SSH server, and git over SSH

## terminal | SSH
- New openssh package (sudo tpkg install openssh): ssh, scp, sftp, ssh-keygen, ssh-agent and ssh-keyscan. ssh user@host connects, ssh host command runs one command, a new host's key is remembered the first time and a changed key is refused.
- Git talks ssh: git clone git@github.com:user/repo.git works with a key from ssh-keygen (sudo tpkg upgrade brings the new Git).
- scp copies files to and from another computer; checked with a file sent there and back.

## settings | Services
- The same package brings sshd, an SSH server, as a service: sudo service start sshd (or sudo service enable sshd, or Settings > Services). People log in with an account of this tOS and its password; root with a key only.
- When a package is installed it can now tell you what to do next (how to connect to the server, how to change a password).
- A login is a plain shell (BusyBox) for now: no full-screen programs over SSH yet.

## terminal | Programs
- BusyBox gets sh (ash), env, xargs, ln, test and more; commands in /bin and /usr/bin (ls, git, ssh...) are found when a program looks for them there.
- Programs can listen for connections and accept them, wait on many descriptors with select and ppoll, copy the terminal with dup, and change user when they run as root (what a login server does).
- /etc/passwd and /etc/group always have a root entry.

## settings | Stability
- A background service starting a program no longer breaks a program that is starting at the same time (the loader is no longer interrupted halfway).
- A 32-bit program starting a 64-bit one (a shell running scp) no longer stops the whole system.
