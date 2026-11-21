#!/usr/bin/env perl
use 5.010;
use strict;
use warnings;
use File::Basename qw(dirname);
use File::Spec;
use FindBin qw($Bin);
use Getopt::Long qw(GetOptions);

# Read the selected policy table from source. The size formulas
# below mirror siqs_select_parameters; no factoring or module build is needed.
my ($details, $describe, $exact, $help, $header_path, $tlp, $two_lp);
Getopt::Long::Configure(qw(no_auto_abbrev no_ignore_case));
GetOptions('detail|details' => \$details,
           'describe|verbose|v' => \$describe,
           'exact' => \$exact,
           '2lp' => \$two_lp,
           '3lp' => \$tlp,
           'help' => \$help,
           'header=s' => \$header_path) or usage(2);
usage(0) if $help;
usage(2) if $tlp && $two_lp;
usage(2) if @ARGV > 1;
my $source_path = shift @ARGV // File::Spec->catfile($Bin, '..', 'siqs.c');
$header_path //= File::Spec->catfile(dirname($source_path), 'siqs.h');
my $source = read_source($source_path);
my $header = read_source($header_path);

my %define;
my $definitions = "$header\n$source";
while ($definitions =~ /^\s*#\s*define\s+(\w+)[ \t]+([^\n]+)/mg) {
  $define{$1} = $2;
}
my $number = qr/(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/;
my $align = numeric('SIQS_SIEVE_ALIGN');
my $srange = '-';  # text for ranges, e.g. ' -> ' or '-' or '..' or '->'
die "invalid SIQS_SIEVE_ALIGN\n" unless $align > 0 && int($align) == $align;

# Extract these constants from the implementation rather than duplicating
# them.  Reject an unfamiliar formula instead of silently reporting old sizes.
my ($interval_offset, $interval_exponent) = $source =~
  /interval\s*=\s*($number)\s*\+\s*exp\(\s*($number)\s*\*\s*ln_term\s*\)/;
my ($interval_floor) = $source =~
  /if\s*\(floor\s*==\s*0\)\s*floor\s*=\s*(\d+)\s*;/;
die "unrecognized SIQS size formulas in $source_path\n"
  unless defined($interval_offset) && defined($interval_floor) &&
         $source =~ /ln_n\s*=\s*p->bits\s*\*\s*M_LN2\s*;/ &&
         $source =~ /ln_term\s*=\s*sqrt\(ln_n\s*\*\s*log\(ln_n\)\)\s*;/ &&
         $source =~ /fb\s*=\s*exp\(p->fb_coefficient\s*\*\s*ln_term\)\s*;/;

my ($struct) = $source =~
  /typedef\s+struct\s*\{([^{}]*)\}\s*siqs_policy_band_t\s*;/s;
my $table = $tlp ? 'siqs_tlp_policy_bands' : 'siqs_policy_bands';
my ($array) = $source =~
  /\b\Q$table\E\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;/s;
die "cannot find siqs_policy_band_t and $table in $source_path\n"
  unless defined($struct) && defined($array);
my ($cutoff_numerator, $cutoff_denominator);
if ($tlp) {
  # These symbolic bounds need the actual FB prime P. Check the formulas
  # before describing them, just as for the size formulas above.
  my $bounds = $source;
  my $bounds_path = $source_path;
  if ($source =~ /\bsiqs_tlp_set_bounds\s*\(\s*ctx\s*\)\s*;/) {
    $bounds_path = File::Spec->catfile(dirname($source_path), 'siqs_3lp_inc.c');
    $bounds = read_source($bounds_path);
  }
  $bounds =~ s/\s+//g;
  die "unrecognized TLP bound formulas in $bounds_path\n"
    unless index($bounds, 'siqs_mpz_set_u64(ctx->tlp_upper,p->large_prime_bound);' .
      ('mpz_mul_ui(ctx->tlp_upper,ctx->tlp_upper,(unsignedlong)pmax);' x 2)) >= 0 &&
      index($bounds, 'mpz_root(root,ctx->tlp_upper,3);' .
      'mpz_mul_ui(root,root,SIQS_TLP_CUTOFF_NUMERATOR);' .
      'mpz_fdiv_q_ui(root,root,SIQS_TLP_CUTOFF_DENOMINATOR);') >= 0;
  $cutoff_numerator = numeric('SIQS_TLP_CUTOFF_NUMERATOR');
  $cutoff_denominator = numeric('SIQS_TLP_CUTOFF_DENOMINATOR');
  die "invalid TLP cutoff fraction\n"
    unless $cutoff_numerator > 0 && int($cutoff_numerator) == $cutoff_numerator &&
           $cutoff_denominator > 0 && int($cutoff_denominator) == $cutoff_denominator;
}
my @fields;
for my $field (split /;/, $struct) {
  next unless $field =~ /\S/;
  $field =~ /(\w+)\s*$/ or die "unrecognized band field: $field\n";
  push @fields, $1;
}
my %field_index = map { $fields[$_] => $_ } 0 .. $#fields;
my $required_fields = @fields;
$required_fields -= 3
  if join(',', @fields[-3 .. -1]) eq 'fb_scale,lp_scale,r2_scale';
my @bands;
for my $row (split_top_level($array)) {
  $row =~ /^\s*\{(.*)\}\s*$/s or die "unrecognized band row: $row\n";
  my (@values, $next);
  $next = 0;
  for my $value (split_top_level($1)) {
    # C permits designated overrides after positional initializers. Any
    # following positional value continues at the next field.
    if ($value =~ /^\.(\w+)\s*=\s*(.*)$/s) {
      die "unknown band field: $1\n" unless exists $field_index{$1};
      ($next, $value) = ($field_index{$1}, $2);
    }
    die "too many values in band row\n" if $next >= @fields;
    $values[$next++] = $value;
  }
  for my $index (0 .. $#fields) {
    next if defined $values[$index];
    die "missing band field: $fields[$index]\n" if $index < $required_fields;
    $values[$index] = 0;  # Omitted optional scales have C's zero default.
  }
  my %band;
  for my $index (0 .. $#fields) {
    my $value = $values[$index];
    if ($fields[$index] eq 'name') {
      $value =~ /^"([^"\\]*)"$/ or die "invalid band name: $value\n";
      $value = $1;
    } elsif ($value eq 'SIQS_R_DERIVED_FROM_LP') {
      $value = 'derived';
    } elsif ($value =~ /^SIQS_POLICY_(LINEAR|RAMP)\((.*)\)$/s) {
      my ($kind, $arguments) = ($1, $2);
      my @args = split_top_level($arguments);
      die "invalid $kind policy: $value\n" unless @args == ($kind eq 'LINEAR' ? 3 : 4);
      $value = {kind => $kind, args => [map { numeric($_) } @args]};
    } elsif ($value =~ /^SIQS_POLICY_RATIO\((.*)\)$/s) {
      my @args = split_top_level($1);
      die "invalid ratio policy: $value\n" unless @args == 4;
      $value = [map { numeric($_) } @args];
    } else {
      $value = numeric($value);
    }
    $band{$fields[$index]} = $value;
  }
  die "invalid bit range in $band{name}\n"
    unless $band{first_bits} >= 1 && $band{last_bits} >= $band{first_bits};
  die "unsupported LP count in $band{name}\n"
    unless ($tlp ? $band{production_large_primes} == 3
                 : $band{production_large_primes} == 1 ||
                   $band{production_large_primes} == 2);
  push @bands, \%band;
}
die "no bands found in $table\n" unless @bands;

