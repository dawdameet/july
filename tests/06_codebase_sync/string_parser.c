#include "string_parser.h"
#include <ctype.h>
#include <string.h>

char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

size_t split_words(char *str, char **tokens, size_t max_tokens) {
    size_t count = 0;
    char *p = str;
    while (*p && count < max_tokens) {
        while (*p && isspace((unsigned char)*p)) *p++ = '\0';
        if (*p) {
            tokens[count++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
    }
    return count;
}

