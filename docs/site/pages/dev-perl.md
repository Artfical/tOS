title: Perl
lede: Perl 5.40 is a first-class development language on tOS, with threads, the full standard library, and CPAN compatibility.
---
## Install

```
sudo tpkg install perl
perl -v
```

Perl 5.40.0 comes with its standard modules and is built with threads enabled (`-Dusethreads`).

## The interpreter

```
perl -e 'print "hello from tOS\n"'
```

Save a script as `script.pl` and run `perl script.pl`, or make it executable with `#!/usr/bin/env perl`.

## Core modules included

Perl 5.40's standard library is nearly complete. The most commonly used modules are present:

| Area | Modules |
|---|---|
| Data structures | `List::Util`, `List::MoreUtils`, `Hash::Util`, `Scalar::Util` |
| Serialization | `Data::Dumper`, `Storable`, `JSON::PP`, `YAML::XS` (if installed) |
| Files & I/O | `File::Temp`, `File::Basename`, `File::Spec`, `Path::Tiny`, `IO::All` |
| Text processing | `Text::CSV`, `Text::Wrap`, `Text::Template`, `Text::Diff` |
| Networking | `IO::Socket::INET`, `IO::Socket::SSL`, `LWP::UserAgent`, `HTTP::Tiny`, `Net::SSLeay` |
| Date/Time | `Time::Piece`, `Time::HiRes`, `DateTime` (if installed) |
| Testing | `Test::More`, `Test::Simple`, `Test2::V0`, `Test::Deep` |
| Concurrency | `threads`, `threads::shared`, `Thread::Queue`, `Thread::Semaphore` |
| Cryptography | `Digest::MD5`, `Digest::SHA`, `Crypt::OpenSSL::RSA`, `Crypt::OpenSSL::AES` |
| Encoding | `Encode`, `MIME::Base64`, `MIME::QuotedPrint` |
| System | `POSIX`, `Sys::Syslog`, `Sys::Hostname`, `IPC::SysV`, `IPC::SysV::Msg` |

Most are part of Perl 5.40's core. The rest are installable from CPAN.

## CPAN and modules

```
perl -MCPAN -e shell
# or
cpan Module::Name
```

CPAN works over HTTPS (the `ca-certificates` package provides the trust store). Local::Lib is supported:

```
perl -Mlocal::lib=~/perl5 -MCPAN -e 'install Some::Module'
```

Modules install under `~/perl5` and are found automatically.

## Threads

Perl was built with `-Dusethreads`. The `threads` and `threads::shared` modules work:

```perl
use threads;
use threads::shared;

my $counter :shared = 0;
my $thr = threads->create(sub { $counter++ for 1..100000 });
$thr->join;
print $counter;  # 100000
```

## Database access

```perl
use DBI;
my $dbh = DBI->connect("dbi:SQLite:dbname=/tmp/test.db","","");
$dbh->do("CREATE TABLE t (n INTEGER)");
$dbh->do("INSERT INTO t VALUES (42)");
my $sth = $dbh->prepare("SELECT n FROM t");
$sth->execute;
while (my @row = $sth->fetchrow_array) { print "@row\n" }
```

SQLite, DBI and DBD::SQLite are included.

## CPAN modules with C extensions

Modules that need compilation (e.g. `JSON::XS`, `Crypt::OpenSSL::AES`, `DBD::mysql`) need the compiler and the corresponding `-dev` packages:

```
sudo tpkg install gcc make openssl-dev
cpan JSON::XS
```

## Packaging a Perl script for tOS

```
# 1. Create a stage directory
mkdir -p mystage/bin

# 2. Put your script (with shebang) in bin/
cat > mystage/bin/mytool <<'EOF'
#!/usr/bin/env perl
use strict; use warnings;
print "Hello from Perl on tOS\n";
EOF
chmod +x mystage/bin/mytool

# 3. Write spec-mypkg.json
cat > spec-mypkg.json <<'EOF'
{
  "name": "mytool",
  "version": "1.0",
  "license": "MIT",
  "desc": "A Perl tool for tOS",
  "min_build": 619,
  "stage": "/home/user/mystage",
  "depends": ["perl"],
  "commands": { "mytool": "bin/mytool" },
  "licenses": [["LICENSE", "LICENSE"]],
  "notice": "mytool 1.0\nLicense: MIT\n",
  "message": "mytool installed: run mytool\n"
}
EOF

# 4. Build
python3 /root/tos-update/pkg/make_pkg.py spec-mypkg.json

# 5. Install on tOS
sudo tpkg install mytool --unsigned
```

## Related

- [Perl on tOS](https://perldoc.perl.org/) — official documentation.
- [CPAN](https://metacpan.org/) — module repository.
EOF