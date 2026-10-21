/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SHELL_PROMPT_H
#define RIPPLE_SHELL_PROMPT_H
/* Static storage: formatting must not allocate from the shell's brk arena. */
const char *shell_prompt_expand(const char *specification, const char *home);
unsigned shell_prompt_columns(const char *text);
const char *shell_prompt_tail(const char *text);
/* Private OSC restart metadata for mux; NULL argv reports an idle shell. */
void shell_mux_report(const char **argv);
#endif
