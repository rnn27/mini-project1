#include "shell.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "intrinsics.h"

#include <errno.h>

/*
 * The directory where the shell was launched.
 * This is the shell's home directory.
 */
char shell_home[PATH_MAX];

void print_prompt(void){
    char cwd[PATH_MAX];
    char host[256];
    char *user = getenv("USER");

    if (getcwd(cwd, sizeof(cwd)) == NULL){
        perror("getcwd error");
        return;
    }

    if (gethostname(host, sizeof(host)) != 0){
        strcpy(host, "unknown");
    }

    size_t home_len = strlen(shell_home);

    /*
     * We are exactly at shell_home.
     */
    if (strcmp(cwd, shell_home) == 0){
        printf("<%s@%s:~> ",
               user ? user : "user",
               host);
    }

    /*
     * We are inside shell_home.
     * Replace the shell_home prefix with "~".
     */
    else if (strncmp(cwd, shell_home, home_len) == 0 &&
             cwd[home_len] == '/'){

        printf("<%s@%s:~%s> ",
               user ? user : "user",
               host,
               cwd + home_len);
    }

    /*
     * We are outside shell_home.
     * Show the absolute path unchanged.
     */
    else{
        printf("<%s@%s:%s> ",
               user ? user : "user",
               host,
               cwd);
    }

    fflush(stdout);
}

int main(void){
    /*
     * A1:
     * The directory from which the shell was launched becomes
     * its home directory.
     */
    if (getcwd(shell_home, sizeof(shell_home)) == NULL){
        perror("Fatal: could not get initial directory");
        return EXIT_FAILURE;
    }

    /*
     * A2:
     * Read one command line at a time.
     */
    char input[SHELL_MAX_INPUT];

    while (1){
        print_prompt();

        if (fgets(input, sizeof(input), stdin) == NULL){
            /*
             * EOF / Ctrl-D.
             */
            printf("\n");
            break;
        }

        /*
         * Remove the newline produced by fgets().
         */
        input[strcspn(input, "\n")] = '\0';

        /*
         * Empty input is valid.
         */
        if (input[0] == '\0'){
            continue;
        }

        /*
         * A3:
         * Lex the entire line before executing anything.
         */
        TokenList tokens;

        LexResult lex_result =
            lex_line(input, &tokens);

        if (lex_result == LEX_INVALID_SYNTAX){
            printf("cshell: invalid syntax\n");
            continue;
        }

        /*
         * Parse the complete token stream.
         */
        CommandList command_list;

        ParseResult parse_result =
            parse_tokens(&tokens,
                         &command_list);

        if (parse_result == PARSE_INVALID_SYNTAX){
            printf("cshell: invalid syntax\n");
            free_tokens(&tokens);
            continue;
        }

        /*
         * Part B:
         *
         * Intrinsics must execute in the shell process itself.
         *
         * This is essential for commands such as "hop", because
         * chdir() in a child process would not change the shell's
         * current working directory.
         *
         * For Part C, only the first command group is executed
         * when ';' or '&' separates command groups.
         */
        if (command_list.count > 0 &&
            command_list.pipelines[0].count == 1 &&
            !command_list.pipelines[0].background){

            Command *command =
                &command_list.pipelines[0].commands[0];

            if (command->argc > 0 &&
                is_intrinsic(command->argv[0])){

                /*
                 * Execute hop/reveal/peek/locate directly in
                 * the shell process.
                 */
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
         * Part C:
         *
         * Execute the first command group.
         *
         * execute_command_list() is responsible for the
         * external command / redirection / pipeline path.
         */
        (void)execute_command_list(&command_list);

        free_command_list(&command_list);
        free_tokens(&tokens);
    }

    return EXIT_SUCCESS;
}