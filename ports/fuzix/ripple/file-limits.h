/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FILE_LIMITS_H
#define RIPPLE_FILE_LIMITS_H
/* Shared by RVM and native kernels. The small physical BF kernel keeps its
 * independent profile. Select v2 exposes these descriptors to rebuilt apps. */
#define RIPPLE_FILE_LIMIT 128
#define RIPPLE_OPEN_FILE_LIMIT 1024
#endif
