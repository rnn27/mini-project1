#include "parser.h"

#include <stdlib.h>
#include <string.h>

static void init_command(Command *command){
    command->argv=NULL;
    command->argc=0;
    command->argv_capacity=0;

    command->input_redirections=NULL;
    command->input_redirection_count=0;
    command->input_redirection_capacity=0;

    command->output_redirections=NULL;
    command->output_redirection_count=0;
    command->output_redirection_capacity=0;
}

static void free_redirections(Redirection *redirections,
                              size_t count){
    for(size_t i=0; i < count; i++){
        free(redirections[i].filename);
    }

    free(redirections);
}

static void free_command(Command *command){
    if(command==NULL){
        return;
    }

    for(size_t i=0; i < command->argc; i++){
        free(command->argv[i]);
    }

    free(command->argv);

    free_redirections(command->input_redirections,
                      command->input_redirection_count);

    free_redirections(command->output_redirections,
                      command->output_redirection_count);

    init_command(command);
}

void free_command_list(CommandList *command_list){
    if(command_list==NULL){
        return;
    }

    for(size_t i=0; i < command_list->count; i++){
        Pipeline *pipeline=&command_list->pipelines[i];

        for(size_t j=0; j < pipeline->count; j++){
            free_command(&pipeline->commands[j]);
        }

        free(pipeline->commands);
    }

    free(command_list->pipelines);

    command_list->pipelines=NULL;
    command_list->count=0;
    command_list->capacity=0;
}

static int add_pipeline(CommandList *command_list){
    if(command_list->count >=command_list->capacity){
        size_t new_capacity=
           (command_list->capacity==0)
                ? 2
                : command_list->capacity * 2;

        Pipeline *new_pipelines=realloc(
            command_list->pipelines,
            new_capacity * sizeof(Pipeline)
        );

        if(new_pipelines==NULL){
            return 0;
        }

        command_list->pipelines=new_pipelines;
        command_list->capacity=new_capacity;
    }

    Pipeline *pipeline=
        &command_list->pipelines[command_list->count];

    pipeline->commands=NULL;
    pipeline->count=0;
    pipeline->capacity=0;
    pipeline->background=0;

    command_list->count++;

    return 1;
}

static int add_command(Pipeline *pipeline){
    if(pipeline->count >=pipeline->capacity){
        size_t new_capacity=
           (pipeline->capacity==0)
                ? 2
                : pipeline->capacity * 2;

        Command *new_commands=realloc(
            pipeline->commands,
            new_capacity * sizeof(Command)
        );

        if(new_commands==NULL){
            return 0;
        }

        pipeline->commands=new_commands;
        pipeline->capacity=new_capacity;
    }

    init_command(&pipeline->commands[pipeline->count]);
    pipeline->count++;

    return 1;
}

static int add_argument(Command *command,
                        const char *value){
    if(command->argc + 1 >=command->argv_capacity){
        size_t new_capacity=
           (command->argv_capacity==0)
                ? 4
                : command->argv_capacity * 2;

        char **new_argv=realloc(
            command->argv,
            new_capacity * sizeof(char *)
        );

        if(new_argv==NULL){
            return 0;
        }

        command->argv=new_argv;
        command->argv_capacity=new_capacity;
    }

    command->argv[command->argc]=strdup(value);

    if(command->argv[command->argc]==NULL){
        return 0;
    }

    command->argc++;
    command->argv[command->argc]=NULL;

    return 1;
}

static int add_redirection(Redirection **redirections,
                           size_t *count,
                           size_t *capacity,
                           RedirectionType type,
                           const char *filename){
    if(*count >=*capacity){
        size_t new_capacity=
           (*capacity==0)
                ? 2
                : *capacity * 2;

        Redirection *new_redirections=realloc(
            *redirections,
            new_capacity * sizeof(Redirection)
        );

        if(new_redirections==NULL){
            return 0;
        }

        *redirections=new_redirections;
        *capacity=new_capacity;
    }

   (*redirections)[*count].type=type;
   (*redirections)[*count].filename=strdup(filename);

    if((*redirections)[*count].filename==NULL){
        return 0;
    }

   (*count)++;

    return 1;
}

