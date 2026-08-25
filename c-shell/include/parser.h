#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

#include <stddef.h>

typedef enum{
    REDIR_INPUT,
    REDIR_OUTPUT,
    REDIR_APPEND
} RedirectionType;

typedef struct{
    RedirectionType type;
    char *filename;
} Redirection;

typedef struct{
    char **argv;
    size_t argc;
    size_t argv_capacity;

    Redirection *input_redirections;
    size_t input_redirection_count;
    size_t input_redirection_capacity;

    Redirection *output_redirections;
    size_t output_redirection_count;
    size_t output_redirection_capacity;
} Command;

typedef struct{
    Command *commands;
    size_t count;
    size_t capacity;

    int background;
} Pipeline;

typedef struct{
    Pipeline *pipelines;
    size_t count;
    size_t capacity;
} CommandList;

typedef enum{
    PARSE_SUCCESS,
    PARSE_INVALID_SYNTAX
} ParseResult;

ParseResult parse_tokens(const TokenList *tokens,
                         CommandList *command_list);

void free_command_list(CommandList *command_list);

#endif