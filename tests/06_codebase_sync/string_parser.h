#ifndef STRING_PARSER_H
#define STRING_PARSER_H

#include <stddef.h>

char *trim_whitespace(char *str);
size_t split_words(char *str, char **tokens, size_t max_tokens);

#endif

