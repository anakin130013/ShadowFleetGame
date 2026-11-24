#!/usr/bin/perl
# Prints faction_questions.txt and favors.txt as the mockup's FQ and FAV
# arrays (drafts, 2026-10-19):  perl make_js_extra.pl > extra.js
use strict;
use warnings;
use FindBin;

my %v = ('+' => 1, '-' => -1, '0' => 0);
my (@fq, @fav);
open my $f, '<', "$FindBin::Bin/faction_questions.txt" or die $!;
while (<$f>) {
  s/\r?\n$//;
  next if /^\s*(#|$)/;
  if (/^F (\w) \| (.+)$/) { push @fq, { f => $1, q => $2, r => [] }; }
  elsif (/^A (.+?) \| ([-+0])$/) { push @{ $fq[-1]{r} }, [ $1, $v{$2} ]; }
  else { die "faction_questions.txt: can't read: $_\n"; }
}
close $f;
for my $x (@fq) { die "needs 3 replies: $x->{q}\n" unless @{ $x->{r} } == 3; }

open my $b, '<', "$FindBin::Bin/favors.txt" or die $!;
while (<$b>) {
  s/\r?\n$//;
  next if /^\s*(#|$)/ || /^H /;  # hostile lines: firmware only (the mockup has its own sample)
  /^([NL]) (\w) \| (\*?)(\w+)(?: (\w+))? \| (.+)$/ or die "favors.txt: can't read: $_\n";
  push @fav, { late => $1 eq 'L' ? 1 : 0, f => $2, hard => $3 ? 1 : 0, check => $4, arg => defined $5 ? $5 : '', text => $6 };
}
close $b;

sub js { my $s = shift; $s =~ s/\\/\\\\/g; $s =~ s/"/\\"/g; return "\"$s\""; }

print "  const FQ = [\n";
print join(",\n", map {
  my $x = $_;
  "    { f: \"$x->{f}\", q: " . js($x->{q}) . ", r: [" . join(", ", map { "[" . js($_->[0]) . ", $_->[1]]" } @{ $x->{r} }) . "] }"
} @fq), "\n  ];\n";
print "  const FAV = [\n";
print join(",\n", map {
  "    { late: $_->{late}, f: \"$_->{f}\", hard: $_->{hard}, check: \"$_->{check}\", arg: \"$_->{arg}\", text: " . js($_->{text}) . " }"
} @fav), "\n  ];\n";
