#include "shell.h"
#include "lexer.h"
#include "parser.h"

// Global to store the directory where the shell was launched.
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
     * The directory from which the shell is launched becomes
     * the shell's home directory.
     */
    if (getcwd(shell_home, sizeof(shell_home)) == NULL) {
        perror("Fatal: could not get initial directory");
        return EXIT_FAILURE;
    }

    /*
     * A2:
     * Read one command line at a time.
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
         * Remove the trailing newline inserted by fgets().
         */
        input[strcspn(input, "\n")] = '\0';

        /*
         * Ignore empty input lines.
         */
        if (input[0] == '\0') {
            continue;
        }

        /*
         * A3:
         * Lex the complete input line before any execution.
         */
        TokenList tokens;

        LexResult result = lex_line(input, &tokens);

        if (result == LEX_INVALID_SYNTAX) {
            printf("cshell: invalid syntax\n");
            continue;
        }

        CommandList command_list;

ParseResult parse_result =
    parse_tokens(&tokens, &command_list);

if (parse_result == PARSE_INVALID_SYNTAX) {
    printf("cshell: invalid syntax\n");
    free_tokens(&tokens);
    continue;
}

/*
 * Temporary parser validation.
 * Execution will be implemented in Part C.
 */
free_command_list(&command_list);
free_tokens(&tokens);
    }

    return EXIT_SUCCESS;
}