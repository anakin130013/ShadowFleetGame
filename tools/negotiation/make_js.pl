#!/usr/bin/perl
# Prints questions.txt as the JavaScript BANK array the negotiation mockup uses:
#   perl make_js.pl > bank.js
# (The firmware generator will read the same file -- questions.txt stays the one source.)
use strict;
use warnings;
use FindBin;

my %val = ('+' => 1, '-' => -1, '0' => 0);
my (@q, $line);
open my $in, '<', "$FindBin::Bin/questions.txt" or die "questions.txt: $!";
while ($line = <$in>) {
  $line =~ s/\r?\n$//;
  next if $line =~ /^\s*(#|$)/;
  if ($line =~ /^Q (EASY|MEDIUM|HARD) \| (.+)$/) { push @q, { tier => lc $1, q => $2, r => [] }; next; }
  if ($line =~ /^A (.+?) \| ([-+0]) ([-+0]) ([-+0]) ([-+0])$/) {
    die "reply before any question: $line\n" unless @q;
    push @{ $q[-1]{r} }, [ $1, map { $val{$_} } $2, $3, $4, $5 ];
    next;
  }
  die "can't read line: $line\n";
}
close $in;

sub js { my $s = shift; $s =~ s/\\/\\\\/g; $s =~ s/"/\\"/g; return "\"$s\""; }

print "  const BANK = [\n";
my @out;
for my $x (@q) {
  die "question needs 3 replies: $x->{q}\n" unless @{ $x->{r} } == 3;
  push @out, "    { tier: \"$x->{tier}\", q: " . js($x->{q}) . ", r: [\n" . join(",\n", map {
    "      [" . js($_->[0]) . ", { R: $_->[1], E: $_->[2], P: $_->[3], C: $_->[4] }]"
  } @{ $x->{r} }) . " ] }";
}
print join(",\n", @out), "\n  ];\n";
