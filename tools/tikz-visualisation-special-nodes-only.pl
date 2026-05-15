#!/usr/bin/env perl

use strict;
use warnings;
use feature 'say';

# ------------------------------------------------------------
# Like tikz-visualisation.pl, but only shows the selected nodes.
# ------------------------------------------------------------

my @special = sort { $a <=> $b } @ARGV;

die "Provide at least one node\n"
    unless @special;

for my $n (@special) {
    die "Invalid node: $n\n"
        unless $n =~ /^\d+$/;
}

my %special = map { $_ => 1 } @special;

my $N = $special[-1];

# ------------------------------------------------------------
# bit helpers
# ------------------------------------------------------------

sub lsb {
    my ($x) = @_;
    return $x & (-$x);
}

sub popcount {
    my ($x) = @_;
    my $c = 0;
    while ($x) {
        $c += $x & 1;
        $x >>= 1;
    }
    return $c;
}

# ------------------------------------------------------------
# keep closure: selected nodes + all ancestors
# ------------------------------------------------------------

my %keep;

for my $s (@special) {
    my $x = $s;

    while (1) {
        $keep{$x} = 1;
        last if $x == 0;
        $x -= lsb($x);
    }
}

# ------------------------------------------------------------
# build pruned adjacency list
# ------------------------------------------------------------

my %children;

for my $i (1 .. $N) {

    next unless $keep{$i};

    my $p = $i - lsb($i);

    next unless $keep{$p};

    push @{ $children{$p} }, $i;
}

for my $k (keys %children) {
    @{ $children{$k} } = sort { $a <=> $b } @{ $children{$k} };
}

# ------------------------------------------------------------
# level assignment
# ------------------------------------------------------------

my %level;

for my $i (0 .. $N) {
    next unless $keep{$i};
    $level{$i} = popcount($i);
}

# ------------------------------------------------------------
# layout (right-aligned subtree DFS)
# ------------------------------------------------------------

my %pos;

my $dx = 1.8;
my $dy = 1.8;

my $next_x = 0;

sub layout {

    my ($node) = @_;

    my @kids = @{ $children{$node} // [] };

    # leaf
    if (!@kids) {

        my $x = $next_x;
        $next_x += $dx;

        $pos{$node} = [$x, -$level{$node} * $dy];

        return $x;
    }

    # children first
    my @xs;

    for my $c (@kids) {
        push @xs, layout($c);
    }

    # align parent with right-most child
    my $x = $xs[-1];

    $pos{$node} = [$x, -$level{$node} * $dy];

    return $x;
}

layout(0);

# ------------------------------------------------------------
# output
# ------------------------------------------------------------

say <<'EOF';
\documentclass[tikz,border=10pt]{standalone}

\usepackage{tikz}
\usetikzlibrary{arrows.meta}

\begin{document}

\begin{tikzpicture}[
    >=Latex,
    edge/.style={->, thick},
    node/.style={circle, draw, minimum size=8mm, inner sep=0pt, font=\small},
    special/.style={fill=blue!30}
]
EOF

# ------------------------------------------------------------
# nodes
# ------------------------------------------------------------

for my $i (0 .. $N) {

    next unless $keep{$i};

    my ($x, $y) = @{ $pos{$i} };

    my $style = $special{$i}
        ? "node,special"
        : "node";

    say sprintf(
        "\\node[%s] (n%d) at (%.2f, %.2f) {%d};",
        $style, $i, $x, $y, $i
    );
}

say "";

# ------------------------------------------------------------
# edges (no self-loops)
# ------------------------------------------------------------


for my $i (1 .. $N) {

    next unless $keep{$i};

    my $p = $i - lsb($i);

    next unless $keep{$p};

    next if $i == $p;

    next unless exists $pos{$i} && exists $pos{$p};

    my ($x1, $y1) = @{ $pos{$i} };
    my ($x2, $y2) = @{ $pos{$p} };

    # final guard: skip degenerate edges
    next if $x1 == $x2 && $y1 == $y2;

    say "\\draw[edge] (n$i) -- (n$p);";
}

say <<'EOF';

\end{tikzpicture}

\end{document}
EOF
