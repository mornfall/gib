#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"
#include "queue.h"
#include "manifest.h"
#include "reader.h"
#include "error.h"

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
    cb_tree *nodes;
    const char *srcdir;
    bool out_set, cmd_set, meta_set;
    bool stanza_started;
    struct location *loc;

    int srcdir_fd;
    queue_t *queue;
};

void load_rules( cb_tree *nodes, cb_tree *env, queue_t *q, int srcdir_fd, node_t *node, location_t *loc );

void rl_get_file( struct rl_state *s, node_t *n )
{
    if ( n->type != out_node )
        return; /* nothing to be done */

    queue_set_outdir( s->queue, s->globals );
    queue_add_goal( s->queue, n->name );
    queue_monitor( s->queue, false );

    if ( s->queue->failed_count != 0 )
        error( s->loc, "error building %s", n->name );
}

int rl_dirfd( struct rl_state *s, node_t *node )
{
    return node->type == out_node ? s->queue->outdir_fd : s->srcdir_fd;
}

void rl_stanza_clear( struct rl_state *s )
{
    s->out_set = false;
    s->cmd_set = false;
    s->meta_set = false;
    s->stanza_started = false;
    cb_clear( &s->locals );
    env_set( s->loc, &s->locals, span_lit( "dep" ) );
}

void rl_stanza_end( struct rl_state *s )
{
    if ( s->out_set || s->meta_set )
    {
        if ( s->out_set && s->meta_set )
            error( s->loc, "can't have both 'out' and 'meta' in the same stanza" );

        span_t name = span_lit( env_get( &s->locals, span_lit( "out" ) )->list->data );
        node_t *node = graph_add( s->nodes, name );

        if ( node->frozen )
            error( s->loc, "duplicate output: %s", name.str );

        node->type = s->meta_set ? meta_node : out_node;
        node->frozen = true;

        if ( s->cmd_set )
        {
            node->cmd = env_get( &s->locals, span_lit( "cmd" ) )->list;
            span_t argv_0 = span_lit( node->cmd->data );
            graph_add_dep( s->nodes, node, argv_0, false );
            /* TODO take ownership! rl_stanza_clear will destroy the variable */
        }

        value_t *dep = env_get( &s->locals, span_lit( "dep" ) )->list;

        for ( ; dep; dep = dep->next )
        {
            node_t *dep_n = graph_get( s->nodes, span_lit( dep->data ) );
            assert( dep_n );
            cb_insert( &node->deps, dep_n, offsetof( node_t, name ), strlen( dep->data ) );
        }
    }

    rl_stanza_clear( s );
}

void rl_replay( struct rl_state *s, value_t *cmds, fileline_t pos );

