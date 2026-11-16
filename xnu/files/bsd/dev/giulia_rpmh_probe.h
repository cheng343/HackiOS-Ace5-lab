/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_RPMH_PROBE_H
#define GIULIA_RPMH_PROBE_H
#include <stdint.h>
/* Only the finite, single-CPU Ace5 mode4 entry calls this function. */
void giulia_rpmh_read_test_start(uint64_t quiet_pa);
#endif
