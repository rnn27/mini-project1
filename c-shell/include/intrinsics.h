#ifndef INTRINSICS_H
#define INTRINSICS_H

#include <stddef.h>

/*
 * Return non-zero if command is a shell intrinsic handled
 * directly by the shell process.
 */
int is_intrinsic(const char *command);

/*
 * Execute an intrinsic in the shell process.
 *
 * argv follows the usual convention:
 * argv[0] = command name
 * argv[argc] = NULL
 *
 * Returns 0 on success, -1 on failure.
 */
int execute_intrinsic(int argc, char *const argv[]);

#endif