my @headings = $tlp ? ('bits', 'q', 'M', 'FB', 'LP', 'R2')
                    : ('bits', 'type', 'q', 'LP', 'M', 'FB', 'res');
push @headings, ('bias', 'free', 'pmin', 'rel +') if $details;
push @headings, ('FBx', 'LPx', 'R2x') if $tlp && $details;
my @rows;
for my $band (@bands) {
  my ($first, $last) = @{$band}{qw(first_bits last_bits)};
  my @sizes = map { sizes_at($band, $_) } ($first, $last);
  my ($lp, $residual, $type);
  my @k = map { curve_at($band->{k_l}, $_) * policy_scale($band, 'lp_scale') }
          ($first, $last);
  die "invalid K_L in $band->{name}\n" if grep { $_ < 1 } @k;
  $type = $band->{production_large_primes} == 3 ? '3LP'
        : $band->{production_large_primes} == 2 ? '2LP'
        : $k[0] == 1 && $k[1] == 1 ? 'smooth' : '1LP';
  @k = round_range_vals(@k) unless $tlp;
  $lp = coefficient_range('P', @k);
  if (!ref($band->{k_r}) && $band->{k_r} eq 'derived') {
    $residual = $band->{production_large_primes} >= 2 ? 'LP*P' : 'LP';
  } else {
    my @r = map { curve_at($band->{k_r}, $_) } ($first, $last);
    die "invalid K_R in $band->{name}\n" if grep { $_ <= 0 } @r;
    @r = round_range_vals(@r) unless $tlp;
    $residual = coefficient_range($band->{production_large_primes} >= 2 ? 'P^2' : 'P', @r);
  }
  my $r2_scale = policy_scale($band, 'r2_scale');
  $residual = sprintf('%.6g*(%s)', $r2_scale, $residual) if $r2_scale != 1;
  my @row = $tlp
    ? (sprintf('%3d-%-3d', $first, $last), $band->{q_count},
       range_text($sizes[0][0], $sizes[1][0], !$exact),
       range_text($sizes[0][1], $sizes[1][1], !$exact), $lp, $residual)
    : (sprintf('%3d-%-3d', $first, $last), $type, $band->{q_count},
             $lp, range_text($sizes[0][0], $sizes[1][0], !$exact),
             range_text($sizes[0][1], $sizes[1][1], !$exact), $residual);
  if ($details) {
    my @bias = map {
      $band->{stage1_bias_base} + ($band->{stage1_bias_step_bits}
        ? int(($_ - $band->{stage1_bias_origin_bits}) /
              $band->{stage1_bias_step_bits}) : 0)
    } ($first, $last);
    push @row, range_text(@bias), $band->{sieve_free_units},
      $band->{sieve_start_prime_floor} || '-', $band->{relation_extra};
    push @row, map { sprintf('%.6g', policy_scale($band, $_)) }
                   qw(fb_scale lp_scale r2_scale) if $tlp;
  }
  push @rows, \@row;
}
my @width = map { length($_) } @headings;
for my $row (@rows) {
  for my $column (0 .. $#width) {
    $width[$column] = length($row->[$column])
      if length($row->[$column]) > $width[$column];
  }
}
my $format = join(' | ', map { "%-${_}s" } @width) . "\n";
if ($describe) {
  say 'bits:   Bit range of the post-trial cofactor N, before multiplying by k.';
  say 'type:   Smooth, or up to one/two large primes per raw relation (1LP/2LP).'
    unless $tlp;
  say 'q:      Number of distinct factor-base primes used in polynomial A.';
  say $tlp
    ? 'LP:     Maximum individual large prime, from scaled K_L times the largest FB prime P.'
    : 'LP:     Maximum individual large prime, from K_L times the actual prime P.';
  say 'M:      Half the sieve interval; the sieve covers 2M positions.';
  say 'FB:     Number of primes used in the factor base.';
  say $tlp
    ? 'R2:     Two-prime product bound; an outer scale applies after base rounding/capping.'
    : 'res:    Maximum cofactor after factor-base division (a product in 2LP).';
  if ($details) {
    say 'bias:           Additional coarse candidate-filter headroom (sieve-byte units).';
    say 'free:           Sieve-byte rounding reserve (sieve_free_units).';
    say 'pmin:           Lower bound for the first sieved prime, not its actual value.';
    say 'rel +:          Initial relation surplus above FB + 1; readiness may stop earlier.';
    say 'FBx/LPx/R2x:    Policy scale overrides; zero/omitted means 1. FB/LP columns include them.'
      if $tlp;
  }
  say '';
}
printf $format, @headings;
say join('-+-', map { '-' x $_ } @width);
printf $format, @$_ for @rows;
if ($tlp) {
  say "\nR3 = LP*P^2; U = floor($cutoff_numerator*floor(cbrt(R3))/$cutoff_denominator).";
}
#say "\nM/FB: 3 significant digits; k=1000, M=1e6, G=1e9. Use --exact for integers."
say "\nUse --exact for integers, --details for more columns, --describe for info."
  unless $exact;
