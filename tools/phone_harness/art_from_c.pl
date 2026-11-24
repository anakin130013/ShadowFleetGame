#!/usr/bin/perl
# Builds the phone harness's fake /art answer from the firmware's own data
# (commander_art.cpp + commander_art.h), so the offline renders show the real
# commander faces -- same JSON shape as the /art endpoint in web_server.cpp.
# Usage: perl art_from_c.pl <commander_art.cpp> <commander_art.h>  > art.js
use strict; use warnings;
my ($cpp, $h) = @ARGV;
local $/; open(my $fc, '<', $cpp) or die "$cpp: $!"; my $c = <$fc>;
open(my $fh, '<', $h) or die "$h: $!"; my $hh = <$fh>;
my (%pal, %px);
while ($c =~ /static const uint32_t P(\d+)\[\d+\] = \{([^}]*)\}/g) {
  my ($i, $body) = ($1, $2); $pal{$i} = join('', map { s/^\s*0x//r } split(/,/, $body));
}
while ($c =~ /static const uint8_t X(\d+)\[\d+\] = \{([^}]*)\}/g) {
  my ($i, $body) = ($1, $2); my @v = grep { /\d/ } split(/[,\s]+/, $body);
  $px{$i} = join('', map { sprintf('%02x', $_) } @v);
}
my @info; while ($hh =~ /\{\s*"([^"]+)",\s*"((?:[^"\\]|\\.)*)"\s*\}/g) { push @info, [$1, $2]; }
my @out;
for my $i (sort { $a <=> $b } keys %pal) {
  my ($n, $b) = @{$info[$i] || ['?', '']}; $b =~ s/'/\\'/g;
  push @out, "{n:'$n',b:'$b',p:'$pal{$i}',x:'$px{$i}'}";
}
print "window.HARNESS_ART = {w:24,h:28,c:[" . join(',', @out) . "]};\n";
