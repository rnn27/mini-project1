#include "parser.h"

#include <stdlib.h>
#include <string.h>

static void init_command(Command *command)
{
    command->argv = NULL;
    command->argc = 0;
    command->argv_capacity = 0;

    command->input_redirection = REDIR_NONE;
    command->input_file = NULL;

    command->output_redirection = REDIR_NONE;
    command->output_file = NULL;
}

static void free_command(Command *command)
{
    if (command == NULL) {
        return;
    }

    for (size_t i = 0; i < command->argc; i++) {
        free(command->argv[i]);
    }

    free(command->argv);
    free(command->input_file);
    free(command->output_file);

    init_command(command);
}

void free_command_list(CommandList *command_list)
{
    if (command_list == NULL) {
        return;
    }

    for (size_t i = 0; i < command_list->count; i++) {
        Pipeline *pipeline = &command_list->pipelines[i];

        for (size_t j = 0; j < pipeline->count; j++) {
            free_command(&pipeline->commands[j]);
        }

        free(pipeline->commands);
    }

    free(command_list->pipelines);

    command_list->pipelines = NULL;
    command_list->count = 0;
    command_list->capacity = 0;
}

static int add_pipeline(CommandList *command_list)
{
    if (command_list->count >= command_list->capacity) {
        size_t new_capacity =
            (command_list->capacity == 0)
                ? 2
                : command_list->capacity * 2;

        Pipeline *new_pipelines = realloc(
            command_list->pipelines,
            new_capacity * sizeof(Pipeline)
        );

        if (new_pipelines == NULL) {
            return 0;
        }

        command_list->pipelines = new_pipelines;
        command_list->capacity = new_capacity;
    }

    Pipeline *pipeline =
        &command_list->pipelines[command_list->count];

    pipeline->commands = NULL;
    pipeline->count = 0;
    pipeline->capacity = 0;
    pipeline->background = 0;

    command_list->count++;

    return 1;
}

static int add_command(Pipeline *pipeline)
{
    if (pipeline->count >= pipeline->capacity) {
        size_t new_capacity =
            (pipeline->capacity == 0)
                ? 2
                : pipeline->capacity * 2;

        Command *new_commands = realloc(
            pipeline->commands,
            new_capacity * sizeof(Command)
        );

        if (new_commands == NULL) {
            return 0;
        }

        pipeline->commands = new_commands;
        pipeline->capacity = new_capacity;
    }

    init_command(&pipeline->commands[pipeline->count]);
    pipeline->count++;

    return 1;
}

static int add_argument(Command *command, const char *value)
{
    if (command->argc + 1 >= command->argv_capacity) {
        size_t new_capacity =
            (command->argv_capacity == 0)
                ? 4
                : command->argv_capacity * 2;

        char **new_argv = realloc(
            command->argv,
            new_capacity * sizeof(char *)
        );

        if (new_argv == NULL) {
            return 0;
        }

        command->argv = new_argv;
        command->argv_capacity = new_capacity;
    }

    command->argv[command->argc] = strdup(value);

    if (command->argv[command->argc] == NULL) {
        return 0;
    }

    command->argc++;

    command->argv[command->argc] = NULL;

    return 1;
}

static int is_redirection(TokenType type)
{
    return type == TOKEN_LT ||
           type == TOKEN_GT ||
           type == TOKEN_GTGT;
}

static int set_redirection(Command *command,
                           TokenType operator,
                           const char *filename)
{
    if (operator == TOKEN_LT) {
        if (command->input_redirection != REDIR_NONE) {
            return 0;
        }

        command->input_redirection = REDIR_INPUT;
        command->input_file = strdup(filename);

        return command->input_file != NULL;
    }

    if (operator == TOKEN_GT) {
        if (command->output_redirection != REDIR_NONE) {
            return 0;
        }

        command->output_redirection = REDIR_OUTPUT;
        command->output_file = strdup(filename);

        return command->output_file != NULL;
    }

    if (operator == TOKEN_GTGT) {
        if (command->output_redirection != REDIR_NONE) {
            return 0;
        }

        command->output_redirection = REDIR_APPEND;
        command->output_file = strdup(filename);

        return command->output_file != NULL;
    }

    return 0;
}

static int parse_pipeline(const TokenList *tokens,
                          size_t *index,
                          Pipeline *pipeline)
{
    if (!add_command(pipeline)) {
        return 0;
    }

    Command *current =
        &pipeline->commands[pipeline->count - 1];

    while (*index < tokens->count) {
        TokenType type = tokens->tokens[*index].type;
        const char *value = tokens->tokens[*index].value;

        /*
         * WORD
         */
        if (type == TOKEN_WORD) {
            if (!add_argument(current, value)) {
                return 0;
            }

            (*index)++;
            continue;
        }

        /*
         * Redirection:
         *
         *     < WORD
         *     > WORD
         *     >> WORD
         */
        if (is_redirection(type)) {
            if (*index + 1 >= tokens->count ||
                tokens->tokens[*index + 1].type != TOKEN_WORD) {
                return 0;
            }

            if (!set_redirection(
                    current,
                    type,
                    tokens->tokens[*index + 1].value)) {
                return 0;
            }

            *index += 2;
            continue;
        }

        /*
         * Pipe.
         */
        if (type == TOKEN_PIPE) {
            if (current->argc == 0) {
                return 0;
            }

            (*index)++;

            if (*index >= tokens->count ||
                tokens->tokens[*index].type != TOKEN_WORD) {
                return 0;
            }

            if (!add_command(pipeline)) {
                return 0;
            }

            current =
                &pipeline->commands[pipeline->count - 1];

            continue;
        }

        /*
 * Background execution.
 *
 * '&' must be the final token of the complete input line.
 */
if (type == TOKEN_AMP) {
    if (current->argc == 0) {
        return 0;
    }

    pipeline->background = 1;
    (*index)++;

    /*
     * '&' must be the final token.
     */
    if (*index != tokens->count) {
        return 0;
    }

    return 1;
}

        /*
         * End of this pipeline.
         */
        if (type == TOKEN_SEMI) {
            if (current->argc == 0) {
                return 0;
            }

            return 1;
        }

        /*
         * Anything else is invalid.
         */
        return 0;
    }

    return current->argc != 0;
}

ParseResult parse_tokens(const TokenList *tokens,
                         CommandList *command_list)
{
    command_list->pipelines = NULL;
    command_list->count = 0;
    command_list->capacity = 0;

    if (tokens == NULL || tokens->count == 0) {
        return PARSE_INVALID_SYNTAX;
    }

    size_t index = 0;

    while (index < tokens->count) {
        /*
         * A command list cannot start with ';'.
         */
        if (tokens->tokens[index].type == TOKEN_SEMI) {
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        if (!add_pipeline(command_list)) {
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        Pipeline *pipeline =
            &command_list->pipelines[command_list->count - 1];

        if (!parse_pipeline(tokens, &index, pipeline)) {
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        /*
         * If we've consumed everything, we're done.
         */
        if (index >= tokens->count) {
            break;
        }

        /*
         * The only token allowed between pipelines is ';'.
         */
        if (tokens->tokens[index].type != TOKEN_SEMI) {
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        /*
         * Consume ';'.
         */
        index++;

        /*
         * A semicolon must be followed by another command.
         */
        if (index >= tokens->count ||
            tokens->tokens[index].type == TOKEN_SEMI) {
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }
    }

    if (command_list->count == 0) {
        free_command_list(command_list);
        return PARSE_INVALID_SYNTAX;
    }

    return PARSE_SUCCESS;
}