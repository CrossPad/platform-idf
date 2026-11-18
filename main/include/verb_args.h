/**
 * @file verb_args.h
 * @brief Matching a CDC verb by its whole name and reaching its arguments.
 */
#pragma once

#include <string.h>

/* The text after `verb` (a space and the arguments, or an empty string), or
 * NULL when `cmd` is not that verb: FW_CHECKX is not FW_CHECK. */
static inline const char *args_of(const char *cmd, const char *verb)
{
    const size_t n = strlen(verb);
    if (strncmp(cmd, verb, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' ')) return NULL;
    return cmd + n;
}

/* args_of() with the leading spaces skipped. */
static inline const char *args_of_trimmed(const char *cmd, const char *verb)
{
    const char *a = args_of(cmd, verb);
    if (a) {
        while (*a == ' ') ++a;
    }
    return a;
}
