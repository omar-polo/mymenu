#!/usr/bin/env perl

use v5.10;
use warnings;
use IPC::Open3;

my $cachedir = $ENV{HOME}. "/.cache";
my $cachefile = $cachedir."/mymenu";
my %progs;

$SIG{CHLD} = "IGNORE";

if (! -d $cachedir) {
    mkdir $cachedir or die "Cannot create cache dir ". $cachedir;
}

if (-e $cachefile) {
    open my $f, "<", $cachefile;
    while (<$f>) {
	my ($c, $p) = split / /, $_;
	chomp $p;
	$progs{$p} = $c + 0;
    }
    close $f;
}

foreach my $p (split /:/, $ENV{PATH}) {
    opendir my $d, $p or next;
    foreach (readdir $d) {
	if (-x "$p/$_" and ! (-d "$p/$_")) {
	    if (! exists $progs{$_}) {
		$progs{$_} = 0;
	    }
	}
    }
    closedir $d;
}

my $args = join ' ', @ARGV;
my $pid = open3(my $c_in, my $c_out, my $c_err, "mymenu $args") or die("Failed open3(): $!");
foreach (sort {$progs{$b} <=> $progs{$a} or lc($a) cmp lc($b)} keys %progs) {
    print $c_in $_."\n";
}
close($c_in);

while (my $p = <$c_out>) {
    chomp $p;
    $progs{$p} += 1;
    say "gonna exec $p";

    if (!fork()) {
    	# i'm the child
    	exec $p;
    }
}

say "Updating cache...";
open my $f, ">", $cachefile;
foreach (keys %progs) {
    print $f $progs{$_} ." ". $_ ."\n" if $progs{$_} != 0;
}
close($f);

waitpid(-1, 0);
