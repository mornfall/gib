#pragma once
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

void var_clear( var_t *var )
{
    /* TODO */
    cb_clear( &var->set );
    var->list = 0;

    if ( var->frozen )
        error( "cannot reset frozen variable %s", var->name );
}

var_t *var_alloc( span_t name )
{
    var_t *var = calloc( 1, offsetof( var_t, name ) + span_len( name ) + 1 );
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
        cb_insert( env, var, offsetof( var_t, name ), span_len( name ) );
    }

    return var;
}

void var_add_value( var_t *var, value_t *val )
{
    if ( var->frozen )
        error( "cannot change frozen variable %s", var->name );

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

void var_add( var_t *var, span_t str )
{
    value_t *val = malloc( offsetof( value_t, data ) + span_len( str ) + 1 );
    span_copy( val->data, str );
    var_add_value( var, val );
}

void var_reset( var_t *var, span_t str )
{
    var_clear( var );
    var_add( var, str );
}

void env_reset( cb_tree *env, span_t name, span_t str )
{
    var_add( env_set( env, name ), str );
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

void env_add( cb_tree *env, span_t name, span_t val )
{
    var_t *var = env_get( env, name );
    assert( var );
    var_add( var, val );
}

void env_dup( cb_tree *out, cb_tree *env )
{
    for ( cb_iterator i = cb_begin( env ); !cb_end( &i ); cb_next( &i ) )
    {
        var_t *var = cb_get( &i ); /* TODO copy the values */
        cb_insert( out, var, offsetof( var_t, name ), -1 );
    }
}

span_t span_stem( span_t s )
{
    const char *end = s.end;

    while ( end > s.str && *--end != '/' )
        if ( *end == '.' )
        {
            s.end = end;
            break;
        }

    return s;
}

span_t span_base( span_t s )
{
    const char *dirsep = s.end;
    while ( dirsep > s.str && *--dirsep != '/' );
    return span_mk( *dirsep == '/' ? dirsep + 1 : dirsep, s.end );
}

span_t env_expand_singleton( cb_tree *local, cb_tree *global, span_t str )
{
    var_t *var = env_get( local, str ) ?: env_get( global, str );

    if ( !var || !var->list || var->list->next )
        error( "expansion '%.*s is not a singleton", span_len( str ), str.str );

    return span_lit( var->list->data );
}

var_t *env_resolve( cb_tree *local, cb_tree *global, span_t spec, bool *autovivify )
{
    span_t sub = spec;
    span_t base = fetch_until( &sub, '.', 0 );
    bool make = *autovivify;

    if ( !span_empty( base ) && sub.str[ 0 ] == '$' )
    {
        sub.str ++;
        span_t sub_value = env_expand_singleton( local, global, sub );

        char buffer[ span_len( base ) + span_len( sub_value ) + 2 ], *ptr = buffer;
        ptr = span_copy( ptr, base );
        *ptr++ = '.';
        ptr = span_copy( ptr, sub_value );
        span_t ref = span_mk( buffer, ptr );

        cb_tree *use_env = env_get( local, base ) ? local : global;

        var_t *var = env_get( use_env, ref );
        *autovivify = true;

        return var ?: make ? env_set( use_env, ref ) : NULL;
    }

    cb_tree *envs[ 3 ] = { local, global, NULL };

    for ( cb_tree **env = envs; *env; ++env )
        if ( !span_empty( base ) && env_get( *env, base ) )
        {
            *autovivify = true;
            var_t *var = env_get( *env, spec );
            return var ?: make ? env_set( *env, spec ) : NULL;
        }

    *autovivify = false;
    return env_get( local, spec ) ?: env_get( global, spec );
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
    int counter = 0;

    if ( *ref_end != '(' )
        goto err;

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

    for ( ; ref_ptr < ref_name.end && !strchr( ":!", *ref_ptr ); ++ ref_ptr );
    ref_name.end = ref_spec.str = ref_ptr;

    bool vivify = false;
    var_t *ref_var = env_resolve( local, global, ref_name, &vivify );

    if ( !ref_var && vivify )
        return;
    if ( !ref_var )
        goto err;

    ref_var->frozen = true;

    char ref_type = span_len( ref_spec ) ? ref_spec.str[ 0 ] : 0;
    ref_spec.str ++;

    if ( ref_type == ':' )
    {
        span_t suffix_match = ref_spec;
        span_t prefix_match = fetch_until( &suffix_match, '*', 0 );

        var_t *prefix_var = var_alloc( span_lit( "temporary" ) );
        env_expand( prefix_var, local, global, prefix_match, 0 );

        for ( value_t *pmatch = prefix_var->list; pmatch; pmatch = pmatch->next )
        {
            span_t pmatch_str = span_lit( pmatch->data );

            for ( cb_iterator i = cb_begin_at( &ref_var->set, pmatch_str ); !cb_end( &i ); cb_next( &i ) )
            {
                value_t *val = cb_get( &i );

                if ( strncmp( val->data, pmatch_str.str, span_len( pmatch_str ) ) )
                    break;

                if ( strncmp( val->data + strlen( val->data ) - span_len( suffix_match ),
                              suffix_match.str, span_len( suffix_match ) ) )
                    continue;

                env_expand_rec( var, local, global, prefix, span_lit( val->data ), suffix );
            }
        }

        var_free( prefix_var );
        return;
    }

    for ( value_t *val = ref_var->list; val; val = val->next )
    {
        span_t data = span_lit( val->data );

        if ( ref_type == '!' )
        {
            if ( span_eq( ref_spec, "stem" ) )
                data = span_stem( data );
            else if ( span_eq( ref_spec, "base" ) )
                data = span_base( data );
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
