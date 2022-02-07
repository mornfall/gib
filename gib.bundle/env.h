#pragma once
#include "common.h"
#include "span.h"
#include "critbit.h"
#define VSIZE( x, m ) ( ( int ) ( ( ( void * ) x->m ) - ( ( void * ) x ) ) )

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

void var_clear( var_t *var )
{
    /* TODO */
    cb_clear( &var->set );
    var->list = 0;
}

var_t *var_alloc( span_t name )
{
    var_t *var = calloc( 1, VSIZE( var, name ) + span_len( name ) + 1 );
    span_copy( var->name, name );
    return var;
}

void var_free( var_t *var )
{
    if ( !var )
        return;

    for ( value_t *v = var->list, *next = 0; v; v = next )
        next = v->next, free( v );

    free( var );
}

var_t *env_set( cb_tree *env, span_t name )
{
    var_t *var = env_get( env, name );

    if ( var )
        var_clear( var );
    else
    {
        var = var_alloc( name );
        cb_insert( env, var, VSIZE( var, name ), span_len( name ) );
    }

    return var;
}

void var_add_value( var_t *var, value_t *val )
{
    if ( var->frozen )
        error( "cannot change frozen variable %s", var->name );

    val->next = 0;
    cb_insert( &var->set, val, VSIZE( val, data ), -1 );

    if ( var->list )
    {
        var->last->next = val;
        var->last = val;
    }
    else
        var->list = var->last = val;
}

void var_add( var_t *var, span_t str )
{
    value_t *val = malloc( VSIZE( val, data ) + span_len( str ) + 1 );
    span_copy( val->data, str );
    var_add_value( var, val );
}

void env_add( cb_tree *env, span_t name, span_t val )
{
    var_t *var = env_get( env, name );
    assert( var );
    var_add( var, val );
}

span_t span_stem( span_t s )
{
    const char *end = s.end;

    while ( end > s.str && *--end != '/' )
        if ( *end == '.' )
            return s.end = end, s;

    return s;
}

void env_expand( var_t *var, cb_tree *local, cb_tree *global, span_t str, const char *ref );

void env_expand_rec( var_t *var, cb_tree *local, cb_tree *global,
                     span_t prefix, span_t value, span_t suffix )
{
    char *buffer;
    int len = asprintf( &buffer, "%.*s%.*s%.*s", span_len( prefix ), prefix.str,
                                                 span_len( value ),  value.str,
                                                 span_len( suffix ), suffix.str );

    const char *next_ref = buffer + span_len( prefix ) + span_len( value );
    env_expand( var, local, global, span_mk( buffer, buffer + len + 1 ), next_ref );
    free( buffer );
}

void env_expand( var_t *var, cb_tree *local, cb_tree *global, span_t str, const char *ref )
{
    if ( !ref )
        ref = str.str;

    while ( ref < str.end && *ref != '$' )
        ++ ref;

    if ( ref < str.end && ref + 2 >= str.end )
        goto err;

    if ( ref == str.end )
        return var_add( var, str );

    const char *ref_end = ref + 1;

    if ( *ref_end != '(' )
        goto err;

    while ( ref_end < str.end && *ref_end != ')' )
        ++ ref_end;

    span_t prefix = span_mk( str.str, ref );
    span_t ref_name = span_mk( ref + 2, ref_end );
    span_t ref_spec = ref_name;
    span_t ref_full = ref_name;
    span_t suffix = span_mk( ref_end + 1, str.end );

    const char *ref_ptr = ref_name.str;
    for ( ; ref_ptr < ref_name.end && !strchr( ":!.", *ref_ptr ); ++ ref_ptr );
    ref_name.end = ref_spec.str = ref_ptr;

    var_t *ref_var = env_get( local, ref_name ) ?: env_get( global, ref_name );

    if ( !ref_var )
        goto err;

    ref_var->frozen = true;

    char ref_type = span_len( ref_spec ) ? ref_spec.str[ 0 ] : 0;
    ref_spec.str ++;

    if ( ref_type == ':' )
    {
        span_t suffix_match = ref_spec;
        span_t prefix_match = fetch_until( &suffix_match, '*', 0 );

        if ( !span_empty( suffix_match ) )
            suffix_match.str ++;

        for ( cb_iterator i = cb_begin_at( &ref_var->set, prefix_match ); !cb_end( &i ); cb_next( &i ) )
        {
            value_t *val = cb_get( &i );

            if ( strncmp( val->data, prefix_match.str, span_len( prefix_match ) ) )
                break;

            if ( strncmp( val->data + strlen( val->data ) - span_len( suffix_match ),
                          suffix_match.str, span_len( suffix_match ) ) )
                continue;

            env_expand_rec( var, local, global, prefix, span_lit( val->data ), suffix );
        }

        return;
    }

    if ( ref_type == '.' )
    {
        span_t sub_ref = ref_spec;

        if ( span_len( ref_spec ) && ref_spec.str[ 0 ] == '$' )
        {
            ref_spec.str ++;
            var_t *spec_var = env_get( local, ref_spec ) ?: env_get( global, ref_spec );

            if ( !spec_var || !spec_var->list || spec_var->list->next )
                error( "substitution $(%.*s.%.*s) is invalid",
                       span_len( ref_name ), ref_name.str, span_len( ref_spec ), ref_spec.str );

            span_t sub_spec = span_lit( spec_var->list->data );
            char sub_name_buf[ span_len( ref_name ) + span_len( sub_spec ) + 2 ], *ptr = sub_name_buf;
            ptr = span_copy( ptr, ref_name );
            *ptr++ = '.';
            ptr = span_copy( ptr, sub_spec );
            span_t sub_name = span_mk( sub_name_buf, ptr );
            ref_var = env_get( local, sub_name ) ?: env_get( global, sub_name );
        }
        else
            ref_var = env_get( local, ref_full ) ?: env_get( global, ref_full );

        ref_type = 0;

        if ( !ref_var ) /* unlike other variable references, non-existence is the same as being empty */
            return;
    }

    for ( value_t *val = ref_var->list; val; val = val->next )
    {
        span_t data = span_lit( val->data );

        if ( ref_type == '!' )
        {
            if ( span_eq( ref_spec, "stem" ) )
                data = span_stem( data );
            else
                error( "unknown substitution operator %.*s\n", span_len( ref_spec ), ref_spec.str );
        }
        else if ( ref_type )
            error( "unknown substitution type %c\n", ref_type );

        env_expand_rec( var, local, global, prefix, data, suffix );
    }

    return;
err:
    error( "invalid variable reference in %.*s", span_len( str ), str );
}
