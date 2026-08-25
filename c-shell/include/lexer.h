#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_AMP,
    TOKEN_SEMI,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_GTGT
} TokenType;

typedef struct {
    TokenType type;
    char *value;
} Token;

typedef struct {
    Token *tokens;
    size_t count;
} TokenList;

typedef enum {
    LEX_SUCCESS,
    LEX_INVALID_SYNTAX
} LexResult;

LexResult lex_line(const char *input, TokenList *result);
void free_tokens(TokenList *tokens);

#endif
