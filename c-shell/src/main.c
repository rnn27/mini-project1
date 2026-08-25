#include "shell.h"
#include "lexer.h"

// Global to store the directory where the shell was launched
char shell_home[PATH_MAX];

void print_prompt() {
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
    fflush(stdout); // Force prompt to print immediately
}

int main() {
    // A1: The directory the shell is started in becomes its home directory
    if (getcwd(shell_home, sizeof(shell_home)) == NULL) {
        perror("Fatal: could not get initial directory");
        exit(EXIT_FAILURE);
    }

    // Changed from MAX_INPUT to SHELL_MAX_INPUT
    char input[SHELL_MAX_INPUT];

    while (1) {
        print_prompt();

        // A2: Consume input
        if (fgets(input, sizeof(input), stdin) == NULL) {
            // Handle EOF (Ctrl+D) gracefully
            printf("\n");
            break;
        }

        // Strip the trailing newline character
        input[strcspn(input, "\n")] = '\0';

        // Skip empty inputs
        if (strlen(input) == 0) {
            continue;
        }

        TokenList tokens;

LexResult result = lex_line(input, &tokens);

if (result == LEX_INVALID_SYNTAX) {
    printf("cshell: invalid syntax\n");
    continue;
}

for (size_t i = 0; i < tokens.count; i++) {
    printf("TOKEN %zu: type=%d value=[%s]\n",
           i,
           tokens.tokens[i].type,
           tokens.tokens[i].value);
}

free_tokens(&tokens);
    }

    return 0;
}