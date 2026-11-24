#!/usr/bin/perl
# Builds a readable HTML page of the whole transmission bank from the five
# text files here (rebel / empire / pyke / crimson / big .txt).
# Usage: perl make_page.pl > bank.html
use strict; use warnings;
my $dir = $0; $dir =~ s{[^/\\]*$}{};
my %FN = (R => ['REBEL ALLIANCE', 'rebel', '--rebel'], E => ['GALACTIC EMPIRE', 'empire', '--empire'],
          P => ['PYKE SYNDICATE', 'pyke', '--pyke'], C => ['CRIMSON DAWN', 'crimson', '--crimson']);
my @ORDER = qw(R E P C);
my %CAUSE = (took => 'You took their planet', betray => 'Betrayal', rival => 'You hit their rival', paid => 'You paid for it',
             deal => 'You made a deal', walkout => 'You walked out', kept => 'You kept a promise', broke => 'You broke a promise');
my @CAUSES = qw(took betray rival paid deal walkout kept broke);
my %MOMENT = (firstblood => 'First blood', seat => 'Seat of power', core => 'The Core falls', allied => 'Allied',
              betrayal => 'Betrayal', oathbreaker => 'Oathbreaker', fleet => 'Fleet destroyed', lastround => 'Last round');
my @MOMENTS = qw(firstblood seat core allied betrayal oathbreaker fleet lastround);
sub esc { my $t = shift; $t =~ s/&/&amp;/g; $t =~ s/</&lt;/g; $t =~ s/>/&gt;/g; $t =~ s/\{planet\}/<em>planet<\/em>/g; return $t; }
sub slurp { my $f = shift; open(my $h, '<:encoding(UTF-8)', "$dir$f") or die "$f: $!"; local $/; my $t = <$h>; return $t; }

