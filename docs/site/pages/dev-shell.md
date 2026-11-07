title: Shell scripting
lede: bash, busybox's shell, Perl, awk and sed run on tOS. This page shows how to write and run scripts, and what each tool is good for.
---
## Which shell

| Shell | Package | Notes |
|---|---|---|
| tOS's own shell | built in | A simple shell with the built-in commands. For scripts with loops, pipes and variables, use `sh` or `bash` |
| `sh` (busybox ash) | `busybox` | A small POSIX shell. `#!/bin/sh` scripts and `./configure` run with it |
| `bash` | `bash` | GNU Bash 5.2: arrays, associative arrays, `[[ ]]`, process substitution, and more |

```
sudo tpkg install bash
bash --version
```

## A script

Save this as `hello.sh`:

```
#!/bin/sh
for name in tOS Linux; do
    echo "hello, $name"
done
```

then:

```
chmod +x hello.sh
./hello.sh
```

A script that starts with `#!/bin/bash` needs the `bash` package; one that starts with `#!/usr/bin/env python3` needs `python3`. The interpreters are found by name through the packages.

## bash

```
#!/bin/bash
set -eu
declare -A age=([alice]=30 [bob]=25)
for n in "${!age[@]}"; do echo "$n is ${age[$n]}"; done
files=(a b "c d")
echo "${#files[@]} files, last: ${files[-1]}"
s="Hello World"; echo "${s,,} ${s^^} ${s/World/tOS}"
while read -r line; do echo "got: $line"; done < <(printf 'one\ntwo\n')
[[ $s =~ ^H(.*)d$ ]] && echo "matched: ${BASH_REMATCH[1]}"
printf '%05.1f %x\n' 3.14159 255
```

Process substitution `<(...)`, functions, `trap`, `$(( ))`, here-documents, `pipefail`, `{1..5}` and the rest work.

## Pipes and tools

The GNU tools come in their own packages: `coreutils`, `sed`, `gawk`, `grep`, `findutils`, `diffutils`, `patch`, `tar`, `gzip`, `less`, `file`.

```
sudo tpkg install coreutils sed gawk grep findutils
printf 'b 2\na 1\nb 2\n' | sort -u
grep -P '(?<=th)\w+' words.txt
find . -name '*.c' | xargs grep -l main
awk -F: '{ n += NF } END { print n }' /etc/passwd
sed -i 's/old/new/g' file.txt
tar -czf backup.tgz folder && tar -tzf backup.tgz
```

`coreutils` brings `ls`, `cp`, `mv`, `sort`, `sha256sum`, `date`, `stat`, `install` and the rest as commands by name. `grep -P` (Perl regular expressions) works.

## Perl

```
sudo tpkg install perl
perl -e 'print 6 * 7, "\n"'
echo 'a b c' | perl -lane 'print scalar(@F), " $F[1]"'
```

Perl 5.40 with its standard modules: `List::Util`, `Data::Dumper`, `File::Temp`, `POSIX`, `Storable`, `JSON::PP`, `Digest::MD5`, `Compress::Zlib`, `IO::Socket::INET` and the rest, with threads, `fork`, pipes and `alarm`.

## Archives and compression

| Task | Command |
|---|---|
| Make a tar archive | `tar -cf a.tar folder` |
| ... compressed with gzip or bzip2 | `tar -czf a.tgz folder`, `tar -cjf a.tbz folder` (and `-J` for xz, with the `xz` package) |
| Unpack | `tar -xf a.tar`, `tar -xzf a.tgz -C /tmp` |
| Compress one file | `gzip file`, `bzip2 file`, `xz file` |
| Zip | `zip a.zip files...`, `unzip a.zip` (built in) |

## Tips

- Use `chmod +x` to make a script run directly.
- In a script that must run anywhere, start with `#!/bin/sh` and use only POSIX; use `#!/bin/bash` when you need bash features.
- Commands are on the PATH by name and also under `/usr/bin` and `/bin`.

## Related

- [Terminal and shell](/tos/terminal) and [Shell commands](/tos/commands).
- [Developing on tOS](/tos/developing).
