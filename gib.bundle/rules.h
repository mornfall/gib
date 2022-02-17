#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"
#include "queue.h"
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
    const char *srcdir;
    bool out_set, cmd_set, meta_set;
    bool stanza_started;
    struct rl_stack *stack;

    int srcdir_fd;
    queue_t *queue;
};

void load_rules( cb_tree *nodes, cb_tree *env, queue_t *q, int srcdir_fd, node_t *node );

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

void rl_get_file( struct rl_state *s, node_t *n )
{
    if ( n->type != out_node )
        return; /* nothing to be done */

    queue_set_outdir( s->queue, s->globals );
    queue_add_goal( s->queue, n->name );
    queue_monitor( s->queue, false );

    if ( s->queue->failed_count != 0 )
        rl_error( s, "error building %s", n->name );
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

        if ( node->frozen )
            rl_error( s, "duplicate output: %s", name.str );

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
            cb_insert( &node->deps, dep_n, VSIZE( dep_n, name ), strlen( dep->data ) );
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
        var_t *cmd = env_set( &s->locals, span_lit( "cmd" ) );

        while ( !span_empty( args ) )
        {
            span_t word = fetch_word( &args );
            env_expand( cmd, &s->locals, s->globals, word, 0 );
        }

        if ( !cmd->list )
            rl_error( s, "empty command" );
    }

    else if ( span_eq( cmd, "src" ) )
    {
        span_t src_name = fetch_word( &args );
        span_t dir_name = fetch_word( &args );

        var_t *src = env_get( s->globals, src_name ) ?: env_set( s->globals, src_name );
        var_t *dir = env_get( s->globals, dir_name ) ?: env_set( s->globals, dir_name );

        var_t *path = var_alloc( span_lit( "manifest-path" ) );
        env_expand( path, &s->locals, s->globals, args, 0 );

        for ( value_t *val = path->list; val; val = val->next )
        {
            node_t *n = graph_find_file( s->nodes, span_lit( val->data ) );
            rl_get_file( s, n );
            load_manifest( s->nodes, src, dir, rl_dirfd( s, n ), val->data );
        }
    }

    else if ( span_eq( cmd, "out" ) || span_eq( cmd, "meta" ) )
    {
        if ( span_eq( cmd, "out" ) ) s->out_set = true; else s->meta_set = true;
        var_t *out = env_set( &s->locals, span_lit( "out" ) );
        env_expand( out, &s->locals, s->globals, args, 0 );
        if ( !out->list || out->list->next )
            rl_error( s, "out must expand into exactly one item" );
    }

    else if ( add || dep )
    {
        span_t name = dep ? span_lit( "dep" ) : fetch_word( &args );
        bool autovivify = true;
        var_t *var = env_resolve( &s->locals, s->globals, name, &autovivify );

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
            {
                char *name = new->data;

                if ( strncmp( name, s->srcdir, strlen( s->srcdir ) ) == 0 )
                    name += strlen( s->srcdir ) + 1;

                node_t *dep = graph_get( s->nodes, span_lit( name ) );

                if ( !dep || !dep->frozen )
                    rl_error( s, "dep: node for '%s' does not exist", new->data );

                memmove( new->data, name, strlen( name ) + 1 );
            }
    }

    else if ( set || let )
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

    else if ( span_eq( cmd, "use" ) )
    {
        span_t name = fetch_word( &args );
        var_t *var = env_get( &s->templates, name );

        if ( !var )
            rl_error( s, "undefined template %.*s\n", span_len( name ), name.str );

        assert( cb_contains( &s->positions, name ) );
        fileline_t *pos = cb_find( &s->positions, name ).leaf;
        rl_replay( s, var->list, *pos );
    }

    else if ( sub )
    {
        var_t *files = var_alloc( span_lit( "sub-files" ) );
        env_expand( files, &s->locals, s->globals, args, 0 );

        for ( value_t *val = files->list; val; val = val->next )
            if ( !ignore_missing || access( val->data, R_OK ) != -1 )
            {
                node_t *n = graph_find_file( s->nodes, span_lit( val->data ) );
                rl_get_file( s, n );
                load_rules( s->nodes, s->globals, s->queue, s->srcdir_fd, n );
            }
    }

    s->stanza_started = true;
}

void rl_for( struct rl_state *s, value_t *cmds, fileline_t pos );

void rl_replay( struct rl_state *s, value_t *stmt, fileline_t pos )
{
    fileline_t bak = s->reader.pos;
    s->reader.pos = pos; /* FIXME eww */

    while ( stmt )
    {
        span_t line = span_lit( stmt->data );
        span_t cmd = fetch_word( &line );

        if ( span_eq( cmd, "for" ) )
        {
            rl_for( s, stmt, pos );
            break;
        }

        rl_command( s, cmd, line );
        s->reader.pos.line ++;
        stmt = stmt->next;
    }

    s->reader.pos = bak;
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
        env_expand( iter, &s->locals, s->globals, word, 0 );
    }

    value_t *val = iter->list;
    cmds = cmds->next;

    while ( val )
    {
        rl_stanza_clear( s );
        env_dup( &s->locals, &saved );
        var_t *ivar = env_set( &s->locals, name );
        var_add( ivar, span_lit( val->data ) );
        rl_replay( s, cmds, pos );
        rl_stanza_end( s );
        val = val->next;
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
        rl_error( s, "def/for in the middle of a stanza" );

    rl_push( s, is_for ? "evaluating for loop" : "evaluating def" );

    span_t name = span_dup( fetch_word( &s->reader.span ) );
    span_t args = span_dup( s->reader.span );

    if ( env_get( s->globals, name ) )
        rl_error( s, "name '%.*s' is already used for a variable", span_len( name ), name.str );

    if ( is_def )
        rl_set_position( s, name );

    var_t *cmds = is_def ? env_set( &s->templates, name )
                         : var_alloc( span_lit( "for-body" ) );

    if ( is_for )
        var_add( cmds, line );

    while ( read_line( &s->reader ) && !span_empty( s->reader.span ) )
        var_add( cmds, s->reader.span );

    if ( is_for )
        rl_for( s, cmds->list, s->reader.pos );
    else
        cmds = 0;

    rl_pop( s );
    var_free( cmds );
    span_free( name );
    span_free( args );
}

void load_rules( cb_tree *nodes, cb_tree *env, queue_t *q, int srcdir_fd, node_t *node )
{
    struct rl_state s;

    s.globals = env;
    cb_init( &s.locals );
    cb_init( &s.templates );
    cb_init( &s.positions );
    s.nodes = nodes;
    s.queue = q;
    s.stack = 0;
    s.srcdir = env_get( env, span_lit( "srcdir" ) )->list->data;
    s.srcdir_fd = srcdir_fd;

    if ( node->stamp_changed > q->stamp_rules )
        q->stamp_rules = node->stamp_changed;

    if ( !reader_init( &s.reader, rl_dirfd( &s, node ), node->name ) )
        sys_error( "opening %s %s", node->type == out_node ? "output" : "source", node->name );

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
}
