#ifndef INTRINSICS_H
#define INTRINSICS_H
#include <stddef.h>
int is_intrinsic(const char *command);
int execute_intrinsic(int argc, char *const argv[]);
#endif
