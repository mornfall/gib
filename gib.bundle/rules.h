#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"
#include "manifest.h"
#include "reader.h"

struct rl_stack
{
    fileline_t pos;
    const char *what;
    struct rl_stack *next;
};

struct rl_state /* rule loader */
{
    reader_t reader;
    cb_tree locals, *globals, templates;
    cb_tree positions;
    cb_tree *nodes;
    bool out_set, cmd_set, meta_set;
    bool stanza_started;
    struct rl_stack *stack;
};

void rl_error( struct rl_state *s, const char *reason, ... )
{
    va_list ap;
    va_start( ap, reason );

    for ( struct rl_stack *bt = s->stack; bt; bt = bt->next )
        fprintf( stderr, "%s:%d: %s\n", bt->pos.file, bt->pos.line, bt->what );

    fprintf( stderr, "%s:%d: ", s->reader.pos.file, s->reader.pos.line );
    vfprintf( stderr, reason, ap );
    fprintf( stderr, "\n" );

    va_end( ap );
    exit( 3 );
}

void rl_pop( struct rl_state *s )
{
    assert( s->stack );
    struct rl_stack *del = s->stack;
    s->stack = s->stack->next;
    free( del );
}

void rl_push( struct rl_state *s, const char *what )
{
    struct rl_stack *new = malloc( sizeof( struct rl_stack ) );
    new->pos  = s->reader.pos;
    new->what = what;
    new->next = s->stack;
    s->stack = new;
}

void rl_set_position( struct rl_state *s, span_t what )
{
    fileline_t *pos = malloc( VSIZE( pos, name ) + span_len( what ) + 1 );
    *pos = s->reader.pos;
    span_copy( pos->name, what );
    cb_insert( &s->positions, pos, VSIZE( pos, name ), span_len( what ) );
}

void rl_stanza_clear( struct rl_state *s )
{
    s->out_set = false;
    s->cmd_set = false;
    s->meta_set = false;
    s->stanza_started = false;
    cb_clear( &s->locals );
    env_set( &s->locals, span_lit( "dep" ) );
}

void rl_stanza_end( struct rl_state *s )
{
    if ( s->out_set || s->meta_set )
    {
        if ( s->out_set && s->meta_set )
            rl_error( s, "can't have both 'out' and 'meta' in the same stanza" );

        span_t name = span_lit( env_get( &s->locals, span_lit( "out" ) )->list->data );
        node_t *node = graph_add( s->nodes, name );
        node->type = out_node;

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
            cb_insert( &node->deps, dep_n, VSIZE( dep_n, name ), strlen( dep->data ) );
        }
    }

    rl_stanza_clear( s );
};

void rl_replay( struct rl_state *s, var_t *cmds, fileline_t pos );

