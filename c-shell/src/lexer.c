#include "lexer.h"

#include <stdlib.h>
#include <string.h>

static int is_space_char(char c){
    return c==' ' || c=='\t' || c=='\n' || c=='\r';
}

static int is_special_char(char c){
    return c=='|' || c=='&' || c==';' || c=='<' || c=='>';
}

static int add_token(TokenList *list, TokenType type,
                     const char *value, size_t length){
    Token *new_tokens=realloc(
        list->tokens,
       (list->count + 1) * sizeof(Token)
    );

    if(new_tokens==NULL){
        return 0;
    }

    list->tokens=new_tokens;

    char *token_value=malloc(length + 1);
    if(token_value==NULL){
        return 0;
    }

    memcpy(token_value, value, length);
    token_value[length]='\0';

    list->tokens[list->count].type=type;
    list->tokens[list->count].value=token_value;
    list->count++;

    return 1;
}

static int add_char(char **buffer, size_t *length,
                    size_t *capacity, char c){
    if(*length + 1 >=*capacity){
        size_t new_capacity=
           (*capacity==0) ? 16 : *capacity * 2;

        char *new_buffer=realloc(*buffer, new_capacity);

        if(new_buffer==NULL){
            return 0;
        }

        *buffer=new_buffer;
        *capacity=new_capacity;
    }

   (*buffer)[*length]=c;
   (*length)++;

    return 1;
}

void free_tokens(TokenList *list){
    if(list==NULL){
        return;
    }

    for(size_t i=0; i < list->count; i++){
        free(list->tokens[i].value);
    }

    free(list->tokens);

    list->tokens=NULL;
    list->count=0;
}

LexResult lex_line(const char *input, TokenList *result){
    result->tokens=NULL;
    result->count=0;

    size_t i=0;

    while(input[i] !='\0'){

        /*
         * Whitespace separates tokens.
         */
        if(is_space_char(input[i])){
            i++;
            continue;
        }

        /*
         * Single-character operators.
         */
        if(input[i]=='|'){
            if(!add_token(result, TOKEN_PIPE, "|", 1)){
                free_tokens(result);
                return LEX_INVALID_SYNTAX;
            }

            i++;
            continue;
        }

        if(input[i]=='&'){
            if(!add_token(result, TOKEN_AMP, "&", 1)){
                free_tokens(result);
                return LEX_INVALID_SYNTAX;
            }

            i++;
            continue;
        }

        if(input[i]==';'){
            if(!add_token(result, TOKEN_SEMI, ";", 1)){
                free_tokens(result);
                return LEX_INVALID_SYNTAX;
            }

            i++;
            continue;
        }

        if(input[i]=='<'){
            if(!add_token(result, TOKEN_LT, "<", 1)){
                free_tokens(result);
                return LEX_INVALID_SYNTAX;
            }

            i++;
            continue;
        }

        /*
         * Output redirection.
         *
         * Maximal munch:
         *     >>  -> TOKEN_GTGT
         *     >   -> TOKEN_GT
         */
        if(input[i]=='>'){
            if(input[i + 1]=='>'){
                if(!add_token(result, TOKEN_GTGT, ">>", 2)){
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                i +=2;
            } else{
                if(!add_token(result, TOKEN_GT, ">", 1)){
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                i++;
            }

            continue;
        }

        /*
         * Anything that is not whitespace or an operator
         * starts a WORD.
         *
         * A WORD may contain:
         *   - ordinary characters
         *   - escaped characters
         *   - double-quoted fragments
         *   - single-quoted fragments
         *
         * word_started is separate from length because:
         *
         *     ""
         *
         * is a valid WORD with an empty value.
         */
        char *word=NULL;
        size_t length=0;
        size_t capacity=0;
        int word_started=0;

        while(input[i] !='\0' &&
               !is_space_char(input[i]) &&
               !is_special_char(input[i])){

            /*
             * Unquoted escape:
             *
             *     \c  -> c
             *
             * A backslash at the end of the input is invalid.
             */
            if(input[i]=='\\'){
                word_started=1;
                i++;

                if(input[i]=='\0'){
                    free(word);
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                if(!add_char(&word, &length,
                              &capacity, input[i])){
                    free(word);
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                i++;
                continue;
            }

            /*
             * Double-quoted fragment.
             *
             * Inside double quotes:
             *
             *     \" -> "
             *     \\ -> \
             *     \c -> \c  for other characters
             */
            if(input[i]=='"'){
                word_started=1;
                i++;

                while(input[i] !='\0' && input[i] !='"'){

                    if(input[i]=='\\'){
                        i++;

                        if(input[i]=='\0'){
                            free(word);
                            free_tokens(result);
                            return LEX_INVALID_SYNTAX;
                        }

                        if(input[i]=='"' ||
                            input[i]=='\\'){

                            if(!add_char(&word, &length,
                                          &capacity, input[i])){
                                free(word);
                                free_tokens(result);
                                return LEX_INVALID_SYNTAX;
                            }

                        } else{

                            /*
                             * For \c where c is neither '"' nor '\',
                             * preserve both characters.
                             */
                            if(!add_char(&word, &length,
                                          &capacity, '\\')){
                                free(word);
                                free_tokens(result);
                                return LEX_INVALID_SYNTAX;
                            }

                            if(!add_char(&word, &length,
                                          &capacity, input[i])){
                                free(word);
                                free_tokens(result);
                                return LEX_INVALID_SYNTAX;
                            }
                        }

                        i++;
                    } else{

                        if(!add_char(&word, &length,
                                      &capacity, input[i])){
                            free(word);
                            free_tokens(result);
                            return LEX_INVALID_SYNTAX;
                        }

                        i++;
                    }
                }

                /*
                 * We reached '\0' without finding the closing quote.
                 */
                if(input[i] !='"'){
                    free(word);
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                i++;
                continue;
            }

            /*
             * Single-quoted fragment.
             *
             * Everything inside single quotes is literal.
             *
             * Therefore:
             *
             *     'a\ b'
             *
             * becomes:
             *
             *     a\ b
             */
            if(input[i]=='\''){
                word_started=1;
                i++;

                while(input[i] !='\0' && input[i] !='\''){

                    if(!add_char(&word, &length,
                                  &capacity, input[i])){
                        free(word);
                        free_tokens(result);
                        return LEX_INVALID_SYNTAX;
                    }

                    i++;
                }

                /*
                 * We reached '\0' without finding
                 * the closing single quote.
                 */
                if(input[i] !='\''){
                    free(word);
                    free_tokens(result);
                    return LEX_INVALID_SYNTAX;
                }

                i++;
                continue;
            }

            /*
             * Ordinary character.
             */
            word_started=1;

            if(!add_char(&word, &length,
                          &capacity, input[i])){
                free(word);
                free_tokens(result);
                return LEX_INVALID_SYNTAX;
            }

            i++;
        }

        /*
         * No fragment was found.
         *
         * This should normally be unreachable because operators
         * and whitespace are handled before entering the WORD logic,
         * but it keeps the lexer defensive.
         */
        if(!word_started){
            free(word);
            free_tokens(result);
            return LEX_INVALID_SYNTAX;
        }

        if(!add_token(result, TOKEN_WORD, word, length)){
            free(word);
            free_tokens(result);
            return LEX_INVALID_SYNTAX;
        }

        free(word);
    }

    return LEX_SUCCESS;
}