#include "outdb.h"
#include "rules.h"
#include "graph.h"
#include "job.h"
#include <sys/wait.h>
#include <sys/utsname.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#define MAX_FD 64

static volatile sig_atomic_t _signalled = 0;

void sighandler( int sig )
{
    _signalled = sig;
}

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
    char *srcdir,
         *outdir;

    int outdir_fd;

    cb_tree env;
    cb_tree nodes;
    cb_tree jobs;

    job_t *job_next, *job_last;
    job_t *running[ MAX_FD ];

    selector_t *select_head, *select_tail;
    cb_tree goals;

    int skipped_count;
    int failed_count;
    int ok_count;
    int todo_count;
    int queued_count;
    int running_count;
    int running_max;

    FILE *debug;
} state_t;

void job_show_result( state_t *s, node_t *n, job_t *j )
{
    const char *status = "??";
    int color = 0;
    bool changed = n->stamp_changed == n->stamp_want;
    bool updated = n->stamp_updated == n->stamp_want;

    if      ( n->failed )      status = "no", color = 31, s->failed_count ++;
    else if ( !changed )       status = "--", color = 33, s->skipped_count ++;
    else if ( j && j->warned ) status = "ok", color = 33, s->ok_count ++;
    else                       status = "ok", color = 32, s->ok_count ++;

    if ( !_signalled && n->failed || changed && j && j->warned )
    {
        fprintf( stderr, "\033[J" );
        char path[ strlen( n->name ) + 13 ];
        char *p = stpcpy( path, "_log/" );
        for ( char *i = n->name; *i; ++p, ++i )
            *p = ( *i == ' ' || *i == '/' ) ? '_' : *i;
        strcpy( p, ".txt" );

        reader_t log;

        if ( !reader_init( &log, s->outdir_fd, path ) )
            sys_error( "opening logfile %s", path );

        while ( read_line( &log ) )
            fprintf( stderr, " │ %.*s\n", span_len( log.span ), log.span.str );

        close( log.fd );
    }

    s->todo_count --;
    fprintf( stderr, "\033[J\033[%dm%s\033[0m %s\n", color, status, n->name );
}

void job_queue( state_t *s, job_t *j )
{
    assert( !j->queued );
    assert( j->node->dirty );

    fprintf( s->debug, "queue: %s\n", j->name );

    j->queued = true;
    ++ s->queued_count;

    if ( !s->job_next )
        s->job_next = j;

    if ( s->job_last )
        s->job_last->next = j;

    s->job_last = j;
}

bool job_start( state_t *s )
{
    if ( !s->job_next )
        return false;

    job_t *j = s->job_next;
    s->job_next = j->next;

    cb_clear( &j->node->deps_dyn );
    job_fork( j, s->outdir_fd );

    s->running_count ++;
    s->queued_count --;

    assert( j->pipe_fd < MAX_FD );
    s->running[ j->pipe_fd ] = j;

    return true;
}

void create_jobs( state_t *s, node_t *goal );

void job_skip( state_t *s, node_t *n )
{
    if ( n->failed )
        return;

    job_show_result( s, n, NULL );
    n->failed = true;

    for ( cb_iterator i = cb_begin( &n->blocking ); !cb_end( &i ); cb_next( &i ) )
        job_skip( s, cb_get( &i ) );
}

void node_cleanup( state_t *s, node_t *n )
{
    if ( n->type != out_node && n->type != meta_node || n->dirty || n->waiting )
        return;

    for ( cb_iterator i = cb_begin( &n->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );

        if ( ! -- b->waiting )
        {
            if ( b->dirty )
                job_queue( s, job_add( &s->jobs, b ) );
            else
            {
                node_cleanup( s, b );
                s->todo_count --;
            }
        }
    }

    cb_clear( &n->blocking );
}

void job_cleanup( state_t *s, int fd )
{
    job_t *j = s->running[ fd ];
    s->running[ fd ] = 0;
    node_t *n = j->node;

    int status;

    if ( waitpid( j->pid, &status, 0 ) == -1 )
        sys_error( "waitpid %d", j->pid );

    if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 )
    {
        n->stamp_updated = n->stamp_want;
        n->cmd_hash = var_hash( n->cmd );
        n->dirty = false;

        if ( j->changed )
            n->stamp_changed = n->stamp_want;
    }
    else
        n->failed = true;

    s->running_count --;
    job_show_result( s, n, j );

    for ( cb_iterator i = cb_begin( &j->node->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );

        if ( n->stamp_changed > b->stamp_updated )
            b->dirty = true;

        if ( n->failed && !b->failed )
            job_skip( s, b );

        if ( -- b->waiting )
            continue;

        if ( b->dirty && b->stamp_updated < b->stamp_want && !b->failed )
            job_queue( s, job_add( &s->jobs, b ) );
        else
            node_cleanup( s, b );
    }
}

