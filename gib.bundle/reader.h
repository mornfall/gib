#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"

#define BUFFER 4096

typedef struct fileline
{
    const char*file;
    int line;
    char name[0];
} fileline_t;

typedef struct reader
{
    int fd;
    char buffer[ BUFFER ];
    int buffer_use;
    span_t span;
    fileline_t pos;
} reader_t;

bool reader_init( reader_t *r, const char *file )
{
    r->fd = open( file, O_RDONLY );
    r->span.str = r->span.end = r->buffer;
    r->buffer_use = 0;

    r->pos.file = file;
    r->pos.line = 0;

    return r->fd >= 0;
}

bool shift_buffer( reader_t *r )
{
    assert( r->buffer + r->buffer_use >= r->span.str );
    int delete = r->span.str - r->buffer;
    r->buffer_use -= delete;

    if ( r->buffer_use == BUFFER )
        error( "ran out of line buffer reading %s, line %d", r->pos.file, r->pos.line );

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

    r->pos.line ++;

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

struct rl_stack
{
    fileline_t pos;
    const char *what;
    struct rl_stack *next;
};

struct rl_state /* rule loader */
{
    reader_t reader;
    cb_tree locals, globals, templates;
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
        env_set( &s->locals, span_lit( "dep" ) );
        env_expand( out, &s->locals, &s->globals, args, 0 );
        if ( out->list->next )
            rl_error( s, "out expanded into a list" );
    }

    if ( span_eq( cmd, "add" ) || span_eq( cmd, "dep" ) )
    {
        span_t name = span_eq( cmd, "dep" ) ? span_lit( "dep" ) : fetch_word( &args );
        var_t *var = env_get( &s->locals, name ) ?: env_get( &s->globals, name );

        if ( !var )
            rl_error( s, "cannot add to a non-existent variable %.*s", span_len( name ), name.str );

        value_t *new = var->last;
        env_expand( var, &s->locals, &s->globals, args, 0 );
        if ( !new )
            new = var->list;

        if ( span_eq( cmd, "dep" ) )
            for ( ; new; new = new->next )
                if ( !graph_get( s->nodes, span_lit( new->data ) ) )
                    rl_error( s, "dep: node for '%s' does not exist", new->data );
    }

    if ( span_eq( cmd, "set" ) || span_eq( cmd, "let" ) )
    {
        span_t name = fetch_word( &args );

        if ( env_get( &s->templates, name ) )
            rl_error( s, "name '%.*s' is already used for a template", span_len( name ), name.str );

        var_t *var = env_set( span_eq( cmd, "set" ) ? &s->globals : &s->locals, name );

        if ( span_eq( cmd, "set" ) )
            rl_set_position( s, name );

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
            rl_error( s, "undefined template %.*s\n", span_len( name ), name.str );

        assert( cb_contains( &s->positions, name.str, span_len( name ) ) );
        fileline_t *pos = cb_find( &s->positions, name.str, span_len( name ) ).leaf;
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

    if ( !span_eq( cmd, "def" ) && !span_eq( cmd, "for" ) )
        return rl_command( s, cmd, s->reader.span );

    if ( s->stanza_started )
        rl_error( s, "def in the middle of a stanza" );

    rl_push( s, span_eq( cmd, "for" ) ? "evaluating for loop" : "evaluating def" );

    span_t name = span_dup( fetch_word( &s->reader.span ) );
    span_t args = span_dup( s->reader.span );
    fileline_t pos = s->reader.pos;

    if ( env_get( &s->globals, name ) )
        rl_error( s, "name '%.*s' is already used for a variable", span_len( name ), name.str );

    if ( span_eq( cmd, "def" ) )
        rl_set_position( s, name );

    var_t *cmds = span_eq( cmd, "def" ) ? env_set( &s->templates, name )
                                        : var_alloc( span_lit( "for-body" ) );

    while ( fetch_line( &s->reader ) && !span_empty( s->reader.span ) )
        var_add( cmds, s->reader.span );

    if ( span_eq( cmd, "def" ) )
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
        rl_replay( s, cmds, pos );
        rl_stanza_end( s );
        val = val->next;
    }

out:
    rl_pop( s );
    span_free( name );
    span_free( args );
}

void load_rules( cb_tree *nodes, cb_tree *env, const char *file )
{
    struct rl_state s;

    cb_init( &s.globals );
    cb_init( &s.locals );
    cb_init( &s.templates );
    cb_init( &s.positions );
    s.nodes = nodes;
    s.stack = 0;

    if ( !reader_init( &s.reader, file ) )
        sys_error( "opening %s", file );

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
