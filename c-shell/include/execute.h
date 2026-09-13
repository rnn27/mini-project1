#ifndef EXECUTE_H
#define EXECUTE_H

#include "parser.h"

int execute_command_list(const CommandList *command_list);
int install_sigchld_handler(void);
int execute_activities(void);

#endif
