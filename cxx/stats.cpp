#include "stats.h"

unsigned long long stats::flops_diff, stats::flops_bc, stats::flops_blas1;
unsigned int stats::iters_cg, stats::iters_newton;
bool stats::verbose_output;
