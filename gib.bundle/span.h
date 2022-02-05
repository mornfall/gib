#pragma once
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

typedef struct span
{
    const char *str, *end;
} span_t;

/*
const char *span_prefix( span_t span, const char *prefix )
{
    const char *str = span.str;

    while ( *prefix && str < span.end && *str == *prefix )
        ++ str, ++ prefix;

    return *prefix ? 0 : str;
}
*/

bool span_empty( span_t span )
{
    return span.str == span.end;
}

bool span_eq( span_t span, const char *eq )
{
    const char *str = span.str;

    while ( *eq && str < span.end && *str == *eq )
        ++ str, ++ eq;

    return str == span.end && !*eq;
}

void span_copy( char *out, span_t span )
{
    while ( span.str < span.end )
        *out++ = *span.str++;
    *out = 0;
}

int span_len( span_t span )
{
    return span.end - span.str;
}

span_t span_mk( const char *str, const char *end )
{
    span_t s = { str, end };
    return s;
}

span_t span_lit( const char *str )
{
    return span_mk( str, str + strlen( str ) );
}

span_t span_dup( span_t s )
{
    char *out = malloc( span_len( s ) + 1 );
    span_copy( out, s );
    return span_mk( out, out + span_len( s ) );
}

void span_free( span_t s )
{
    free( ( void * ) s.str );
}
