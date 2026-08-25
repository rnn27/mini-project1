#include "executor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int copy_fd(int source_fd, int destination_fd)
{
    char buffer[8192];

    while (1) {
        ssize_t bytes_read =
            read(source_fd, buffer, sizeof(buffer));

        if (bytes_read == 0) {
            return 0;
        }

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        ssize_t total_written = 0;

        while (total_written < bytes_read) {
            ssize_t bytes_written =
                write(destination_fd,
                       buffer + total_written,
                       (size_t)(bytes_read - total_written));

            if (bytes_written < 0) {
                if (errno == EINTR) {
                    continue;
                }

                return -1;
            }

            total_written += bytes_written;
        }
    }
}

static int prepare_input_stream(const Command *command)
{
    if (command->input_redirection_count == 0) {
        return -1;
    }

    char template[] = "/tmp/cshell-input-XXXXXX";

    int temp_fd = mkstemp(template);

    if (temp_fd < 0) {
        return -1;
    }

    if (unlink(template) < 0) {
        close(temp_fd);
        return -1;
    }

    for (size_t i = 0;
         i < command->input_redirection_count;
         i++) {

        const Redirection *redirection =
            &command->input_redirections[i];

        int input_fd =
            open(redirection->filename, O_RDONLY);

        if (input_fd < 0) {
            fprintf(stderr,
                    "cshell: no such file or directory\n");
            close(temp_fd);
            return -1;
        }

        if (copy_fd(input_fd, temp_fd) < 0) {
            close(input_fd);
            close(temp_fd);
            return -1;
        }

        close(input_fd);
    }

    if (lseek(temp_fd, 0, SEEK_SET) < 0) {
        close(temp_fd);
        return -1;
    }

    return temp_fd;
}

static int open_output_files(const Command *command,
                             int **fds_out)
{
    size_t count =
        command->output_redirection_count;

    int *fds = malloc(count * sizeof(int));

    if (fds == NULL) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        const Redirection *redirection =
            &command->output_redirections[i];

        int flags = O_WRONLY | O_CREAT;

        if (redirection->type == REDIR_OUTPUT) {
            flags |= O_TRUNC;
        } else {
            flags |= O_APPEND;
        }

        fds[i] =
            open(redirection->filename, flags, 0644);

        if (fds[i] < 0) {
            for (size_t j = 0; j < i; j++) {
                close(fds[j]);
            }

            free(fds);

            fprintf(stderr,
                    "cshell: unable to create file for writing\n");

            return -1;
        }
    }

    *fds_out = fds;
    return 0;
}

static int relay_output(int read_fd,
                        const Command *command,
                        int *output_fds)
{
    char buffer[8192];

    while (1) {
        ssize_t bytes_read =
            read(read_fd, buffer, sizeof(buffer));

        if (bytes_read == 0) {
            return 0;
        }

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        for (size_t i = 0;
             i < command->output_redirection_count;
             i++) {

            ssize_t total_written = 0;

            while (total_written < bytes_read) {
                ssize_t bytes_written =
                    write(output_fds[i],
                          buffer + total_written,
                          (size_t)(bytes_read - total_written));

                if (bytes_written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }

                    return -1;
                }

                total_written += bytes_written;
            }
        }
    }
}

static int setup_input_for_child(const Command *command)
{
    if (command->input_redirection_count == 0) {
        return 0;
    }

    int input_fd = prepare_input_stream(command);

    if (input_fd < 0) {
        return -1;
    }

    if (dup2(input_fd, STDIN_FILENO) < 0) {
        close(input_fd);
        return -1;
    }

    close(input_fd);
    return 0;
}

static int execute_simple_command(const Command *command)
{
    int input_fd = -1;

    if (command->input_redirection_count > 0) {
        input_fd = prepare_input_stream(command);

        if (input_fd < 0) {
            return -1;
        }
    }

    int *output_fds = NULL;

    if (command->output_redirection_count > 0) {
        if (open_output_files(command, &output_fds) < 0) {
            if (input_fd >= 0) {
                close(input_fd);
            }

            return -1;
        }
    }

    int output_pipe[2] = {-1, -1};

    if (command->output_redirection_count > 0) {
        if (pipe(output_pipe) < 0) {
            perror("cshell: pipe");

            for (size_t i = 0;
                 i < command->output_redirection_count;
                 i++) {
                close(output_fds[i]);
            }

            free(output_fds);

            if (input_fd >= 0) {
                close(input_fd);
            }

            return -1;
        }
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("cshell: fork");

        if (output_pipe[0] >= 0) {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }

        for (size_t i = 0;
             i < command->output_redirection_count;
             i++) {
            close(output_fds[i]);
        }

        free(output_fds);

        if (input_fd >= 0) {
            close(input_fd);
        }

        return -1;
    }

    if (pid == 0) {
        if (input_fd >= 0) {
            if (dup2(input_fd, STDIN_FILENO) < 0) {
                _exit(1);
            }

            close(input_fd);
        }

        if (command->output_redirection_count > 0) {
            close(output_pipe[0]);

            if (dup2(output_pipe[1],
                     STDOUT_FILENO) < 0) {
                _exit(1);
            }

            close(output_pipe[1]);
        }

        execvp(command->argv[0], command->argv);

        fprintf(stderr,
                "cshell: %s: %s\n",
                command->argv[0],
                strerror(errno));

        _exit(127);
    }

    if (input_fd >= 0) {
        close(input_fd);
    }

    if (command->output_redirection_count > 0) {
        close(output_pipe[1]);

        if (relay_output(output_pipe[0],
                         command,
                         output_fds) < 0) {
            close(output_pipe[0]);

            for (size_t i = 0;
                 i < command->output_redirection_count;
                 i++) {
                close(output_fds[i]);
            }

            free(output_fds);

            (void)waitpid(pid, NULL, 0);

            return -1;
        }

        close(output_pipe[0]);

        for (size_t i = 0;
             i < command->output_redirection_count;
             i++) {
            close(output_fds[i]);
        }

        free(output_fds);
    }

    int status;

    if (waitpid(pid, &status, 0) < 0) {
        perror("cshell: waitpid");
        return -1;
    }

    return 0;
}

