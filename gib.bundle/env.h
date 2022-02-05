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
    var_t *var = cb_find( env, name.str, span_len( name ) ).leaf;

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

void var_add( var_t *var, span_t str )
{
    value_t *val = malloc( VSIZE( val, data ) + span_len( str ) + 1 );
    val->next = 0;
    span_copy( val->data, str );

    if ( var->list )
    {
        var->last->next = val;
        var->last = val;
    }
    else
        var->list = var->last = val;
}

void env_add( cb_tree *env, span_t name, span_t val )
{
    var_t *var = env_get( env, name );
    assert( var );
    var_add( var, val );
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
    span_t suffix = span_mk( ref_end + 1, str.end );

    var_t *ref_var = env_get( local, ref_name ) ?: env_get( global, ref_name );

    if ( !ref_var )
        goto err;

    for ( value_t *val = ref_var->list; val; val = val->next )
    {
        char *buffer;
        int len = asprintf( &buffer, "%.*s%s%.*s", span_len( prefix ), prefix.str,
                                                   val->data,
                                                   span_len( suffix ), suffix.str );

        const char *next_ref = buffer + span_len( prefix ) + strlen( val->data );
        env_expand( var, local, global, span_mk( buffer, buffer + len + 1 ), next_ref );
        free( buffer );
    }

    return;
err:
    die( "invalid variable reference in %.*s", span_len( str ), str );
}