void rl_command( struct rl_state *s, span_t cmd, span_t args )
{
    if ( span_eq( cmd, "cmd" ) )
    {
        s->cmd_set = true;
        var_t *cmd = env_set( &s->locals, span_lit( "cmd" ) );

        while ( !span_empty( args ) )
        {
            span_t word = fetch_word( &args );
            env_expand( cmd, &s->locals, s->globals, word, 0 );
        }
    }

    if ( span_eq( cmd, "src" ) )
    {
        var_t *src = env_get( s->globals, span_lit( "src" ) ) ?:
                     env_set( s->globals, span_lit( "src" ) );
        var_t *path = var_alloc( span_lit( "manifest-path" ) );
        env_expand( path, &s->locals, s->globals, args, 0 );
        for ( value_t *val = path->list; val; val = val->next )
            load_manifest( s->nodes, src, val->data );
    }

    if ( span_eq( cmd, "out" ) )
    {
        s->out_set = true;
        var_t *out = env_set( &s->locals, span_lit( "out" ) );
        env_expand( out, &s->locals, s->globals, args, 0 );
        if ( out->list->next )
            rl_error( s, "out expanded into a list" );
    }

    bool let = false, set = false, split = false, add = false;
    bool dep = span_eq( cmd, "dep" );
    if ( span_eq( cmd, "set" ) )  set = true, split = false;
    if ( span_eq( cmd, "set*" ) ) set = true, split = true;
    if ( span_eq( cmd, "let" ) )  let = true, split = false;
    if ( span_eq( cmd, "let*" ) ) let = true, split = true;
    if ( span_eq( cmd, "add" ) )  add = true, split = false;
    if ( span_eq( cmd, "add*" ) ) add = true, split = true;

    if ( add || dep )
    {
        span_t name = dep ? span_lit( "dep" ) : fetch_word( &args );
        var_t *var = env_get( &s->locals, name ) ?: env_get( s->globals, name );

        if ( !var )
            rl_error( s, "cannot add to a non-existent variable %.*s", span_len( name ), name.str );

        value_t *new = var->last;

        if ( split )
            while ( !span_empty( args ) )
            {
                span_t word = fetch_word( &args );
                env_expand( var, &s->locals, s->globals, word, 0 );
            }
        else
            env_expand( var, &s->locals, s->globals, args, 0 );

        if ( !new )
            new = var->list;

        if ( dep )
            for ( ; new; new = new->next )
                if ( !graph_get( s->nodes, span_lit( new->data ) ) )
                    rl_error( s, "dep: node for '%s' does not exist", new->data );
    }

    if ( set || let )
    {
        span_t name = fetch_word( &args );

        if ( env_get( &s->templates, name ) )
            rl_error( s, "name '%.*s' is already used for a template", span_len( name ), name.str );

        var_t *var = env_set( set ? s->globals : &s->locals, name );

        if ( set )
            rl_set_position( s, name );

        if ( split )
            while ( !span_empty( args ) )
            {
                span_t word = fetch_word( &args );
                env_expand( var, &s->locals, s->globals, word, 0 );
            }
        else
            if ( !span_empty( args ) )
                env_expand( var, &s->locals, s->globals, args, 0 );
    }

    if ( span_eq( cmd, "use" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_get( &s->templates, name );

        if ( !var )
            rl_error( s, "undefined template %.*s\n", span_len( name ), name.str );

        assert( cb_contains( &s->positions, name ) );
        fileline_t *pos = cb_find( &s->positions, name ).leaf;
        rl_replay( s, var, *pos );
    }

    s->stanza_started = true;
}

void rl_replay( struct rl_state *s, var_t *cmds, fileline_t pos )
{
    value_t *stmt = cmds->list;
    fileline_t bak = s->reader.pos;
    s->reader.pos = pos; /* FIXME eww */

    while ( stmt )
    {
        span_t line = span_lit( stmt->data );
        span_t cmd = fetch_word( &line );
        s->reader.pos.line ++;
        rl_command( s, cmd, line );
        stmt = stmt->next;
    }

    s->reader.pos = bak;
}

void rl_statement( struct rl_state *s )
{
    span_t cmd = fetch_word( &s->reader.span );
    var_t *iter = 0;

    if ( !span_eq( cmd, "def" ) && !span_eq( cmd, "for" ) )
        return rl_command( s, cmd, s->reader.span );

    if ( s->stanza_started )
        rl_error( s, "def in the middle of a stanza" );

    rl_push( s, span_eq( cmd, "for" ) ? "evaluating for loop" : "evaluating def" );

    span_t name = span_dup( fetch_word( &s->reader.span ) );
    span_t args = span_dup( s->reader.span );
    fileline_t pos = s->reader.pos;

    if ( env_get( s->globals, name ) )
        rl_error( s, "name '%.*s' is already used for a variable", span_len( name ), name.str );

    if ( span_eq( cmd, "def" ) )
        rl_set_position( s, name );

    var_t *cmds = span_eq( cmd, "def" ) ? env_set( &s->templates, name )
                                        : var_alloc( span_lit( "for-body" ) );

    while ( fetch_line( &s->reader ) && !span_empty( s->reader.span ) )
        var_add( cmds, s->reader.span );

    if ( span_eq( cmd, "def" ) )
    {
        cmds = 0;
        goto out;
    }

    iter = var_alloc( span_lit( "for-iter" ) );
    env_expand( iter, &s->locals, s->globals, args, 0 );
    value_t *val = iter->list;

    while ( val )
    {
        rl_stanza_clear( s );
        var_t *ivar = env_set( &s->locals, name );
        var_add( ivar, span_lit( val->data ) );
        rl_replay( s, cmds, pos );
        rl_stanza_end( s );
        val = val->next;
    }

out:
    rl_pop( s );
    var_free( cmds );
    var_free( iter );
    span_free( name );
    span_free( args );
}

void load_rules( cb_tree *nodes, cb_tree *env, const char *file )
{
    struct rl_state s;

    s.globals = env;
    cb_init( &s.locals );
    cb_init( &s.templates );
    cb_init( &s.positions );
    s.nodes = nodes;
    s.stack = 0;

    if ( !reader_init( &s.reader, file ) )
        sys_error( "opening %s", file );

    graph_add( nodes, span_lit( file ) );

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