static int execute_pipeline(const Pipeline *pipeline)
{
    size_t command_count = pipeline->count;

    if (command_count == 0) {
        return -1;
    }

    if (command_count == 1) {
        return execute_simple_command(
            &pipeline->commands[0]
        );
    }

    size_t pipe_count = command_count - 1;

    int (*pipes)[2] =
        malloc(pipe_count * sizeof(*pipes));

    pid_t *pids =
        malloc(command_count * sizeof(pid_t));

    if (pipes == NULL || pids == NULL) {
        free(pipes);
        free(pids);

        perror("cshell: malloc");
        return -1;
    }

    /*
     * Create all pipes before forking.
     */
    for (size_t i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("cshell: pipe");

            for (size_t j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            free(pipes);
            free(pids);

            return -1;
        }
    }

    for (size_t i = 0; i < command_count; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("cshell: fork");

            for (size_t j = 0; j < pipe_count; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            for (size_t j = 0; j < i; j++) {
                (void)waitpid(pids[j], NULL, 0);
            }

            free(pipes);
            free(pids);

            return -1;
        }

        pids[i] = pid;

        if (pid == 0) {
            const Command *command =
                &pipeline->commands[i];

            /*
             * Input side of the pipeline.
             */
            if (i > 0) {
                if (dup2(pipes[i - 1][0],
                         STDIN_FILENO) < 0) {
                    _exit(1);
                }
            }

            /*
             * Output side of the pipeline.
             */
            if (i < pipe_count) {
                if (dup2(pipes[i][1],
                         STDOUT_FILENO) < 0) {
                    _exit(1);
                }
            }

            /*
             * Explicit input redirection overrides the
             * pipeline input for this command.
             */
            if (command->input_redirection_count > 0) {
                if (setup_input_for_child(command) < 0) {
                    _exit(1);
                }
            }

            /*
             * Output redirection overrides pipeline stdout
             * for the final command.
             *
             * Multiple-output fan-out in a pipeline will be
             * handled in the next refinement.
             */
            if (i == command_count - 1 &&
                command->output_redirection_count > 0) {

                if (command->output_redirection_count == 1) {
                    const Redirection *redirection =
                        &command->output_redirections[0];

                    int flags =
                        O_WRONLY | O_CREAT;

                    if (redirection->type == REDIR_OUTPUT) {
                        flags |= O_TRUNC;
                    } else {
                        flags |= O_APPEND;
                    }

                    int fd =
                        open(redirection->filename,
                             flags,
                             0644);

                    if (fd < 0) {
                        fprintf(stderr,
                                "cshell: unable to create file for writing\n");
                        _exit(1);
                    }

                    if (dup2(fd, STDOUT_FILENO) < 0) {
                        close(fd);
                        _exit(1);
                    }

                    close(fd);
                }
            }

            /*
             * Close every inherited pipe descriptor.
             */
            for (size_t j = 0; j < pipe_count; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            execvp(command->argv[0], command->argv);

            fprintf(stderr,
                    "cshell: %s: %s\n",
                    command->argv[0],
                    strerror(errno));

            _exit(127);
        }
    }

    /*
     * Parent closes every pipe descriptor.
     */
    for (size_t i = 0; i < pipe_count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /*
     * Wait for every pipeline stage.
     */
    int result = 0;

    for (size_t i = 0; i < command_count; i++) {
        int status;

        if (waitpid(pids[i], &status, 0) < 0) {
            perror("cshell: waitpid");
            result = -1;
        }
    }

    free(pipes);
    free(pids);

    return result;
}

int execute_command_list(const CommandList *command_list)
{
    if (command_list == NULL) {
        return -1;
    }

    for (size_t i = 0;
         i < command_list->count;
         i++) {

        const Pipeline *pipeline =
            &command_list->pipelines[i];

        /*
         * Background execution is implemented later.
         */
        if (pipeline->background) {
            fprintf(stderr,
                    "cshell: background execution not implemented yet\n");
            return -1;
        }

        if (execute_pipeline(pipeline) < 0) {
            return -1;
        }
    }

    return 0;
}