void teardown( state_t *s )
{
    fprintf( stderr, "\033[J\r[caught signal %d, cleaning up]\n", _signalled );
    fflush( stderr );

    job_t *j = NULL;

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( ( j = s->running[ fd ] ) )
            kill( j->pid, SIGTERM );

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( s->running[ fd ] )
            job_cleanup( s, fd );
}

bool main_loop( state_t *s )
{
    fd_set ready, except;
    FD_ZERO( &ready );
    FD_ZERO( &except );

    if ( s->running_count )
    {
        for ( int i = 0; i < MAX_FD; ++i )
            if ( s->running[ i ] )
                FD_SET( i, &ready ), FD_SET( i, &except );

        struct timeval tv = { 1, 0 };

        switch ( select( MAX_FD, &ready, 0, &except, &tv ) )
        {
            case 0:
                return true;
            case -1:
                if ( errno != EINTR )
                    sys_error( "select" );
            default:
                ;
        }
    }

    if ( _signalled )
        teardown( s );

    if ( s->running_count )
        for ( int fd = 0; fd < MAX_FD; ++ fd )
            if ( FD_ISSET( fd, &ready ) || FD_ISSET( fd, &except ) )
                if ( job_update( s->running[ fd ], &s->nodes, s->srcdir ) )
                    job_cleanup( s, fd );

    if ( !_signalled )
        while ( s->running_count < s->running_max )
            if ( !job_start( s ) )
                break;

    return !_signalled && ( s->running_count || s->job_next );
}

void create_jobs( state_t *s, node_t *goal )
{
    if ( goal->visited )
        return;

    goal->visited = true;
    node_t *out = goal->type == out_node ? goal : 0;

    for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *dep = cb_get( &i );
        create_jobs( s, dep );

        if ( out && out->stamp_want < dep->stamp_want )
            out->stamp_want = dep->stamp_want;
    }

    if ( !out )
        return;

    if ( var_hash( out->cmd ) != out->cmd_hash )
        out->dirty = true;

    if ( out->stamp_want > out->stamp_updated || out->dirty )
    {
        for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
        {
            node_t *dep = cb_get( &i );
            node_t *dep_out = dep->type == out_node ? dep : 0;

            if ( dep_out && !dep_out->failed )
                if ( dep_out->stamp_want > dep_out->stamp_updated || dep_out->dirty )
                {
                    cb_insert( &dep->blocking, goal, VSIZE( goal, name ), -1 );
                    goal->waiting ++;
                }

            if ( dep->stamp_changed > out->stamp_updated )
                out->dirty = true;
        }
    }

    if ( out->waiting || out->dirty )
        ++ s->todo_count;

    if ( !out->waiting && out->dirty ) /* can run right away */
        job_queue( s, job_add( &s->jobs, out ) );
}

void set_goal( state_t *s, const char *name )
{
    node_t *goal = graph_get( &s->nodes, span_lit( name ) );

    if ( goal )
        create_jobs( s, goal );
    else
        error( "goal %s not defined", name );
}

void state_init( state_t *s )
{
    s->srcdir = getcwd( 0, 0 );

    cb_init( &s->env );
    cb_init( &s->nodes );
    cb_init( &s->jobs );

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

    s->outdir = "build";
    s->failed_count = 0;
    s->skipped_count = 0;
    s->ok_count = 0;
    s->running_count = 0;
    s->queued_count = 0;
    s->todo_count = 0;
    s->running_max = 4;

    s->job_next = NULL;
    s->job_last = NULL;

    for ( int i = 0; i < MAX_FD; ++i )
        s->running[ i ] = 0;
}

