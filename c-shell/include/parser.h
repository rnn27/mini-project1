#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

#include <stddef.h>

typedef enum {
    REDIR_NONE,
    REDIR_INPUT,
    REDIR_OUTPUT,
    REDIR_APPEND
} RedirectionType;

typedef struct {
    char **argv;
    size_t argc;
    size_t argv_capacity;

    RedirectionType input_redirection;
    char *input_file;

    RedirectionType output_redirection;
    char *output_file;
} Command;

typedef struct {
    Command *commands;
    size_t count;
    size_t capacity;

    int background;
} Pipeline;

typedef struct {
    Pipeline *pipelines;
    size_t count;
    size_t capacity;
} CommandList;

typedef enum {
    PARSE_SUCCESS,
    PARSE_INVALID_SYNTAX
} ParseResult;

ParseResult parse_tokens(const TokenList *tokens,
                         CommandList *command_list);

void free_command_list(CommandList *command_list);

#endif