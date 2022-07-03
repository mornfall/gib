#pragma once
#include <ctype.h>
#include "common.h"
#include "span.h"
#include "critbit.h"
#include "sha1.h"

typedef struct value
{
    struct value *next;
    char data[];
} value_t;

typedef struct
{
    cb_tree set;
    value_t *list, *last;
    bool frozen;
    char name[];
} var_t;

var_t *env_get( cb_tree *env, span_t name )
{
    var_t *var = cb_find( env, name ).leaf;

    if ( var && span_eq( name, var->name ) )
        return var;
    else
        return 0;
}

var_t *var_alloc( span_t name )
{
    var_t *var = calloc( 1, SIZE_NAMED( var_t, span_len( name ) ) );
    span_copy( var->name, name );
    return var;
}

void var_free( var_t *var )
{
    if ( !var )
        return;

    cb_clear( &var->set, false );

    for ( value_t *v = var->list, *next = 0; v; v = next )
        next = v->next, free( v );
}

void var_clear( location_t *loc, var_t *var )
{
    if ( var->frozen )
        error( loc, "cannot reset frozen variable %s", var->name );

    var_free( var );
    var->list = NULL;
}

void env_clear( cb_tree *env, bool release )
{
    if ( release )
        for ( cb_iterator i = cb_begin( env ); !cb_end( &i ); cb_next( &i ) )
            var_free( cb_get( &i ) );

    cb_clear( env, release );
}

var_t *env_set( location_t *loc, cb_tree *env, span_t name )
{
    var_t *var = env_get( env, name );

    if ( var )
        var_clear( loc, var );
    else
    {
        var = var_alloc( name );
        cb_insert( env, var, offsetof( var_t, name ), span_len( name ) );
    }

    return var;
}

void var_add_value( location_t *loc, var_t *var, value_t *val )
{
    if ( var->frozen )
        error( loc, "cannot change frozen variable %s", var->name );

    val->next = 0;
    cb_insert( &var->set, val, offsetof( value_t, data ), -1 );

    if ( var->list )
    {
        var->last->next = val;
        var->last = val;
    }
    else
        var->list = var->last = val;
}

void var_add( location_t *loc, var_t *var, span_t str )
{
    value_t *val = malloc( offsetof( value_t, data ) + span_len( str ) + 1 );
    span_copy( val->data, str );
    var_add_value( loc, var, val );
}

void var_reset( var_t *var, span_t str )
{
    var_clear( NULL, var );
    var_add( NULL, var, str );
}

void env_reset( cb_tree *env, span_t name, span_t str )
{
    var_add( NULL, env_set( NULL, env, name ), str );
}

uint64_t var_hash( value_t *head )
{
    sha1_ctx ctx;
    sha1_init( &ctx );

    union
    {
        uint8_t  sha[ SHA1_DIGEST_LENGTH ];
        uint64_t hash;
    } result;

    for ( value_t *val = head; val; val = val->next )
        sha1_update( &ctx, val->data, strlen( val->data ) + 1 );

    sha1_final( result.sha, &ctx );
    return result.hash;
}

void env_add( location_t *loc, cb_tree *env, span_t name, span_t val )
{
    var_t *var = env_get( env, name );
    assert( var );
    var_add( loc, var, val );
}

void env_dup( cb_tree *out, cb_tree *env )
{
    for ( cb_iterator i = cb_begin( env ); !cb_end( &i ); cb_next( &i ) )
    {
        var_t *old = cb_get( &i ); /* TODO copy the values */
        var_t *new = env_set( NULL, out, span_lit( old->name ) );

        for ( value_t *v = old->list; v; v = v->next )
            var_add( NULL, new, span_lit( v->data ) );

        cb_insert( out, new, offsetof( var_t, name ), -1 );
    }
}

var_t *env_resolve( location_t *loc, cb_tree *local, cb_tree *global, span_t spec, bool *autovivify )
{
    span_t sub = spec;
    span_t base = fetch_until( &sub, ".", 0 );
    bool make = *autovivify;

    if ( !span_empty( base ) && sub.str[ 0 ] == '$' )
    {
        sub.str ++;

        var_t *sub_var = env_get( local, sub ) ?: env_get( global, sub );

        if ( !sub_var )
            error( loc, "variable '%.*s not defined", span_len( sub ), sub.str );
        if ( !sub_var->list )
            return *autovivify = true, NULL;
        if ( sub_var->list->next )
            error( loc, "cannot expand non-singleton %.*s in %.*s",
                   span_len( sub ), sub.str,
                   span_len( spec ), spec.str );

        span_t sub_value = span_lit( sub_var->list->data );

        char buffer[ span_len( base ) + span_len( sub_value ) + 2 ], *ptr = buffer;
        ptr = span_copy( ptr, base );
        *ptr++ = '.';
        ptr = span_copy( ptr, sub_value );
        span_t ref = span_mk( buffer, ptr );

        cb_tree *use_env = env_get( local, base ) ? local : global;

        var_t *var = env_get( use_env, ref );
        *autovivify = true;

        return var ?: make ? env_set( loc, use_env, ref ) : NULL;
    }

    cb_tree *envs[ 3 ] = { local, global, NULL };

    for ( cb_tree **env = envs; *env; ++env )
        if ( !span_empty( base ) && env_get( *env, base ) )
        {
            *autovivify = true;
            var_t *var = env_get( *env, spec );
            return var ?: make ? env_set( loc, *env, spec ) : NULL;
        }

    *autovivify = false;
    return env_get( local, spec ) ?: env_get( global, spec );
}