void rl_command( struct rl_state *s, span_t cmd, span_t args )
{
    bool let = false, set = false, split = false, add = false;
    bool sub = false, ignore_missing = false;
    bool dep = span_eq( cmd, "dep" );

    if      ( span_eq( cmd, "set=" ) )  set = true, split = false;
    else if ( span_eq( cmd, "set" ) ) set = true, split = true;
    else if ( span_eq( cmd, "let=" ) )  let = true, split = false;
    else if ( span_eq( cmd, "let" ) ) let = true, split = true;
    else if ( span_eq( cmd, "add=" ) )  add = true, split = false;
    else if ( span_eq( cmd, "add" ) ) add = true, split = true;
    else if ( span_eq( cmd, "sub" ) )  sub = true, ignore_missing = false;
    else if ( span_eq( cmd, "sub?" ) ) sub = true, ignore_missing = true;

    if ( span_eq( cmd, "cmd" ) )
    {
        s->cmd_set = true;
        var_t *cmd = env_set( s->loc, &s->locals, span_lit( "cmd" ) );

        while ( !span_empty( args ) )
        {
            span_t word = fetch_word( &args );
            env_expand( s->loc, cmd, &s->locals, s->globals, word );
        }

        if ( !cmd->list )
            error( s->loc, "empty command" );
    }

    else if ( span_eq( cmd, "src" ) )
    {
        span_t src_name = fetch_word( &args );
        span_t dir_name = fetch_word( &args );

        var_t *src = env_get( s->globals, src_name ) ?: env_set( s->loc, s->globals, src_name );
        var_t *dir = env_get( s->globals, dir_name ) ?: env_set( s->loc, s->globals, dir_name );

        var_t *path = var_alloc( span_lit( "manifest-path" ) );
        env_expand( s->loc, path, &s->locals, s->globals, args );

        for ( value_t *val = path->list; val; val = val->next )
        {
            node_t *n = graph_find_file( s->nodes, span_lit( val->data ) );
            rl_get_file( s, n );
            load_manifest( s->nodes, src, dir, s->srcdir_fd, rl_dirfd( s, n ), val->data );
        }
    }

    else if ( span_eq( cmd, "out" ) || span_eq( cmd, "meta" ) )
    {
        if ( span_eq( cmd, "out" ) ) s->out_set = true; else s->meta_set = true;
        var_t *out = env_set( s->loc, &s->locals, span_lit( "out" ) );
        env_expand( s->loc, out, &s->locals, s->globals, args );
        if ( !out->list || out->list->next )
            error( s->loc, "out must expand into exactly one item" );
    }

    else if ( add || dep )
    {
        span_t name = dep ? span_lit( "dep" ) : fetch_word( &args );
        bool autovivify = true;
        var_t *var = env_resolve( s->loc, &s->locals, s->globals, name, &autovivify );

        if ( !var )
            error( s->loc, "cannot add to a non-existent variable %.*s", span_len( name ), name.str );

        value_t *new = var->last;

        if ( split )
            while ( !span_empty( args ) )
            {
                span_t word = fetch_word( &args );
                env_expand( s->loc, var, &s->locals, s->globals, word );
            }
        else
            env_expand( s->loc, var, &s->locals, s->globals, args );

        if ( !new )
            new = var->list;

        if ( dep )
            for ( ; new; new = new->next )
            {
                char *name = new->data;

                if ( strncmp( name, s->srcdir, strlen( s->srcdir ) ) == 0 )
                    name += strlen( s->srcdir ) + 1;

                node_t *dep = graph_get( s->nodes, span_lit( name ) );

                if ( !dep || !dep->frozen )
                    error( s->loc, "dep: node for '%s' does not exist", new->data );

                memmove( new->data, name, strlen( name ) + 1 );
            }
    }

    else if ( set || let )
    {
        span_t name = fetch_word( &args );

        if ( env_get( &s->templates, name ) )
            error( s->loc, "name '%.*s' is already used for a template", span_len( name ), name.str );

        var_t *var = env_set( s->loc, set ? s->globals : &s->locals, name );

        if ( set )
            location_set( s->loc, name );

        if ( split )
            while ( !span_empty( args ) )
            {
                span_t word = fetch_word( &args );
                env_expand( s->loc, var, &s->locals, s->globals, word );
            }
        else
            env_expand( s->loc, var, &s->locals, s->globals, args );
    }

    else if ( span_eq( cmd, "use" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_get( &s->templates, name );

        if ( !var )
            error( s->loc, "undefined template %.*s\n", span_len( name ), name.str );

        fileline_t pos = location_push_named( s->loc, name, "in a macro defined here" );
        rl_replay( s, var->list, pos );
        location_pop( s->loc );
    }

    else if ( sub )
    {
        var_t *files = var_alloc( span_lit( "sub-files" ) );
        env_expand( s->loc, files, &s->locals, s->globals, args );

        for ( value_t *val = files->list; val; val = val->next )
            if ( !ignore_missing || access( val->data, R_OK ) != -1 )
            {
                location_push_current( s->loc, "included from here" );
                node_t *n = graph_find_file( s->nodes, span_lit( val->data ) );
                rl_get_file( s, n );
                load_rules( s->nodes, s->globals, s->queue, s->srcdir_fd, n, s->loc );
                location_pop( s->loc );
            }
    }

    s->stanza_started = true;
}

void rl_for( struct rl_state *s, value_t *cmds, fileline_t pos );

void rl_replay( struct rl_state *s, value_t *stmt, fileline_t pos )
{
    while ( stmt )
    {
        span_t line = span_lit( stmt->data );
        span_t cmd = fetch_word( &line );

        if ( span_eq( cmd, "for" ) )
        {
            rl_for( s, stmt, pos );
            break;
        }

        pos.line ++;
        location_push_fixed( s->loc, pos, NULL );
        rl_command( s, cmd, line );
        location_pop( s->loc );
        stmt = stmt->next;
    }
}

void rl_for( struct rl_state *s, value_t *cmds, fileline_t pos )
{
    span_t line = span_lit( cmds->data );
    fetch_word( &line ); /* 'for' */
    span_t name = fetch_word( &line );

    cb_tree saved;
    cb_init( &saved );
    env_dup( &saved, &s->locals );
    var_t *iter = var_alloc( span_lit( "for-iter" ) );

    while ( !span_empty( line ) )
    {
        span_t word = fetch_word( &line );
        env_expand( s->loc, iter, &s->locals, s->globals, word );
    }

    value_t *val = iter->list;
    cmds = cmds->next;
    char location[ 1024 ];

    while ( val )
    {
        snprintf( location, 1024, "while evaluating for loop with %.*s = %s",
                  span_len( name ), name.str, val->data );
        location_push_current( s->loc, location );
        rl_stanza_clear( s );
        env_dup( &s->locals, &saved );
        var_t *ivar = env_set( s->loc, &s->locals, name );
        var_add( s->loc, ivar, span_lit( val->data ) );
        rl_replay( s, cmds, pos );
        rl_stanza_end( s );
        val = val->next;
        location_pop( s->loc );
    }

    var_free( iter );
}

void rl_statement( struct rl_state *s )
{
    span_t line = s->reader.span;
    span_t cmd = fetch_word( &s->reader.span );

    bool is_def = span_eq( cmd, "def" );
    bool is_for = span_eq( cmd, "for" );

    if ( !is_def && !is_for )
        return rl_command( s, cmd, s->reader.span );

    if ( s->stanza_started )
        error( s->loc, "def/for in the middle of a stanza" );

    span_t name = span_dup( fetch_word( &s->reader.span ) );
    span_t args = span_dup( s->reader.span );

    if ( env_get( s->globals, name ) )
        error( s->loc, "name '%.*s' is already used for a variable", span_len( name ), name.str );

    if ( is_def )
        location_set( s->loc, name );

    var_t *cmds = is_def ? env_set( s->loc, &s->templates, name )
                         : var_alloc( span_lit( "for-body" ) );

    if ( is_for )
        var_add( s->loc, cmds, line );

    while ( read_line( &s->reader ) && !span_empty( s->reader.span ) )
        var_add( s->loc, cmds, s->reader.span );

    if ( is_for )
        rl_for( s, cmds->list, s->reader.pos );
    else
        cmds = 0;

    var_free( cmds );
    span_free( name );
    span_free( args );
}

void load_rules( cb_tree *nodes, cb_tree *env, queue_t *q, int srcdir_fd, node_t *node, location_t *loc )
{
    struct rl_state s;

    s.globals = env;
    cb_init( &s.locals );
    cb_init( &s.templates );
    s.loc = loc;
    s.nodes = nodes;
    s.queue = q;
    s.srcdir = env_get( env, span_lit( "srcdir" ) )->list->data;
    s.srcdir_fd = srcdir_fd;

    if ( node->stamp_changed > q->stamp_rules )
        q->stamp_rules = node->stamp_changed;

    if ( !reader_init( &s.reader, rl_dirfd( &s, node ), node->name ) )
        sys_error( s.loc, "opening %s %s", node->type == out_node ? "output" : "source", node->name );

    location_push_reader( s.loc, &s.reader );
    rl_stanza_clear( &s );

    while ( read_line( &s.reader ) )
    {
        if ( *s.reader.span.str == '#' )
            continue;

        if ( span_empty( s.reader.span ) )
            rl_stanza_end( &s );
        else
            rl_statement( &s );
    }

    rl_stanza_end( &s );
    location_pop( s.loc );
}
