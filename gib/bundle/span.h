#pragma once
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

typedef struct span
{
    const char *str, *end;
} span_t;

span_t span_mk( const char *str, const char *end ) { span_t s = { str, end }; return s; }
int    span_len( span_t span )     { return span.end - span.str; }
bool   span_empty( span_t span )   { return span.str == span.end; }
span_t span_lit( const char *str ) { return span_mk( str, str + strlen( str ) ); }
span_t span_tail( span_t span )    { return span_mk( span.str + 1, span.end ); }

const char *span_starts_with( span_t span, const char *prefix )
{
    const char *str = span.str;

    while ( *prefix && str < span.end && *str == *prefix )
        ++ str, ++ prefix;

    return *prefix ? 0 : str;
}

bool span_eq( span_t span, const char *eq )
{
    const char *str = span.str;

    while ( *eq && str < span.end && *str == *eq )
        ++ str, ++ eq;

    return str == span.end && !*eq;
}

char *span_copy( char *out, span_t span )
{
    while ( span.str < span.end )
        *out++ = *span.str++;
    *out = 0;
    return out;
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

bool span_match_rec( span_t pat, span_t str, span_t capture[ 9 ], int c_idx, int c_len, bool c_end )
{
    if ( c_end )
        capture[ c_idx - 1 ].end = capture[ c_idx - 1 ].str + c_len;

    if ( span_empty( pat ) && span_empty( str ) )
        return true;
    if ( span_empty( pat ) || str.str > str.end )
        return false;

    if ( !c_len )
        capture[ c_idx ].str = capture[ c_idx ].end = str.str;

    if ( pat.str[ 0 ] == '%' ) /* non-greedy wildcard */
        return span_match_rec( span_tail( pat ), str, capture, c_idx + 1, c_len, true ) ||
               span_match_rec( pat, span_tail( str ), capture, c_idx, c_len + 1, false );

    if ( pat.str[ 0 ] == '*' )
        return span_match_rec( pat, span_tail( str ), capture, c_idx, c_len + 1, false ) ||
               span_match_rec( span_tail( pat ), str, capture, c_idx + 1 , c_len, true );

    if ( pat.str[ 0 ] == '\\' ) /* \x matches x, just like posix extended regexes */
        pat.str ++;

    if ( span_empty( pat ) || span_empty( str ) || pat.str[ 0 ] != str.str[ 0 ] )
        return false;

    pat.str ++;
    str.str ++;

    return span_match_rec( pat, str, capture, c_idx, 0, false );
}

bool span_match( span_t pat, span_t str, span_t capture[ 9 ] )
{
    return span_match_rec( pat, str, capture, 0, 0, false );
}

typedef struct buffer
{
    char *data, *end;
    size_t size;
} buffer_t;

span_t buffer_span( buffer_t buf ) { return span_mk( buf.data, buf.end ); }
int    buffer_len( buffer_t buf )  { return buf.end - buf.data; }

buffer_t buffer_alloc( int size )
{
    buffer_t buf;
    buf.data = malloc( size );
    buf.size = buf.data ? size : 0;
    buf.end = buf.data;
    return buf;
}

buffer_t buffer_realloc( buffer_t buf, int min_size )
{
    if ( min_size < buf.size )
        return buf;
    if ( buf.size * 2 > min_size )
        min_size = buf.size * 2;

    int len = buf.end - buf.data;
    buf.data = realloc( buf.data, min_size + 1 );
    buf.end = buf.data + len;
    buf.size = min_size + 1;
    return buf;
}

void buffer_free( buffer_t buf )
{
    free( buf.data );
}

buffer_t buffer_append( buffer_t buf, span_t str )
{
    buf = buffer_realloc( buf, buffer_len( buf ) + span_len( str ) );
    buf.end = span_copy( buf.end, str );
    return buf;
}

buffer_t buffer_append_char( buffer_t buf, char c )
{
    buf = buffer_realloc( buf, buffer_len( buf ) + 1 );
    *buf.end++ = c;
    return buf;
}
