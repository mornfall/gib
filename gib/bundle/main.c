#include "outdb.h"
#include "rules.h"
#include "graph.h"
#include "queue.h"
#include <sys/utsname.h>

typedef enum
{
    match_substr, /* substring match on goal name */
    match_exact   /* exact-matched goal or a variable */
} match_type_t;

typedef enum
{
    match_op_none, /* do nothing */
    match_op_intersect,
    match_op_union,
    match_op_subtract
} match_op_t;

typedef struct selector
{
    match_type_t type;
    match_op_t op;
    span_t string;
    cb_tree matched;
    bool clear_matched;
    struct selector *next;
} selector_t;

typedef struct
{
    queue_t queue;

    char *srcdir;
    FILE *debug;

    cb_tree env;
    cb_tree nodes;

    selector_t *select_head, *select_tail;
    cb_tree goals;
} state_t;

void state_init( state_t *s )
{
    s->srcdir = getcwd( 0, 0 );

    cb_init( &s->env );
    cb_init( &s->nodes );

    struct utsname uts;

    if ( uname( &uts ) < 0 )
        sys_error( "uname" );

    for ( char *s = uts.sysname; *s != 0; ++s )
        if ( isupper( *s ) )
            *s += 32;

    var_t *srcdir = env_set( &s->env, span_lit( "srcdir" ) );
    var_add( srcdir, span_lit( s->srcdir ) );
    var_t *uname = env_set( &s->env, span_lit( "uname" ) );
    var_add( uname, span_lit( uts.sysname ) );
}

void state_load( state_t *s )
{
    var_t *jobs_var, *outdir_var;
    const char *main = "gib/main";

    int srcdir_fd = open( ".", O_DIRECTORY | O_CLOEXEC );
    if ( srcdir_fd < 0 )
        sys_error( "opening source directory" );

    if ( faccessat( srcdir_fd, "gibfile", R_OK, 0 ) == 0 )
        main = "gibfile";

    queue_init( &s->queue, &s->nodes, s->srcdir );
    load_rules( &s->nodes, &s->env, &s->queue, srcdir_fd,
                graph_find_file( &s->nodes, span_lit( main ) ) );
    queue_set_outdir( &s->queue, &s->env );

    if ( ( jobs_var = env_get( &s->env, span_lit( "jobs" ) ) ) && jobs_var->list )
        s->queue.running_max = atoi( jobs_var->list->data );

    int debug_fd = openat( s->queue.outdir_fd, "gib.debug",
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666 );

    if ( debug_fd < 0 )
        sys_error( "opening gib.debug for writing" );
    else
        s->debug = fdopen( debug_fd, "w" );
}

void state_save( state_t *s )
{
    write_stamps( &s->nodes, s->queue.outdir_fd, "gib.stamps" );
    save_dynamic( &s->nodes, s->queue.outdir_fd, "gib.dynamic" );
}

void state_destroy( state_t *s )
{
    /* … */
}

void selector_init( selector_t *sel, match_op_t op, match_type_t type, const char *arg )
{
    sel->op = op;
    sel->type = type;
    sel->string = span_lit( arg );
}

bool process_goal( state_t *s, const char *arg )
{
    selector_t *new = s->select_tail;
    match_type_t type = match_exact;
    int skip = 0;

    if ( strlen( arg ) >= 2 && arg[ 1 ] == '~' )
        type = match_substr, ++ skip;

    switch ( arg[ 0 ] )
    {
        /* exact matches (goal or variable name): 'm'atch, 'a'nd, 's'kip */
        case ':': selector_init( new, match_op_intersect, type,         arg + 1 + skip ); break;
        case '+': selector_init( new, match_op_union,     type,         arg + 1 + skip ); break;
        case '/': selector_init( new, match_op_subtract,  type,         arg + 1 + skip ); break;
        case '%': selector_init( new, match_op_intersect, match_substr, arg + 1 ); break;

        default: selector_init( new, match_op_union,
                                arg[ 0 ] == '~' ? match_substr : match_exact,
                                arg + ( arg[ 0 ] == '~' ) ); break;
    }

    s->select_tail->next = calloc( 1, sizeof( selector_t ) );
    s->select_tail = s->select_tail->next;

    return true;
}

void usage() {}

void parse_options( state_t *s, int argc, char *argv[] )
{
    int ch;

    s->select_head = s->select_tail = calloc( 1, sizeof( selector_t ) );

    for ( int i = 1; i < argc; ++i )
        process_goal( s, argv[ i ] );

    if ( s->select_head == s->select_tail )
        process_goal( s, "all" );
}

void selector_fill( state_t *s, selector_t *sel )
{
    cb_init( &sel->matched );
    cb_tree *subset = sel->op == match_op_union ? &s->nodes : &s->goals;

    if ( sel->type == match_substr )
        for ( cb_iterator i = cb_begin( subset ); !cb_end( &i ); cb_next( &i ) )
        {
            sel->clear_matched = true;
            node_t *node = cb_get( &i );
            if ( strstr( node->name, sel->string.str ) ) /* sel->string is 0-terminated */
                cb_insert( &sel->matched, node, offsetof( node_t, name ), -1 );
        }
    else
    {
        if ( cb_contains( &s->nodes, sel->string ) )
        {
            sel->clear_matched = true;
            cb_result r = cb_find( &s->nodes, sel->string );
            cb_insert( &sel->matched, r.leaf, r.vsize, -1 );
        }
        else
        {
            var_t *var = env_get( &s->env, sel->string );
            if ( var )
                sel->matched = var->set; /* share */
            else
                error( "nothing is known about '%s'", sel->string );
        }
    }
}

void update_goals( state_t *s, selector_t *sel )
{
    cb_tree target;
    cb_init( &target );

    if ( s->goals.root == s->nodes.root && sel->op == match_op_union )
        sel->op = match_op_intersect;

    if ( sel->op == match_op_union )
    {
        for ( cb_iterator i = cb_begin( &sel->matched ); !cb_end( &i ); cb_next( &i ) )
        {
            node_t *node = cb_get( &i );
            cb_insert( &s->goals, node, offsetof( node_t, name ), -1 );
        }
    }
    else
    {
        for ( cb_iterator i = cb_begin( &s->goals ); !cb_end( &i ); cb_next( &i ) )
        {
            node_t *node = cb_get( &i );
            bool found = cb_contains( &sel->matched, span_lit( node->name ) );

            if ( sel->op == match_op_intersect &&  found ||
                 sel->op == match_op_subtract  && !found )
            {
                cb_insert( &target, node, offsetof( node_t, name ), -1 );
            }
        }

        if ( s->goals.root != s->nodes.root )
            cb_clear( &s->goals );
    }

    s->goals = target;
}

int main( int argc, char *argv[] )
{
    state_t s;

    state_init( &s );
    parse_options( &s, argc, argv );
    state_load( &s );

    cb_init( &s.goals );
    s.goals = s.nodes; /* share */

    for ( selector_t *sel = s.select_head; sel; sel = sel->next )
        if ( sel->op != match_op_none )
        {
            selector_fill( &s, sel );
            update_goals( &s, sel );
        }

    for ( cb_iterator i = cb_begin( &s.goals ); !cb_end( &i ); cb_next( &i ) )
        queue_create_jobs( &s.queue, cb_get( &i ) );

    for ( cb_iterator i = cb_begin( &s.nodes ); !cb_end( &i ); cb_next( &i ) )
        queue_cleanup_node( &s.queue, cb_get( &i ) );

    graph_dump( s.debug, &s.nodes );
    queue_monitor( &s.queue, true );
    state_save( &s );

    return s.queue.failed_count > 0;
}