static int add_input_redirection(Command *command,
                                  const char *filename){
    return add_redirection(
        &command->input_redirections,
        &command->input_redirection_count,
        &command->input_redirection_capacity,
        REDIR_INPUT,
        filename
    );
}

static int add_output_redirection(Command *command,
                                  RedirectionType type,
                                  const char *filename){
    return add_redirection(
        &command->output_redirections,
        &command->output_redirection_count,
        &command->output_redirection_capacity,
        type,
        filename
    );
}

static int is_redirection(TokenType type){
    return type==TOKEN_LT ||
           type==TOKEN_GT ||
           type==TOKEN_GTGT;
}

static int parse_redirection(const TokenList *tokens,
                             size_t *index,
                             Command *command){
    TokenType operator=tokens->tokens[*index].type;

    if(*index + 1 >=tokens->count ||
        tokens->tokens[*index + 1].type !=TOKEN_WORD){
        return 0;
    }

    const char *filename=
        tokens->tokens[*index + 1].value;

    int success;

    if(operator==TOKEN_LT){
        success=add_input_redirection(
            command,
            filename
        );
    } else if(operator==TOKEN_GT){
        success=add_output_redirection(
            command,
            REDIR_OUTPUT,
            filename
        );
    } else{
        success=add_output_redirection(
            command,
            REDIR_APPEND,
            filename
        );
    }

    if(!success){
        return 0;
    }

    *index +=2;

    return 1;
}

static int parse_pipeline(const TokenList *tokens,
                          size_t *index,
                          Pipeline *pipeline){
    if(!add_command(pipeline)){
        return 0;
    }

    Command *current=
        &pipeline->commands[pipeline->count - 1];

    while(*index < tokens->count){
        TokenType type=
            tokens->tokens[*index].type;

        if(type==TOKEN_WORD){
            if(!add_argument(
                    current,
                    tokens->tokens[*index].value)){
                return 0;
            }

           (*index)++;
            continue;
        }

        if(is_redirection(type)){
            if(!parse_redirection(
                    tokens,
                    index,
                    current)){
                return 0;
            }

            continue;
        }

        if(type==TOKEN_PIPE){
            if(current->argc==0){
                return 0;
            }

           (*index)++;

            if(*index >=tokens->count ||
                tokens->tokens[*index].type !=TOKEN_WORD){
                return 0;
            }

            if(!add_command(pipeline)){
                return 0;
            }

            current=
                &pipeline->commands[pipeline->count - 1];

            continue;
        }

        if(type==TOKEN_AMP){
            if(current->argc==0){
                return 0;
            }

            pipeline->background=1;
           (*index)++;

            if(*index !=tokens->count){
                return 0;
            }

            return 1;
        }

        if(type==TOKEN_SEMI){
            if(current->argc==0){
                return 0;
            }

            return 1;
        }

        return 0;
    }

    return current->argc !=0;
}

ParseResult parse_tokens(const TokenList *tokens,
                         CommandList *command_list){
    command_list->pipelines=NULL;
    command_list->count=0;
    command_list->capacity=0;

    if(tokens==NULL || tokens->count==0){
        return PARSE_INVALID_SYNTAX;
    }

    size_t index=0;

    while(index < tokens->count){
        if(tokens->tokens[index].type==TOKEN_SEMI){
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        if(!add_pipeline(command_list)){
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        Pipeline *pipeline=
            &command_list->pipelines[
                command_list->count - 1
            ];

        if(!parse_pipeline(
                tokens,
                &index,
                pipeline)){
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        if(index >=tokens->count){
            break;
        }

        if(tokens->tokens[index].type !=TOKEN_SEMI){
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }

        index++;

        if(index >=tokens->count ||
            tokens->tokens[index].type==TOKEN_SEMI){
            free_command_list(command_list);
            return PARSE_INVALID_SYNTAX;
        }
    }

    if(command_list->count==0){
        free_command_list(command_list);
        return PARSE_INVALID_SYNTAX;
    }

    return PARSE_SUCCESS;
}