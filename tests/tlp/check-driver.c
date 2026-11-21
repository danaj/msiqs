#if defined(PSIQS) && (!defined(SIQS_RESIDUAL_FIRST) || SIQS_RESIDUAL_FIRST)
#include "check-residual-first.c"
#else
#include "check.c"
#endif