void state_setup_outputs( state_t *s )
{
    mkdir( s->outdir, 0777 ); /* ignore errors */
    s->outdir_fd = open( s->outdir, O_DIRECTORY | O_CLOEXEC );

    if ( s->outdir_fd < 0 )
        sys_error( "opening the output directory '%s'", s->outdir );

    if ( flock( s->outdir_fd, LOCK_EX | LOCK_NB ) == -1 )
        sys_error( "locking the output directory '%s'", s->outdir );

    int debug_fd = openat( s->outdir_fd, "debug", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666 );

    if ( debug_fd < 0 )
        sys_error( "opening %s/debug for writing", s->outdir );
    else
        s->debug = fdopen( debug_fd, "w" );
}

void state_load( state_t *s )
{
    var_t *jobs, *outdir;

    load_rules( &s->nodes, &s->env, "gib.file" );

    if ( ( outdir = env_get( &s->env, span_lit( "outdir" ) ) ) && outdir->list )
        s->outdir = outdir->list->data;

    state_setup_outputs( s );

    if ( ( jobs = env_get( &s->env, span_lit( "jobs" ) ) ) && jobs->list )
        s->running_max = atoi( jobs->list->data );

    load_dynamic( &s->nodes, s->outdir_fd, "gib.dynamic" );
    load_stamps( &s->nodes, s->outdir_fd, "gib.stamps" );
}

void state_save( state_t *s )
{
    write_stamps( &s->nodes, s->outdir_fd, "gib.stamps" );
    save_dynamic( &s->nodes, s->outdir_fd, "gib.dynamic" );
}

void state_destroy( state_t *s )
{
    /* … */
}

void monitor( state_t *s )
{
    time_t started = time( NULL );
    time_t elapsed = 0;

    signal( SIGHUP, sighandler );
    signal( SIGINT, sighandler );
    signal( SIGTERM, sighandler );

    signal( SIGPIPE, sighandler ); // ??
    signal( SIGALRM, sighandler ); // ??

    while ( main_loop( s ) )
    {
        elapsed = time( NULL ) - started;
        fprintf( stderr, "%d/%d running + %d/%d queued | %d ok + %d failed | %lld:%02lld elapsed\r",
                 s->running_count, s->running_max, s->queued_count, s->todo_count - s->running_count,
                 s->ok_count, s->failed_count, elapsed / 60, elapsed % 60 );
    }

    elapsed = time( NULL ) - started;
    fprintf( stderr, "build finished: %d ok, %d failed, %d skipped, %lld:%02lld elapsed\n",
             s->ok_count, s->failed_count, s->skipped_count, elapsed / 60, elapsed % 60 );
}

void selector_init( selector_t *sel, match_op_t op, match_type_t type, const char *arg )
{
    sel->op = op;
    sel->type = type;
    sel->string = span_lit( arg );
}

bool process_option( state_t *s, int ch, const char *arg )
{
    selector_t *new = s->select_tail;

    switch ( ch )
    {
        /* exact matches (goal or variable name): 'm'atch, 'a'nd, 's'kip */
        case 'm': selector_init( new, match_op_intersect, match_exact,  arg ); break;
        case 'a': selector_init( new, match_op_union,     match_exact,  arg ); break;
        case 's': selector_init( new, match_op_subtract,  match_exact,  arg ); break;

        /* add/remove goals by name substring */
        case 'M': selector_init( new, match_op_intersect, match_substr, arg ); break;
        case 'A': selector_init( new, match_op_union,     match_substr, arg ); break;
        case 'S': selector_init( new, match_op_subtract,  match_substr, arg ); break;

        default: return false;
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

    while ( ( ch = getopt( argc, argv, "m:a:s:M:A:S:" ) ) != -1 )
        if ( !process_option( s, ch, optarg ) )
            usage(), error( "unknown option -%c", ch );

    for ( int i = optind; i < argc; ++i )
        process_option( s, 'a', argv[ i ] ); /* union anything without a switch */

    if ( s->select_head == s->select_tail )
        if ( optind == argc )
            process_option( s, 'a', "all" );
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
                cb_insert( &sel->matched, node, VSIZE( node, name ), -1 );
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
            cb_insert( &s->goals, node, VSIZE( node, name ), -1 );
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
                cb_insert( &target, node, VSIZE( node, name ), -1 );
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
        create_jobs( &s, cb_get( &i ) );

    for ( cb_iterator i = cb_begin( &s.nodes ); !cb_end( &i ); cb_next( &i ) )
        node_cleanup( &s, cb_get( &i ) );

    graph_dump( s.debug, &s.nodes );
    monitor( &s );
    state_save( &s );

    return s.failed_count > 0;
}
