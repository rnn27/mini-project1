#include "executor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int execute_command_list(const CommandList *command_list)
{
    if (command_list == NULL) {
        return -1;
    }

    for (size_t i = 0; i < command_list->count; i++) {
        const Pipeline *pipeline =
            &command_list->pipelines[i];

        /*
         * C1 supports only a single command.
         * Pipelines will be implemented next.
         */
        if (pipeline->count != 1) {
            fprintf(stderr,
                    "cshell: pipelines not implemented yet\n");
            return -1;
        }

        const Command *command =
            &pipeline->commands[0];

        if (command->argc == 0) {
            continue;
        }

        /*
         * Redirection will be implemented in C2.
         */
        if (command->input_redirection != REDIR_NONE ||
            command->output_redirection != REDIR_NONE) {
            fprintf(stderr,
                    "cshell: redirection not implemented yet\n");
            return -1;
        }

        /*
         * Background execution will be implemented later.
         */
        if (pipeline->background) {
            fprintf(stderr,
                    "cshell: background execution not implemented yet\n");
            return -1;
        }

        pid_t pid = fork();

        if (pid < 0) {
            perror("cshell: fork");
            return -1;
        }

        if (pid == 0) {
            /*
             * Child process.
             */
            execvp(command->argv[0], command->argv);

            /*
             * execvp() returns only if execution failed.
             */
            fprintf(stderr,
                    "cshell: %s: %s\n",
                    command->argv[0],
                    strerror(errno));

            _exit(127);
        }

        /*
         * Parent waits for the foreground child.
         */
        int status;

        if (waitpid(pid, &status, 0) < 0) {
            perror("cshell: waitpid");
            return -1;
        }
    }

    return 0;
}
