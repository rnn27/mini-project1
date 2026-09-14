#ifndef SHELL_H
#define SHELL_H
/* Shared shell configuration and prompt interface. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define SHELL_MAX_INPUT 1024
void print_prompt();
#endif