my ($body, $total, $toc) = ('', 0, '');
for my $k (@ORDER) {
  my ($name, $file, $var) = @{$FN{$k}};
  my (%lines, @planet);
  for (split /\n/, slurp("$file.txt")) {
    next if /^\s*(#|$)/;
    if (/^@([A-Z ]+):\s*(.*)$/) { push @planet, [$1, $2]; next; }
    if (/^([a-z]+):\s*(.*)$/) { push @{$lines{$1}}, $2; }
  }
  $toc .= "<a href=\"#$file\" style=\"color: var($var)\">$name</a>";
  $body .= "<section id=\"$file\"><h2 style=\"color: var($var)\">$name</h2>\n";
  for my $c (@CAUSES) {
    my $n = 0;
    $body .= "<h3>$CAUSE{$c}</h3><ol>\n";
    for my $l (@{$lines{$c} || []}) { $n++; $total++; $body .= "<li><span class=\"id\">\U$k\E-$c-$n</span><q>" . esc($l) . "</q></li>\n"; }
    $body .= "</ol>\n";
  }
  if (@planet) {
    $body .= "<h3>Planet lines</h3><ol>\n";
    for my $p (@planet) { $total++; $body .= "<li><span class=\"id\">$p->[0]</span><q>" . esc($p->[1]) . "</q></li>\n"; }
    $body .= "</ol>\n";
  }
  $body .= "</section>\n";
}
my %big;
for (split /\n/, slurp('big.txt')) { next if /^\s*(#|$)/; push @{$big{$1}{$2}}, $3 if /^([a-z]+) ([REPC]):\s*(.*)$/; }
$toc .= "<a href=\"#big\" style=\"color: var(--gold)\">BIG MOMENTS</a>";
$body .= "<section id=\"big\"><h2 style=\"color: var(--gold)\">BIG MOMENTS</h2>\n";
for my $m (@MOMENTS) {
  $body .= "<h3><span class=\"prio\">PRIORITY</span> $MOMENT{$m}</h3><ol>\n";
  for my $k (@ORDER) { my $n = 0; for my $l (@{$big{$m}{$k} || []}) { $n++; $total++;
    $body .= "<li><span class=\"id\" style=\"color: var($FN{$k}[2])\">$k-$m-$n</span><q>" . esc($l) . "</q></li>\n"; } }
  $body .= "</ol>\n";
}
$body .= "</section>\n";

print <<"HTML";
<title>Transmission Bank</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Oxanium:wght\@500;600;700&family=Barlow+Semi+Condensed:ital,wght\@0,400;0,500;0,600;1,400;1,500&display=swap">
<style>
/* Layout: one reading column with a sticky faction jump bar. Committed dark look, like the game. */
:root {
  --bg: #05070C; --line: #18263A; --text: #E6F1FA; --dim: #7C93A8; --holo: #CFF4FF; --cyan: #5FD4FF; --gold: #FFD166;
  --rebel: #FFC933; --empire: #A9B4C0; --pyke: #B98CF0; --crimson: #E0707A;
  --display: 'Oxanium', 'Segoe UI', system-ui, sans-serif; --body: 'Barlow Semi Condensed', 'Segoe UI', system-ui, sans-serif;
  color-scheme: dark;
}
body { background: var(--bg); color: var(--text); font-family: var(--body); font-size: 17px; line-height: 1.45; }
.wrap { max-width: 860px; margin: 0 auto; padding-inline: 18px; padding-block: 36px 64px; }
header { display: flex; flex-direction: column; gap: 10px; margin-bottom: 18px; }
.eyebrow { font-family: var(--display); font-size: 12px; letter-spacing: 4px; color: var(--cyan); }
h1 { font-family: var(--display); font-size: clamp(30px, 6vw, 44px); letter-spacing: 3px; margin: 0; }
.intro { color: var(--dim); max-width: 64ch; margin: 0; }
nav { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 2; background: var(--bg); display: flex; flex-wrap: wrap; gap: 6px 18px;
      padding-block: 10px; border-bottom: 1px solid var(--line); font-family: var(--display); font-size: 13px; letter-spacing: 2px; }
nav a { text-decoration: none; }
section { padding-top: 26px; }
h2 { font-family: var(--display); font-size: 22px; letter-spacing: 3px; margin: 0 0 4px; }
h3 { font-family: var(--display); font-size: 12px; letter-spacing: 3px; text-transform: uppercase; color: var(--dim); margin: 22px 0 6px; display: flex; gap: 10px; align-items: center; }
ol { list-style: none; margin: 0; padding: 0; }
li { display: grid; grid-template-columns: 128px minmax(0, 1fr); gap: 12px; padding: 7px 0; border-bottom: 1px solid #0F1A27; align-items: baseline; }
.id { font-family: var(--display); font-size: 11px; letter-spacing: 1px; color: #56697C; font-variant-numeric: tabular-nums; }
q { font-style: italic; color: var(--holo); quotes: '\\201C' '\\201D'; }
q em { font-style: italic; color: var(--text); background: rgba(95, 212, 255, 0.10); padding: 0 3px; border-radius: 3px; }
.prio { font-size: 10px; letter-spacing: 2px; color: var(--gold); border: 1px solid var(--gold); border-radius: 10px; padding: 1px 7px; }
\@media (max-width: 560px) { li { grid-template-columns: minmax(0, 1fr); gap: 2px; } }
</style>
<div class="wrap">
<header>
  <span class="eyebrow">SHADOW FLEET · FACTION TRANSMISSIONS</span>
  <h1>Transmission bank</h1>
  <p class="intro">All $total lines, in the voices you approved on the tone sample. "planet" marks where the game fills in the planet's name. Each faction's lines for a cause are shuffled like a deck and the board remembers its place across games, so a line only comes back after the whole deck has been used. Planet lines play first when the planet matches. Big moments fire at most once per player per game. To change a line, quote its ID (for example P-broke-2).</p>
</header>
<nav>$toc</nav>
$body
</div>
HTML
