#!/usr/bin/env perl
use 5.006002;
use strict;
use warnings;
use File::Spec;
use File::Temp qw(tempdir);
use FindBin qw($Bin);
use Getopt::Long qw(GetOptions);
use Text::ParseWords qw(shellwords);

# Called only when make needs a build. Probe by linking, never by executing,
# so this also works with cross compilers. No build-mode stamp is retained.
my ($cc, $cppflags, $cflags, $ldflags, $ldlibs, $output, $serial) =
   ('cc', '', '-O3 -falign-loops=32', '', '-lgmp -lm', 'msiqs', 0);
Getopt::Long::Configure(qw(no_auto_abbrev no_ignore_case));
GetOptions('cc=s' => \$cc, 'cppflags=s' => \$cppflags, 'cflags=s' => \$cflags,
           'ldflags=s' => \$ldflags, 'ldlibs=s' => \$ldlibs,
           'output=s' => \$output, 'serial' => \$serial)
  && @ARGV && length($output)
  or die "usage: build-siqs.pl [--cc COMMAND] [--cflags FLAGS] " .
         "[--cppflags FLAGS] [--ldflags FLAGS] [--ldlibs LIBS] " .
         "[--output FILE] [--serial] SOURCE ...\n";
my @compiler = shellwords($cc);
die "empty compiler command\n" unless @compiler;
my @flags = (shellwords($cppflags), shellwords($cflags));
my @link_flags = shellwords($ldflags);
my @libraries = shellwords($ldlibs);
my @thread_flags = ('-DPSIQS', '-pthread');
my ($threaded, $atomics) = (0, 0);
unless ($serial) {
  my $temporary = tempdir('siqs-probes-XXXXXX', TMPDIR => 1, CLEANUP => 1);
  my $probe = File::Spec->catfile($temporary, 'pthread-probe');
  my $atomic_probe = File::Spec->catfile($temporary, 'atomic-probe');
  my $log = File::Spec->catfile($temporary, 'probe.log');
  # Redirect the file descriptors inherited by the compiler.
  open my $stdout, '>&', \*STDOUT or die "cannot save stdout: $!\n";
  open my $stderr, '>&', \*STDERR or die "cannot save stderr: $!\n";
  open STDOUT, '>', $log or die "cannot open $log: $!\n";
  open STDERR, '>&', \*STDOUT or die "cannot redirect probe errors: $!\n";
  $threaded = system(@compiler, @flags, @thread_flags, @link_flags, '-o', $probe,
                    File::Spec->catfile($Bin, 'pthread-probe.c'), @libraries) == 0;
  $atomics = system(@compiler, @flags, @link_flags, '-o', $atomic_probe,
                   File::Spec->catfile($Bin, 'atomic-probe.c'), @libraries) == 0;
  open STDOUT, '>&', $stdout or die "cannot restore stdout: $!\n";
  open STDERR, '>&', $stderr or die "cannot restore stderr: $!\n";
}

print "Building $output ", ($threaded ? 'with pthreads' : 'serial-only'), "\n";
$| = 1;
my @command = (@compiler, @flags);
if (grep { /\A-DDMSIQS(?:=|\z)/ } @flags) {
  my $info = dmsiqs_git_info();
  push @command, '-DDMSIQS_GIT_INFO="' . $info . '"' if defined $info;
}
push @command, $threaded ? @thread_flags : '-UPSIQS';
push @command, '-DSIQS_HAVE_RF_ATOMICS=1' if $threaded && $atomics;
push @command, @link_flags, '-o', $output, @ARGV, @libraries;
print join(' ', map {shell_quote($_)} @command), "\n";
system(@command);
my $status = $?;
die "could not execute compiler: $!\n" if $status == -1;
die "SIQS build terminated by signal " . ($status & 127) . "\n"
  if $status & 127;
# A real build failure must not silently fall back to the other mode.
exit($status >> 8);

sub dmsiqs_git_info {
  my $root = File::Spec->catdir($Bin, File::Spec->updir());
  my $manifest = File::Spec->catfile($root, 'DMSIQS_BUILD');
  my $from_manifest = -f $manifest;
  my $metadata;
  if ($from_manifest) {
    open $metadata, '<', $manifest or return;
  } elsif (-e File::Spec->catfile($root, '.git')) {
    my $pid = open $metadata, '-|';
    return unless defined $pid;
    if ($pid == 0) {
      open STDERR, '>', File::Spec->devnull();
      exec 'git', '-C', $root, 'log', '-1', '--format=%H %cs' or exit 127;
    }
  } else {
    return;
  }
  local $/;
  my $line = <$metadata>;
  close $metadata or return;
  return unless defined $line &&
      $line =~ /\A([0-9a-fA-F]+) ([0-9]{4})-([0-9]{2})-([0-9]{2})(?:\r?\n)?\z/;
  my ($commit, $year, $month, $day) = ($1, $2, $3, $4);
  my $digits = length $commit;
  return unless $from_manifest ? $digits == 8 : ($digits == 40 || $digits == 64);
  return unless $month >= 1 && $month <= 12 && $day >= 1 && $day <= 31;
  my @months = qw(Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec);
  return sprintf 'git %s %d %s %s', lc(substr($commit, 0, 8)), $day, $months[$month - 1], $year;
}

# Display a copyable shell command; execution still uses the argument list.
sub shell_quote {
  my ($argument) = @_;
  return $argument if $argument =~ m{\A[-A-Za-z0-9_./:=+,@%]+\z};
  $argument =~ s/'/'\\''/g;
  return "'$argument'";
}