typedef struct env_expand
{
    location_t *loc;
    var_t *var;
    cb_tree *local, *global;
    span_t capture[ 9 ];
} env_expand_t;

void env_expand_item( env_expand_t s, span_t prefix, span_t value, span_t suffix, bool replace );
void env_expand_list( env_expand_t s, span_t str, const char *ref );

void env_expand( location_t *loc, var_t *var, cb_tree *local, cb_tree *global, span_t str )
{
    env_expand_t s;
    s.loc = loc;
    s.var = var;
    s.local = local;
    s.global = global;
    memset( s.capture, 0, sizeof( s.capture ) );

    env_expand_list( s, str, 0 );
}

void env_expand_item( env_expand_t s, span_t prefix, span_t value, span_t suffix, bool replace )
{
    buffer_t buffer = buffer_alloc( span_len( prefix ) + span_len( value ) + span_len( suffix ) + 1 );
    buffer = buffer_append( buffer, prefix );
    bool escape = false;

    while ( replace && !span_empty( value ) )
    {
        if ( value.str[ 0 ] == '\\' )
            escape = true, value.str ++;
        else if ( !escape && span_len( value ) >= 2 &&
                  value.str[ 0 ] == '$' && isdigit( value.str[ 1 ] ) )
        {
            buffer = buffer_append( buffer, s.capture[ value.str[ 1 ] - '1' ] );
            value.str += 2;
        }
        else
        {
            buffer = buffer_append_char( buffer, *value.str++ );
            escape = false;
        }
    }

    if ( !replace )
        buffer = buffer_append( buffer, value );

    int suffix_offset = buffer_len( buffer );
    buffer = buffer_append( buffer, suffix );

    env_expand_list( s, buffer_span( buffer ), buffer.data + suffix_offset );
    buffer_free( buffer );
}

void env_expand_match( env_expand_t s, var_t *var, span_t spec, span_t prefix, span_t suffix )
{
    span_t replacement  = spec;
    span_t pattern_str  = fetch_until( &replacement, ":", '\\' );
    var_t *pattern_var  = var_alloc( span_lit( "temporary" ) );
    env_expand( s.loc, pattern_var, s.local, s.global, pattern_str );
    /* TODO: quote * and % that came in from expansion */

    for ( value_t *pat_item = pattern_var->list; pat_item; pat_item = pat_item->next )
    {
        span_t pattern    = span_lit( pat_item->data );
        span_t pat_suffix = pattern;
        span_t pat_prefix = fetch_until( &pat_suffix, "*%", '\\' );

        for ( cb_iterator i = cb_begin_at( &var->set, pat_prefix ); !cb_end( &i ); cb_next( &i ) )
        {
            value_t *val = cb_get( &i );
            span_t val_str = span_lit( val->data );
            bool replace = !span_empty( replacement );

            if ( strncmp( val->data, pat_prefix.str, span_len( pat_prefix ) ) )
            {
                cb_iterator_free( &i );
                break;
            }

            if ( !span_empty( pat_suffix ) && !span_match( pattern, val_str, s.capture ) )
                continue;

            env_expand_item( s, prefix, replace ? replacement : val_str, suffix, replace );
        }
    }

    var_free( pattern_var );
    free( pattern_var );
    return;
}

void env_expand_list( env_expand_t s, span_t str, const char *ref )
{
    if ( !ref )
        ref = str.str;

    while ( ref < str.end && *ref != '$' )
        ++ ref;

    if ( ref < str.end && ref + 2 >= str.end )
        error( s.loc, "unexpected $ at the end of string" );

    if ( ref == str.end )
        return var_add( s.loc, s.var, str );

    const char *ref_end = ref + 1;
    int counter = 0;

    if ( *ref_end != '(' )
        error( s.loc, "expected ( after $ in %.*s", span_len( str ), str );

    while ( ref_end < str.end )
    {
        if ( *ref_end == '(' )
            ++ counter;
        if ( *ref_end == ')' )
            if ( ! -- counter )
                break;

        ++ ref_end;
    }

    span_t prefix = span_mk( str.str, ref );
    span_t ref_name = span_mk( ref + 2, ref_end );
    span_t ref_spec = ref_name;
    span_t ref_full = ref_name;
    span_t suffix = span_mk( ref_end + 1, str.end );

    const char *ref_ptr = ref_name.str;

    for ( ; ref_ptr < ref_name.end && !strchr( ":~", *ref_ptr ); ++ ref_ptr );
    ref_name.end = ref_spec.str = ref_ptr;

    bool vivify = false;
    var_t *ref_var = env_resolve( s.loc, s.local, s.global, ref_name, &vivify );

    if ( !ref_var && vivify )
        return;
    if ( !ref_var )
        error( s.loc, "invalid variable reference %.*s", span_len( str ), str );

    ref_var->frozen = true;

    char ref_type = span_len( ref_spec ) ? ref_spec.str[ 0 ] : 0;
    ref_spec.str ++;

    if ( ref_type == ':' )
        return env_expand_match( s, ref_var, ref_spec, prefix, suffix );
    if ( ref_type == '~' )
        assert( false ); /* not implemented */

    for ( value_t *val = ref_var->list; val; val = val->next )
        env_expand_item( s, prefix, span_lit( val->data ), suffix, false );
}
