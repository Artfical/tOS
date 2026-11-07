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

## Advanced bash features

```bash
#!/bin/bash
set -euo pipefail

# Arrays and associative arrays
declare -A ports=(["http"]=80 ["https"]=443 ["ssh"]=22)
for proto in "${!ports[@]}"; do echo "$proto -> ${ports[$proto]}"; done

# Process substitution and redirection
diff <(sort a.txt) <(sort b.txt)
grep pattern <(command_that_outputs)

# Parameter expansion
file=/path/to/file.txt
echo "${file##*/}"     # filename
echo "${file%.*}"      # without extension
echo "${file/path/new}" # replace first

# Arithmetic and logic
echo $(( 16 + 4 * 3 ))
[[ -f file.txt && -r file.txt ]] && echo "readable"

# Functions
die() { echo "ERROR: $*" >&2; exit 1; }
die "something went wrong"
```

## Loops and conditionals

```bash
for f in *.c; do echo "compiling $f"; gcc -c "$f"; done
while read -r line; do echo "got: $line"; done < file.txt
until ping -c 1 host; do sleep 5; done
```

## Pipes and the GNU tools

```bash
sudo tpkg install coreutils sed gawk grep findutils diffutils patch tar gzip less file wget rsync
```

Then:

```bash
# Find and process
find . -name '*.c' -exec grep -l 'main' {} +
grep -r 'TODO' --include='*.c' .
awk -F: '{print $1, $3}' /etc/passwd | sort
sed -i 's/old/new/g' file.txt
tar -czf backup.tgz folder/
```

## Here-documents and input

```bash
cat <<'EOF' > config.txt
server {
    listen 80;
    root /var/www;
}
EOF

# With parameter expansion
cat <<EOF
Hello $USER, today is $(date)
EOF
```

## Debugging

```bash
bash -x script.sh          # trace execution
set -x                     # trace from here
set +x                     # stop tracing
```

## Tips

- Always `chmod +x script.sh` before running.
- Use `#!/bin/bash` when you need bash features; `#!/bin/sh` (busybox ash) for maximum portability.
- `set -euo pipefail` at the top catches errors early.
- Commands are on the PATH by name and also under `/usr/bin` and `/bin`.

## Related

- [Terminal and shell](/tos/terminal) and [Shell commands](/tos/commands).
- [Developing on tOS](/tos/developing).
