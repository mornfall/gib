#include "outdb.h"
#include "rules.h"
#include "graph.h"
#include "job.h"
#include <sys/wait.h>
#include <signal.h>
#define MAX_FD 64

static sig_atomic_t _signalled = 0;

void sighandler( int sig )
{
    _signalled = sig;
    signal( sig, SIG_DFL );
}

typedef struct
{
    char *srcdir;
    char *outdir;
    int outdir_fd;

    cb_tree env;
    cb_tree nodes;
    cb_tree jobs;

    job_t *job_next, *job_last;
    job_t *running[ MAX_FD ];

    int running_count;
    int running_max;
} state_t;

void job_queue( state_t *s, job_t *j )
{
    assert( !j->queued );

    fprintf( s->debug, "queue: %s [%llx → %llx]\n", j->name, j->node->stamp, j->node->new_stamp );
    j->queued = true;

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

    assert( j->pipe_fd < MAX_FD );
    s->running[ j->pipe_fd ] = j;

    return true;
}

void create_jobs( state_t *s, node_t *goal );

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
        fprintf( stderr, "%-*sok\n", 75, j->name );
        n->stamp = n->new_stamp;
    }
    else
    {
        fprintf( stderr, "%-*sno\n", 75, j->name );
        n->failed = true;
    }

    s->running_count --;

    for ( cb_iterator i = cb_begin( &j->node->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );

        if ( n->changed )
            b->dirty = true;

        if ( n->failed && !b->failed )
            skip( s, b );

        if ( -- b->waiting )
            continue;

        if ( b->dirty && b->stamp != b->new_stamp && !b->failed )
            job_queue( s, job_add( &s->jobs, b ) );
    }
}

void teardown( state_t *s )
{
    fprintf( stderr, "[caught signal %d, cleaning up]                \n", _signalled );
    fflush( stderr );

    job_t *j = NULL;

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( ( j = s->running[ fd ] ) )
            kill( j->pid, SIGTERM );

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( s->running[ fd ] )
            job_cleanup( s, fd );
}

void main_loop( state_t *s )
{
    while ( true )
    {
        while ( s->running_count < s->running_max )
            if ( !job_start( s ) )
                break;

        fprintf( stderr, "running: %d/%d\r", s->running_count, s->running_max );

        if ( _signalled || ( !s->running_count && !s->job_next ) )
            return;

        fd_set ready;
        FD_ZERO( &ready );

        for ( int i = 0; i < MAX_FD; ++i )
            if ( s->running[ i ] )
                FD_SET( i, &ready );

        if ( select( MAX_FD, &ready, 0, &ready, 0 ) == -1 )
            sys_error( "select" );

        for ( int fd = 0; fd < MAX_FD; ++ fd )
            if ( FD_ISSET( fd, &ready ) )
                if ( job_update( s->running[ fd ] ) )
                    job_cleanup( s, fd );

        if ( !_signalled )
            while ( s->running_count < s->running_max )
                if ( !job_start( s ) )
                    break;
    }
}

void load_graph( state_t *s )
{
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

        if ( out && out->new_stamp < dep->new_stamp )
            out->new_stamp = dep->new_stamp;
    }

    if ( out && out->stamp != out->new_stamp )
    {
        for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
        {
            node_t *dep = cb_get( &i );
            node_t *dep_out = dep->type == out_node ? dep : 0;

            if ( dep_out && dep_out->stamp != dep_out->new_stamp && !dep_out->failed )
            {
                cb_insert( &dep->blocking, goal, VSIZE( goal, name ), -1 );
                goal->waiting ++;
            }
        }

        if ( !out->waiting ) /* can run right away */
            job_queue( s, job_add( &s->jobs, out ) );
    }
}

int main( int argc, const char *argv[] )
{
    state_t s;
    s.srcdir = getcwd( 0, 0 );

    cb_init( &s.env );
    cb_init( &s.nodes );
    cb_init( &s.jobs );

    var_t *srcdir = env_set( &s.env, span_lit( "srcdir" ) );
    var_add( srcdir, span_lit( s.srcdir ) );

    load_rules( &s.nodes, &s.env, "gib.file" );
    var_t *var_outpath = env_get( &s.env, span_lit( "outpath" ) );
    const char *outpath = var_outpath && var_outpath->list ? var_outpath->list->data : 0;

    if ( asprintf( &s.outdir, "%s%sdefault", outpath ?: s.srcdir, outpath ? "" : "/bin." ) < 0 )
        sys_error( "asprintf" );

    char *path_dyn, *path_stamp;

    if ( asprintf( &path_dyn, "%s/dynamic.gib", s.outdir ) < 0 )
        sys_error( "asprintf" );

    if ( asprintf( &path_stamp, "%s/stamps.gib", s.outdir ) < 0 )
        sys_error( "asprintf" );

    load_dynamic( &s.nodes, path_dyn );
    load_stamps( &s.nodes, path_stamp );

    graph_dump( &s.nodes );

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
    node_t *all = graph_get( &s.nodes, span_lit( "all" ) );

    if ( !all )
        fprintf( stderr, "goal all does not exist\n" ), exit( 1 );

    s.running_count = 0;
    s.running_max = jobs && jobs->list ? atoi( jobs->list->data ) : 4;

    create_jobs( &s, all ); /* TODO */

    mkdir( s.outdir, 0777 ); /* ignore errors */
    s.outdir_fd = open( s.outdir, O_DIRECTORY | O_CLOEXEC );
    if ( s.outdir_fd < 0 )
        sys_error( "opening output directory %s", s.outdir );

    main_loop( &s );

    write_stamps( &s.nodes, path_stamp );
    // write_dynamic( &s.nodes );

    return 0;
}
