#!/usr/bin/env perl

use strict;
use warnings;
use feature 'say';

# ------------------------------------------------------------
# tikz-visualisation.pl
#
# Generate a TikZ diagram of a Fenwick tree where:
#
# Special nodes passed on the command line are shaded.
#
# ------------------------------------------------------------
#
# Example:
#
#   diffmonger alive node=23 | xargs perl tikz-visualisation.pl 3 5 11 > tree.tex
#   latex tree.tex && dvisvgm tree.dvi --font-format=woff
#
# ------------------------------------------------------------

my @special = sort { $a <=> $b } @ARGV;

die "Provide at least one special node\n"
    unless @special;

for my $n (@special) {
    die "Invalid node index: $n\n"
        unless $n =~ /^\d+$/;
}

my %special = map { $_ => 1 } @special;

my $N = $special[-1];

# ------------------------------------------------------------
# Utilities
# ------------------------------------------------------------

sub lsb {
    my ($x) = @_;
    return $x & (-$x);
}

sub popcount {
    my ($x) = @_;

    my $count = 0;

    while ($x) {
        $count += $x & 1;
        $x >>= 1;
    }

    return $count;
}

# ------------------------------------------------------------
# Build tree structure
# ------------------------------------------------------------

my %children;

for my $i (1 .. $N) {

    my $parent = $i - lsb($i);

    push @{ $children{$parent} }, $i;
}

# Keep children ordered numerically
for my $k (keys %children) {
    @{ $children{$k} } =
        sort { $a <=> $b } @{ $children{$k} };
}

# ------------------------------------------------------------
# Determine levels by popcount
# ------------------------------------------------------------

my %level;

for my $i (0 .. $N) {
    $level{$i} = popcount($i);
}

# ------------------------------------------------------------
# Recursive subtree layout
# ------------------------------------------------------------

my %pos;

my $dx = 1.8;
my $dy = 1.8;

my $next_x = 0;

sub layout {

    my ($node) = @_;

    my @kids = @{ $children{$node} // [] };

    # --------------------------------------------------------
    # Leaf
    # --------------------------------------------------------

    if (!@kids) {

        my $x = $next_x;
        $next_x += $dx;

        my $y = -$level{$node} * $dy;

        $pos{$node} = [$x, $y];

        return $x;
    }

    # --------------------------------------------------------
    # Layout children left-to-right
    # --------------------------------------------------------

    my @child_x;

    for my $c (@kids) {
        push @child_x, layout($c);
    }

    # --------------------------------------------------------
    # Parent aligned with right-most child
    # --------------------------------------------------------

    my $x = $child_x[-1];
    my $y = -$level{$node} * $dy;

    $pos{$node} = [$x, $y];

    return $x;
}

layout(0);

# ------------------------------------------------------------
# Emit LaTeX
# ------------------------------------------------------------

say <<'EOF';
\documentclass[tikz,border=10pt]{standalone}

\usepackage{tikz}
\usetikzlibrary{arrows.meta}

\usepackage{xcolor}
\pagecolor{white}

\begin{document}

\begin{tikzpicture}[
    >=Latex,
    edge/.style={
        ->,
        thick
    },
    every node/.style={
        circle,
        draw,
        minimum size=8mm,
        inner sep=0pt,
        font=\small
    },
    special/.style={
        fill=blue!30
    }
]
EOF

# ------------------------------------------------------------
# Emit nodes
# ------------------------------------------------------------

for my $i (0 .. $N) {

    my ($x, $y) = @{ $pos{$i} };

    my $style = $special{$i}
        ? "special"
        : "";

    say sprintf(
        "\\node[%s] (n%d) at (%.2f, %.2f) {%d};",
        $style,
        $i,
        $x,
        $y,
        $i
    );
}

say "";

# ------------------------------------------------------------
# Emit edges
# ------------------------------------------------------------

for my $i (1 .. $N) {

    my $parent = $i - lsb($i);

    say sprintf(
        "\\draw[edge] (n%d) -- (n%d);",
        $i,
        $parent
    );
}

# ------------------------------------------------------------
# Finish document
# ------------------------------------------------------------

say <<'EOF';

\end{tikzpicture}

\end{document}
EOF