if ($details) {
  say "\nM and FB show first -> last endpoint values, not sorted extrema; sieve length is 2M.";
  say $tlp ? 'P is the largest factor-base prime.'
           : 'P is the largest factor-base prime; smooth rows allow no primes above P.';
  if ($tlp) {
    say 'LP coefficients include LPx; fractional coefficients are preserved.';
    say 'LP is capped at SIQS_LP_MAX; R2 uses K_R*P^2 or coupled LP*P, then R2x.';
    say 'R2 is capped at SIQS_RESIDUAL_PRODUCT_MAX before and after its scale.';
    say 'Only TLP bands are shown; uncovered sizes fall back to the ordinary table.';
  } else {
    say 'LP coefficients show first -> last values of K_L; products are rounded down.';
    say 'LP is capped at SIQS_LP_MAX before coupled R is derived: LP in 1LP, LP*P in 2LP.';
    say 'Explicit K_R uses P in 1LP or P^2 in 2LP; R is capped at SIQS_RESIDUAL_PRODUCT_MAX.';
    say 'Primary production bands only; low-end recovery profiles are not shown.';
  }
  say 'At the unused 1-bit SIQS endpoint, the FB floor is shown (pretests return first).'
  if $bands[0]{first_bits} == 1;
    say 'pmin is a floor, not the actual first sieved prime; rel + is the initial surplus.'
}

