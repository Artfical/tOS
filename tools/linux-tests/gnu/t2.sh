#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
PL=$P/perl/bin/perl
chk "$($PL -e 'print 6*7')" 42 "perl -e"
chk "$($PL -v | grep -o 'v5\.[0-9.]*')" v5.40.0 "perl version"
cat > /tmp/p1.pl <<'XX'
use strict; use warnings;
use List::Util qw(sum max first);
use Data::Dumper; $Data::Dumper::Sortkeys = 1; $Data::Dumper::Indent = 0;
use File::Temp qw(tempfile); use File::Basename; use File::Spec; use POSIX qw(floor strftime);
use Scalar::Util qw(blessed looks_like_number); use Storable qw(freeze thaw); use Digest::MD5 qw(md5_hex);
use MIME::Base64; use Time::Local; use Encode qw(encode decode); use Getopt::Long; use Cwd;
my @a = map { $_ * 2 } 1..10;
print "sum=", sum(@a), " max=", max(@a), "\n";
print Dumper({ a => [1,2], b => { c => 3 } }), "\n";
my ($fh, $fn) = tempfile(); print $fh "line1\nline2\n"; close $fh;
open my $in, '<', $fn or die; my @l = <$in>; print "lines=", scalar(@l), "\n"; unlink $fn;
my $t = thaw(freeze({ x => [1,2,3] })); print "storable=", scalar(@{$t->{x}}), "\n";
print "md5=", md5_hex("tOS"), " b64=", encode_base64("tOS", ""), "\n";
my $s = "The quick brown fox"; (my $u = $s) =~ s/(\w+)/\u$1/g; print "$u\n";
print "match=$1\n" if $s =~ /(q\w+)/;
print "floor=", floor(-2.5), " ", strftime("%Y", gmtime(0)), "\n";
print "utf8=", length(decode('UTF-8', encode('UTF-8', "\x{263A}"))), "\n";
my %h = (one => 1, two => 2, three => 3); print join(",", sort { $h{$a} <=> $h{$b} } keys %h), "\n";
printf "%s %5.2f %03d\n", "fmt", 3.14159, 7;
my $pid = fork(); if ($pid == 0) { exit 7 } waitpid($pid, 0); print "child=", $? >> 8, "\n";
pipe(my $r, my $w); if (!fork()) { close $r; print $w "viapipe\n"; exit 0 } close $w; print scalar <$r>; wait;
print "sprintf=", sprintf("%vd", "1.22.333"), "\n";
use threads; my $th = threads->create(sub { return 6 * 7 }); print "thread=", $th->join, "\n";
local $SIG{ALRM} = sub { print "alarm\n" }; alarm 1; sleep 2;
print "cwd ok\n" if getcwd();
print "done\n";
XX
out=$($PL /tmp/p1.pl 2>&1); echo "$out" | sed 's/^/  /'
chk "$(echo "$out" | tail -1)" done "perl big script ran to the end"
chk "$(echo "$out" | grep -c '^sum=110 max=20')" 1 "perl List::Util"
chk "$(echo "$out" | grep -c 'md5=')" 1 "perl Digest::MD5 and MIME::Base64"
chk "$(echo "$out" | grep -c '^child=7')" 1 "perl fork and waitpid"
chk "$(echo "$out" | grep -c '^viapipe')" 1 "perl pipe between processes"
chk "$(echo "$out" | grep -c '^thread=42')" 1 "perl threads"
chk "$(echo "$out" | grep -c '^alarm')" 1 "perl alarm"
echo 'a b c' | $PL -lane 'print scalar(@F), " $F[1]"' > /tmp/p2.out; chk "$(cat /tmp/p2.out)" "3 b" "perl -lane"
$PL -pi -e 's/x/y/g' /tmp/p2.out; chk "$($PL -MJSON::PP -e 'print JSON::PP->new->canonical->encode({a=>[1,2,{b=>undef}]})')" '{"a":[1,2,{"b":null}]}' "perl JSON::PP"
chk "$($PL -MPOSIX -e 'print((POSIX::strtol("ff",16))[0])')" 255 "perl POSIX"
chk "$($PL -MIO::Socket::INET -e 'print "sock\n"')" sock "perl IO::Socket::INET loads"
chk "$($PL -MCompress::Zlib -e 'print length(Compress::Zlib::uncompress(Compress::Zlib::compress("abc" x 100)))')" 300 "perl Compress::Zlib"
