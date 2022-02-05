#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"

#define BUFFER 4096

typedef struct reader
{
    int fd;
    char buffer[ BUFFER ];
    int buffer_use;
    span_t span;

    const char *file;
    int line;
} reader_t;

bool reader_init( reader_t *r, const char *file )
{
    r->fd = open( file, O_RDONLY );
    r->span.str = r->span.end = r->buffer;
    r->buffer_use = 0;

    r->file = file;
    r->line = 0;

    return r->fd >= 0;
}

bool shift_buffer( reader_t *r )
{
    assert( r->buffer + r->buffer_use >= r->span.str );
    int delete = r->span.str - r->buffer;
    r->buffer_use -= delete;

    if ( r->buffer_use == BUFFER )
        die( "ran out of line buffer reading %s, line %d", r->file, r->line );

    memmove( r->buffer, r->span.str, r->buffer_use );

    r->span.str -= delete;
    r->span.end -= delete;

    int bytes = read( r->fd, r->buffer + r->buffer_use, BUFFER - r->buffer_use );

    if ( bytes == 0 )
        return false;

    if ( bytes < 0 )
    {
        perror( "read" );
        abort();
    }

    r->buffer_use += bytes;
    return true;
}

bool fetch_line( reader_t *r )
{
    r->span.str = r->span.end;
    r->span.end = r->span.str;

    do
    {
        if ( r->span.end >= r->buffer + r->buffer_use )
            if ( !shift_buffer( r ) )
                return false;
        r->span.end ++;
    }
    while ( *r->span.end != '\n' );

    if ( *r->span.str == '\n' )
        r->span.str ++;

    return true;
}

span_t fetch_word( span_t *in )
{
    span_t r = *in;

    while ( in->str < in->end && *in->str != ' ' )
        ++ in->str;

    r.end = in->str;

    while ( *in->str == ' ' )
        in->str ++;

    return r;
}

void repeat( const char *f )
{
}

struct rl_state /* rule loader */
{
    reader_t reader;
    cb_tree locals, globals, templates;
    bool out_set, cmd_set, meta_set;
    bool stanza_started;
};

void rl_stanza_clear( struct rl_state *s )
{
    s->out_set = false;
    s->cmd_set = false;
    s->meta_set = false;
    s->stanza_started = false;
    cb_clear( &s->locals );
}

void rl_stanza_end( struct rl_state *s )
{
    if ( s->out_set || s->meta_set )
    {
        if ( s->out_set && s->meta_set )
            rl_error( s, "can't have both 'out' and 'meta' in the same stanza" );

        span_t name = span_lit( env_get( &s->locals, span_lit( "out" ) )->list->data );
        node_t *node = graph_add( s->nodes, name );

        if ( !node )
            rl_error( s, "duplicate output: %s", name.str );

        if ( s->cmd_set )
        {
            node->cmd = env_get( &s->locals, span_lit( "cmd" ) )->list;
            /* TODO take ownership! rl_stanza_clear will destroy the variable */
        }

        value_t *dep = env_get( &s->locals, span_lit( "dep" ) )->list;

        for ( ; dep; dep = dep->next )
        {
            node_t *dep_n = graph_get( s->nodes, span_lit( dep->data ) );
            assert( dep_n );
        }
    }

    rl_stanza_clear( s );
};

void rl_replay( struct rl_state *s, var_t *cmds );

void rl_command( struct rl_state *s, span_t cmd, span_t args )
{
    if ( span_eq( cmd, "cmd" ) )
    {
        s->cmd_set = true;
        var_t *cmd = env_set( &s->locals, span_lit( "cmd" ) );

        while ( !span_empty( args ) )
        {
            span_t word = fetch_word( &args );
            env_expand( cmd, &s->locals, &s->globals, word, 0 );
        }

        fprintf( stderr, "cmd: " );
        for ( value_t *v = cmd->list; v; v = v->next )
            fprintf( stderr, "'%s' ", v->data );
        fprintf( stderr, "\n" );
    }

    if ( span_eq( cmd, "out" ) )
    {
        s->out_set = true;
        var_t *out = env_set( &s->locals, span_lit( "out" ) );
        env_expand( out, &s->locals, &s->globals, args, 0 );
        if ( out->list->next )
            die( "out expanded into a list" );
    }

    if ( span_eq( cmd, "add" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_get( &s->locals, name ) ?: env_get( &s->globals, name );

        if ( var )
            env_expand( var, &s->locals, &s->globals, args, 0 );
        else
            die( "cannot add to a non-existent variable %.*s", span_len( name ), name.str );
    }

    if ( span_eq( cmd, "set" ) || span_eq( cmd, "let" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_set( span_eq( cmd, "set" ) ? &s->globals : &s->locals, name );

        if ( !span_empty( args ) )
        {
            fprintf( stderr, "set %.*s to '%.*s'\n", span_len( name ), name.str,
                                                     span_len( args ), args.str );
            env_expand( var, &s->locals, &s->globals, args, 0 );
        }
    }

    if ( span_eq( cmd, "use" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_get( &s->templates, name );

        if ( !var )
            die( "undefined template %.*s\n", span_len( name ), name.str );

        rl_replay( s, var );
    }

    s->stanza_started = true;
}

void rl_replay( struct rl_state *s, var_t *cmds )
{
    value_t *stmt = cmds->list;

    while ( stmt )
    {
        span_t line = span_lit( stmt->data );
        span_t cmd = fetch_word( &line );
        rl_command( s, cmd, line );
        stmt = stmt->next;
    }
}

void rl_statement( struct rl_state *s )
{
    span_t cmd = fetch_word( &s->reader.span );

    if ( !span_eq( cmd, "def" ) && !span_eq( cmd, "for" ) )
        return rl_command( s, cmd, s->reader.span );

    if ( s->stanza_started )
        die( "def in the middle of a stanza" );

    span_t name = span_dup( fetch_word( &s->reader.span ) );
    span_t args = span_dup( s->reader.span );

    var_t *cmds = span_eq( cmd, "def" ) ? env_set( &s->templates, name )
                                        : var_alloc( span_lit( "for-body" ) );

    while ( fetch_line( &s->reader ) && !span_empty( s->reader.span ) )
        var_add( cmds, s->reader.span );

    if ( !span_eq( cmd, "for" ) )
        goto out;

    var_t *iter = var_alloc( span_lit( "for-iter" ) );
    env_expand( iter, &s->locals, &s->globals, args, 0 );
    value_t *val = iter->list;

    fprintf( stderr, "for: %.*s, %.*s\n", span_len( name ), name.str, span_len( args ), args.str );

    while ( val )
    {
        rl_stanza_clear( s );
        var_t *ivar = env_set( &s->locals, name );
        var_add( ivar, span_lit( val->data ) );
        rl_replay( s, cmds );
        rl_stanza_end( s );
        val = val->next;
    }

out:
    span_free( name );
    span_free( args );
}

void load_rules( cb_tree *nodes, cb_tree *env, const char *file )
{
    struct rl_state s;

    cb_init( &s.globals );
    cb_init( &s.locals );
    cb_init( &s.templates );

    if ( !reader_init( &s.reader, file ) )
        die( "opening %s", file );

    rl_stanza_clear( &s );

    while ( fetch_line( &s.reader ) )
    {
        if ( *s.reader.span.str == '#' )
            continue;

        if ( span_empty( s.reader.span ) )
            rl_stanza_end( &s );
        else
            rl_statement( &s );
    }

    rl_stanza_end( &s );
}

void load_dynamic( cb_tree *nodes, const char *file )
{}

void load_stamps( cb_tree *nodes, const char *file )
{}