sub usage {
  my ($status) = @_;
  my $out = $status ? *STDERR : *STDOUT;
  print {$out} "Usage: perl $0 [--2lp|--3lp] [--describe] [--details] [--exact] [--header siqs.h] [siqs.c]\n",
    "  Default source: siqs.c in the SIQS project directory.\n",
    "  --2lp summarizes the ordinary table (the default); --3lp summarizes the 3LP table.\n",
    "  --describe prints column descriptions above the table (aliases: --verbose, -v).\n",
    "  M/FB are compact by default (three significant digits, decimal k/M/G).\n",
    "  --exact displays M/FB as comma-separated integers instead.\n",
    "  --details adds byte-sieve bias/free units, prime floor, and relation surplus.\n",
    "  --header supplies the header defining the bit limits (default: beside siqs.c).\n";
  exit $status;
}

sub read_source {
  my ($path) = @_;
  open my $in, '<', $path or die "$path: $!\n";
  local $/;
  my $text = <$in>;
  close $in or die "$path: $!\n";
  # Preserve quoted strings while removing C comments and line continuations.
  $text =~ s{("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|/\*.*?\*/|//[^\n]*}
            {defined($1) ? $1 : ' '}gse;
  $text =~ s/\\\r?\n/ /g;
  return $text;
}

sub numeric {
  my ($expression, $depth) = @_;
  die "recursive numeric macro: $expression\n" if ($depth // 0) > 20;
  # Only numeric tokens/operators reach eval; source text cannot execute Perl.
  $expression =~ s/\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b/$1/g;
  $expression =~ s{\b([A-Za-z_]\w*)\b}{
    exists($define{$1}) ? '(' . numeric($define{$1}, ($depth // 0) + 1) . ')'
                       : die("unknown numeric macro: $1\n")}ge;
  $expression =~ s/\b0[xX]([0-9a-fA-F]+)\b/hex($1)/ge;
  die "unsupported numeric expression: $expression\n"
    unless $expression =~ /\A(?:$number|[\s()+*\/<>-])+\z/;
  my $value = eval $expression;
  die "invalid numeric expression: $expression ($@)\n" if $@ || !defined($value);
  die "nonfinite numeric expression: $expression\n"
    unless $value == $value && abs($value) <= 1.7976931348623157e308;
  return $value;
}

sub split_top_level {
  my ($text) = @_;
  my (@parts, @close);
  my %matching = ('(' => ')', '{' => '}', '[' => ']');
  my $start = 0;
  while ($text =~ /"(?:\\.|[^"\\])*"|[(){}\[\],]/g) {
    my $token = $&;
    if (exists $matching{$token}) {
      push @close, $matching{$token};
    } elsif ($token =~ /^[)}\]]$/) {
      die "unbalanced policy expression\n" unless @close && pop(@close) eq $token;
    } elsif ($token eq ',' && !@close) {
      push @parts, substr($text, $start, $-[0] - $start);
      $start = $+[0];
    }
  }
  die "unbalanced policy expression\n" if @close;
  push @parts, substr($text, $start);
  s/^\s+|\s+$//g for @parts;
  pop @parts if @parts && $parts[-1] eq '';
  return @parts;
}

