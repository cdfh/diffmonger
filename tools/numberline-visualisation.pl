#!/usr/bin/env perl

use strict;
use warnings;

# Read integers from stdin
my @points = @ARGV;

die "No points provided" unless @points;

# Sort and deduplicate
my %seen;

@points = sort { $a <=> $b } grep { !$seen{$_}++ } @points;

my $max = $points[-1];

# Generate lower ticks at:
#   x - 0, x - 2^0, x - 2^1, x - 2^2, ...
# stopping before negative values.
my @lower_ticks;

{
    my %lt_seen;

    my $k = $max;
    push @lower_ticks, $k;
    $lt_seen{$k} = 1;

    my $i = 0;
    while (1) {
        my $v = $max - (2 ** $i);
        last if $v < 0;

        if (!$lt_seen{$v}) {
            push @lower_ticks, $v;
            $lt_seen{$v} = 1;
        }

        $i++;
    }

    @lower_ticks = sort { $a <=> $b } @lower_ticks;
}

# Avoid division by zero if only 0 is present
my $display_width = 12;  # cm
my $scale = ($max > 0) ? ($display_width / $max) : 1;

print <<'EOF';
\documentclass[tikz,border=5pt]{standalone}
\usepackage{tikz}

\usepackage{xcolor}
\pagecolor{white}

\begin{document}
\begin{tikzpicture}
EOF

# Draw horizontal line
printf "\\draw[thick] (0,0) -- (%.6f,0);\n", $max * $scale;

# Upper ticks: original input points
for my $p (@points) {
    my $x = $p * $scale;
    printf "\\draw[thick] (%.6f,0) -- (%.6f,0.3);\n", $x, $x;
}

# Lower ticks: x - 2^i sequence
for my $p (@lower_ticks) {
    my $x = $p * $scale;
    printf "\\draw[thick] (%.6f,0) -- (%.6f,-0.3);\n", $x, $x;
}

print <<'EOF';
\end{tikzpicture}
\end{document}
EOF
