#pragma once

/* Host: no DT — keep macros inert so accidental includes compile. */
#define DT_NODELABEL(x) 0
#define DT_ALIAS(x) 0
#define DT_SIZE_M(x) ((x) * 1024 * 1024)
#define DT_SIZE_K(x) ((x) * 1024)
