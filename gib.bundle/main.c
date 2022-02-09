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

typedef struct
{
    char *srcdir;
    char *outdir;
    int outdir_fd;

    cb_tree env;
    cb_tree nodes;
    cb_tree jobs;
    cb_tree dyn;

    job_t *job_next, *job_last;
    job_t *running[ MAX_FD ];

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
        bool changed = true;

        if ( j->dyn_info )
        {
            dyn_t *di = malloc( VSIZE( di, name ) + strlen( j->name ) + 1 );
            strcpy( di->name, j->name );
            span_t data = di->data = span_mk( j->dyn_info, j->dyn_info + j->dyn_size );
            cb_insert( &s->dyn, di, VSIZE( di, name ), -1 );

            while ( !span_empty( data ) )
            {
                span_t line = fetch_line( &data );

                if ( span_eq( line, "unchanged" ) )
                    changed = false;

                if ( span_eq( line, "warning" ) )
                    j->warned = true;
            }
        }

        n->stamp_updated = n->stamp_want;
        n->cmd_hash = var_hash( n->cmd );

        if ( changed )
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
                if ( job_update( s->running[ fd ] ) )
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

    if ( out->stamp_want > out->stamp_updated )
    {
        for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
        {
            node_t *dep = cb_get( &i );
            node_t *dep_out = dep->type == out_node ? dep : 0;

            if ( dep_out && dep_out->stamp_want > dep_out->stamp_updated && !dep_out->failed )
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

int main( int argc, const char *argv[] )
{
    state_t s;
    s.srcdir = getcwd( 0, 0 );

    cb_init( &s.env );
    cb_init( &s.nodes );
    cb_init( &s.jobs );
    cb_init( &s.dyn );

    struct utsname uts;
    if ( uname( &uts ) < 0 )
        sys_error( "uname" );
    for ( char *s = uts.sysname; *s != 0; ++s )
        if ( isupper( *s ) )
            *s += 32;

    var_t *srcdir = env_set( &s.env, span_lit( "srcdir" ) );
    var_add( srcdir, span_lit( s.srcdir ) );
    var_t *uname = env_set( &s.env, span_lit( "uname" ) );
    var_add( uname, span_lit( uts.sysname ) );

    load_rules( &s.nodes, &s.env, "gib.file" );
    var_t *var_outpath = env_get( &s.env, span_lit( "outpath" ) );
    const char *outpath = var_outpath && var_outpath->list ? var_outpath->list->data : 0;

    if ( asprintf( &s.outdir, "%s%sdefault", outpath ?: s.srcdir, outpath ? "" : "/bin." ) < 0 )
        sys_error( "asprintf" );

    char *path_dyn, *path_stamp, *path_debug;

    if ( asprintf( &path_dyn, "%s/gib.dynamic", s.outdir ) < 0 )
        sys_error( "asprintf" );

    if ( asprintf( &path_stamp, "%s/gib.stamps", s.outdir ) < 0 )
        sys_error( "asprintf" );

    if ( asprintf( &path_debug, "%s/gib.debug", s.outdir ) < 0 )
        sys_error( "asprintf" );

    load_dynamic( &s.nodes, &s.dyn, path_dyn );
    load_stamps( &s.nodes, path_stamp );

    s.job_next = 0;
    s.job_last = 0;

    for ( int i = 0; i < MAX_FD; ++i )
        s.running[ i ] = 0;

    signal( SIGHUP, sighandler );
    signal( SIGINT, sighandler );
    signal( SIGTERM, sighandler );

    signal( SIGPIPE, sighandler ); // ??
    signal( SIGALRM, sighandler ); // ??

    var_t *jobs = env_get( &s.env, span_lit( "jobs" ) );
    s.failed_count = 0;
    s.skipped_count = 0;
    s.ok_count = 0;
    s.running_count = 0;
    s.queued_count = 0;
    s.todo_count = 0;
    s.running_max = jobs && jobs->list ? atoi( jobs->list->data ) : 4;

    mkdir( s.outdir, 0777 ); /* ignore errors */
    s.outdir_fd = open( s.outdir, O_DIRECTORY | O_CLOEXEC );
    if ( s.outdir_fd < 0 )
        sys_error( "opening output directory %s", s.outdir );

    s.debug = fopen( path_debug, "w" );
    graph_dump( s.debug, &s.nodes );

    for ( int i = 1; i < argc; ++ i )
        set_goal( &s, argv[ i ] );

    if ( argc == 1 )
        set_goal( &s, "all" );

    for ( cb_iterator i = cb_begin( &s.nodes ); !cb_end( &i ); cb_next( &i ) )
        node_cleanup( &s, cb_get( &i ) );

    time_t started = time( NULL );
    time_t elapsed = 0;

    while ( main_loop( &s ) )
    {
        elapsed = time( NULL ) - started;
        fprintf( stderr, "%d/%d running + %d/%d queued | %d ok + %d failed | %lld:%02lld elapsed\r",
                 s.running_count, s.running_max, s.queued_count, s.todo_count - s.running_count,
                 s.ok_count, s.failed_count, elapsed / 60, elapsed % 60 );
    }

    elapsed = time( NULL ) - started;
    fprintf( stderr, "build finished: %d ok, %d failed, %d skipped, %lld:%02lld elapsed\n",
             s.ok_count, s.failed_count, s.skipped_count, elapsed / 60, elapsed % 60 );

    write_stamps( &s.nodes, path_stamp );
    save_dynamic( &s.dyn, path_dyn );

    return s.failed_count > 0;
}