sub curve_at {
  my ($curve, $bits) = @_;
  die "derived R is not a numeric curve\n" unless ref($curve) eq 'HASH';
  my ($begin, $step_or_end, $first, $last) = @{$curve->{args}};
  return $begin + $step_or_end * ($bits - $first) if $curve->{kind} eq 'LINEAR';
  die "invalid endpoint policy ramp\n"
    if $first > $last || ($first == $last && $begin != $step_or_end);
  die "cannot extrapolate policy ramp to $bits bits\n" if $bits < $first || $bits > $last;
  return $begin if $bits == $first;
  return $step_or_end if $bits == $last;
  return $begin + ($step_or_end - $begin) * (($bits - $first) / ($last - $first));
}

sub sizes_at {
  my ($band, $bits) = @_;
  # ln(N)*ln(ln(N)) is negative at one bit, but SIQS never runs there.
  my $ln_n = $bits * numeric('M_LN2');
  my $ln_term = $ln_n >= 1 ? sqrt($ln_n * log($ln_n)) : 0;
  my $fb = exp(curve_at($band->{fb_coefficient}, $bits) * $ln_term);
  $fb *= policy_scale($band, 'fb_scale');
  $fb = $band->{fb_floor} if $fb < $band->{fb_floor};
  my $m = $band->{fixed_half_interval};
  if (!$m) {
    my ($amount, $numerator, $step, $denominator) = @{$band->{interval_lp_scale}};
    my $ratio = $denominator
      ? 1 + $amount * ($numerator + $step * ($bits - $band->{first_bits})) / $denominator
      : 1;
    my $scale = curve_at($band->{interval_base_scale}, $bits) * $ratio;
    my $interval = ($interval_offset + exp($interval_exponent * $ln_term)) * $scale;
    die "negative interval in $band->{name} at $bits bits\n"
      if $interval < 0 && !$band->{half_interval_floor};
    my $floor = $band->{half_interval_floor} || $interval_floor;
    $interval = $floor if $interval < $floor;
    $m = int((int($interval) + $align - 1) / $align) * $align;
  }
  return [$m, int($fb)];
}

sub policy_scale {
  my ($band, $field) = @_;
  my $scale = $band->{$field} || 1;
  die "invalid $field in $band->{name}\n" unless $scale > 0;
  return $scale;
}

sub round_range_vals {
  my($first, $last) = @_;
  $first = int($first + 0.5) if $first != int($first);
  $last  = int($last  + 0.5) if $last  != int($last );
  return ($first,$last);
}

sub coefficient_range {
  my ($unit, $first, $last) = @_;
  my @text = map { $_ == 1 ? "" : sprintf('%.6g', $_) } ($first, $last);
  my $out = $text[0] eq $text[1] ? $text[0] : "$text[0]${srange}$text[1]";
  return $out.$unit;
}

sub integer_text {
  my ($value) = @_;
  my $text = sprintf('%.0f', $value);
  $text =~ s/(?<=\d)(?=(?:\d{3})+$)/,/g;
  return $text;
}

sub range_text {
  my ($first, $last, $compact) = @_;
  my @text = map { $compact ? compact_integer_text($_) : integer_text($_) }
             ($first, $last);
  return $text[0] eq $text[1] ? $text[0] : "$text[0]${srange}$text[1]";
}

sub compact_integer_text {
  my ($value) = @_;
  return integer_text($value) if $value < 1000;
  my @suffix = ('', 'k', 'M', 'G');
  my $unit = 0;
  while ($value >= 1000 && $unit < $#suffix) {
    $value /= 1000;
    $unit++;
  }
  # Round before choosing decimals, including carries across a unit boundary.
  $value = 0 + sprintf('%.3g', $value);
  if ($value >= 1000 && $unit < $#suffix) {
    $value /= 1000;
    $unit++;
  }
  my $decimals = $value < 10 ? 2 : $value < 100 ? 1 : 0;
  return sprintf('%.*f%s', $decimals, $value, $suffix[$unit]);
}
