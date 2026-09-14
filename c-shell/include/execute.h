#ifndef EXECUTE_H
#define EXECUTE_H

#include "parser.h"

int execute_command_list(const CommandList *command_list);
int install_sigchld_handler(void);
int initialize_job_control(void);
int execute_activities(void);
int execute_resume(int argc, char *const argv[]);
int execute_ping(int argc, char *const argv[]);
int execute_spy(int argc, char *const argv[]);
int has_stopped_jobs(void);
void send_sighup_to_jobs(void);

#endif
