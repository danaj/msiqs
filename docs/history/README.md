# Source history

The project was extracted from Math::Prime::Util::GMP, including historical
root-level sources and their later moves. TinySIQS128 history was extracted
from Math::Prime::Util and merged separately. The maps retain original and
filtered commit identifiers; source revisions in historical benchmarks refer
to the original repositories. Unrelated commits and Perl release tags were
not imported. Historical snapshots are not promised to build independently.

The current project is MIT licensed. Preserved historical revisions retain
their original copyright and license notices. The old FLINT-derived Lanczos
source and header are deliberately excluded from this repository's history,
including their intermediate versions. The independent solver's history is
retained from its introduction as `nlanczos.c` (original GMP revision
4c12e554), through its replacement of the old solver (ba25022), and onward.
The current source documents Montgomery's algorithm and msieve's influence.

The original module briefly contained the FLINT-derived solver, but did not
release it. Removing that code from this extracted history does not change
its upstream history or the acknowledgements in the current implementation.
The commit maps above account for this exclusion; historical snapshots that
used the excluded solver will not contain that source or its header.

SQUFOF acknowledges Ben Buhrow's racing implementation, distributed with
an explicit public-domain dedication in YAFU, and earlier work by Jason
Papadopoulos and others. Existing mathematical and implementation credits
remain in the source. MIT licensing here does not change the licenses of
the original Perl projects or of linked GMP installations.

The original module license texts are retained alongside these notes for
historical context. They do not replace the current root MIT license.
