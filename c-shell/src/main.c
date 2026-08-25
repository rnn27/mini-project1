#include "shell.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "intrinsics.h"

#include <errno.h>

/*
 * The directory where the shell was launched.
 */
char shell_home[PATH_MAX];

void print_prompt(void)
{
    char cwd[PATH_MAX];
    char host[256];
    char *user = getenv("USER");

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd error");
        return;
    }

    if (gethostname(host, sizeof(host)) != 0) {
        strcpy(host, "unknown");
    }

    size_t home_len = strlen(shell_home);

    if (strcmp(cwd, shell_home) == 0) {
        printf("<%s@%s:~> ",
               user ? user : "user",
               host);
    } else if (strncmp(cwd, shell_home, home_len) == 0 &&
               cwd[home_len] == '/') {
        printf("<%s@%s:~%s> ",
               user ? user : "user",
               host,
               cwd + home_len);
    } else {
        printf("<%s@%s:%s> ",
               user ? user : "user",
               host,
               cwd);
    }

    fflush(stdout);
}

int main(void)
{
    /*
     * A1:
     * The directory where the shell starts becomes its home.
     */
    if (getcwd(shell_home, sizeof(shell_home)) == NULL) {
        perror("Fatal: could not get initial directory");
        return EXIT_FAILURE;
    }

    /*
     * A2:
     * Read one line at a time.
     */
    char input[SHELL_MAX_INPUT];

    while (1) {
        print_prompt();

        if (fgets(input, sizeof(input), stdin) == NULL) {
            /*
             * Ctrl-D / EOF.
             */
            printf("\n");
            break;
        }

        /*
         * Remove trailing newline.
         */
        input[strcspn(input, "\n")] = '\0';

        /*
         * Ignore empty lines.
         */
        if (input[0] == '\0') {
            continue;
        }

        /*
         * A3:
         * Lex the complete command line.
         */
        TokenList tokens;

        LexResult lex_result =
            lex_line(input, &tokens);

        if (lex_result == LEX_INVALID_SYNTAX) {
            printf("cshell: invalid syntax\n");
            continue;
        }

        /*
         * Parse the complete command line.
         */
        CommandList command_list;

        ParseResult parse_result =
            parse_tokens(&tokens,
                         &command_list);

        if (parse_result == PARSE_INVALID_SYNTAX) {
            printf("cshell: invalid syntax\n");
            free_tokens(&tokens);
            continue;
        }

        /*
         * B1:
         *
         * Intrinsics must execute inside the shell process.
         *
         * In particular, hop must call chdir() in this process;
         * executing it in a forked child would not change the
         * shell's working directory.
         *
         * For this first intrinsic implementation, only a
         * standalone command is handled here.
         */
        if (command_list.count == 1 &&
            command_list.pipelines[0].count == 1) {

            Command *command =
                &command_list.pipelines[0].commands[0];

            if (command->argc > 0 &&
                is_intrinsic(command->argv[0])) {

                (void)execute_intrinsic(
                    (int)command->argc,
                    command->argv
                );

                free_command_list(&command_list);
                free_tokens(&tokens);
                continue;
            }
        }

        /*
         * Normal external command execution.
         */
        (void)execute_command_list(&command_list);

        free_command_list(&command_list);
        free_tokens(&tokens);
    }

    return EXIT_SUCCESS;
}