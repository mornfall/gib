#pragma once
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
    const char *srcdir;

    int outdir_fd,
        logdir_fd,
        faildir_fd;
    time_t started;

    cb_tree *nodes;
    cb_tree jobs;

    job_t *job_next, *job_last;
    job_t *running[ MAX_FD ];

    int skipped_count;
    int failed_count;
    int ok_count;
    int todo_count;
    int queued_count;
    int running_count;
    int running_max;
} queue_t;


void queue_set_outdir( queue_t *q, cb_tree *env )
{
    var_t *var = env_get( env, span_lit( "outdir" ) );

    if ( !var )
    {
        var = env_set( env, span_lit( "outdir" ) );
        var_add( var, span_lit( "build" ) );
    }

    var->frozen = true;

    if ( !var->list || var->list->next )
        error( "variable 'outdir' must be a singleton" );

    const char *dir = var->list->data;

    if ( q->outdir_fd >= 0 )
        return;

    mkdir( dir, 0777 ); /* ignore errors */
    q->outdir_fd = open( dir, O_DIRECTORY | O_CLOEXEC );

    if ( q->outdir_fd < 0 )
        sys_error( "opening the output directory '%s'", dir );

    if ( flock( q->outdir_fd, LOCK_EX | LOCK_NB ) == -1 )
        sys_error( "locking the output directory '%s'", dir );

    load_dynamic( q->nodes, q->outdir_fd, "gib.dynamic" );
    load_stamps( q->nodes, q->outdir_fd, "gib.stamps" );
}

void queue_show_result( queue_t *q, node_t *n, job_t *j )
{
    const char *status = "??";
    int color = 0;
    bool changed = n->stamp_changed == n->stamp_want;
    bool updated = n->stamp_updated == n->stamp_want;

    if      ( n->failed )      status = "no", color = 31, q->failed_count ++;
    else if ( !changed )       status = "--", color = 33, q->skipped_count ++;
    else if ( j && j->warned ) status = "ok", color = 33, q->ok_count ++;
    else                       status = "ok", color = 32, q->ok_count ++;

    if ( !_signalled && n->failed || changed && j && j->warned )
    {
        fprintf( stderr, "\033[J" );
        char filename[ strlen( n->name ) + 5 ], *p = filename;
        for ( char *i = n->name; *i; ++p, ++i )
            *p = ( *i == ' ' || *i == '/' ) ? '_' : *i;
        strcpy( p, ".txt" );

        linkat( s->logdir_fd, filename, s->faildir_fd, filename, 0 );

        reader_t log;

        if ( !reader_init( &log, s->outdir_fd, path ) )
            sys_error( "opening logfile %s", path );

        while ( read_line( &log ) )
            fprintf( stderr, " │ %.*s\n", span_len( log.span ), log.span.str );

        close( log.fd );
    }

    q->todo_count --;
    fprintf( stderr, "\033[J\033[%dm%s\033[0m %s\n", color, status, n->name );
}

void queue_add( queue_t *q, job_t *j )
{
    assert( !j->queued );
    assert( j->node->dirty );

    j->queued = true;
    ++ q->queued_count;

    if ( !q->job_next )
        q->job_next = j;

    if ( q->job_last )
        q->job_last->next = j;

    q->job_last = j;
}

bool queue_start_next( queue_t *q )
{
    if ( !q->job_next )
        return false;

    job_t *j = q->job_next;
    q->job_next = j->next;

    cb_clear( &j->node->deps_dyn );
    job_fork( j, s->outdir_fd );

    s->running_count ++;
    s->queued_count --;

    assert( j->pipe_fd < MAX_FD );
    q->running[ j->pipe_fd ] = j;

    return true;
}

void queue_skip( queue_t *q, node_t *n )
{
    if ( n->failed )
        return;

    queue_show_result( q, n, NULL );
    n->failed = true;

    for ( cb_iterator i = cb_begin( &n->blocking ); !cb_end( &i ); cb_next( &i ) )
        queue_skip( q, cb_get( &i ) );
}

void queue_cleanup_node( queue_t *q, node_t *n )
{
    if ( n->type != out_node && n->type != meta_node || n->dirty || n->waiting )
        return;

    for ( cb_iterator i = cb_begin( &n->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );

        if ( ! -- b->waiting )
        {
            if ( b->dirty )
                queue_add( q, job_add( &q->jobs, b ) );
            else
            {
                queue_cleanup_node( q, b );
                q->todo_count --;
            }
        }
    }

    cb_clear( &n->blocking );
}

void queue_cleanup_job( queue_t *q, int fd )
{
    job_t *j = q->running[ fd ];
    q->running[ fd ] = 0;
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

    q->running_count --;
    queue_show_result( q, n, j );

    for ( cb_iterator i = cb_begin( &j->node->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );

        if ( n->stamp_changed > b->stamp_updated )
            b->dirty = true;

        if ( n->failed && !b->failed )
            queue_skip( q, b );

        if ( -- b->waiting )
            continue;

        if ( b->dirty && b->stamp_updated < b->stamp_want && !b->failed )
            queue_add( q, job_add( &q->jobs, b ) );
        else
            queue_cleanup_node( q, b );
    }
}

void queue_teardown( queue_t *q )
{
    fprintf( stderr, "\033[J\r[caught signal %d, cleaning up]\n", _signalled );
    fflush( stderr );

    job_t *j = NULL;

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( ( j = q->running[ fd ] ) )
            kill( j->pid, SIGTERM );

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( q->running[ fd ] )
            queue_cleanup_job( q, fd );
}

bool queue_loop( queue_t *q )
{
    fd_set ready, except;
    FD_ZERO( &ready );
    FD_ZERO( &except );

    if ( q->running_count )
    {
        for ( int i = 0; i < MAX_FD; ++i )
            if ( q->running[ i ] )
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
        queue_teardown( q );

    if ( q->running_count )
        for ( int fd = 0; fd < MAX_FD; ++ fd )
            if ( FD_ISSET( fd, &ready ) || FD_ISSET( fd, &except ) )
                if ( job_update( q->running[ fd ], q->nodes, q->srcdir ) )
                    queue_cleanup_job( q, fd );

    if ( !_signalled )
        while ( q->running_count < q->running_max )
            if ( !queue_start_next( q ) )
                break;

    return !_signalled && ( q->running_count || q->job_next );
}

void queue_create_jobs( queue_t *q, node_t *goal )
{
    if ( goal->visited )
        return;

    goal->visited = true;
    node_t *out = goal->type == out_node ? goal : 0;

    for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *dep = cb_get( &i );
        queue_create_jobs( q, dep );

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
        ++ q->todo_count;

    if ( !out->waiting && out->dirty ) /* can run right away */
        queue_add( q, job_add( &q->jobs, out ) );
}

void queue_add_goal( queue_t *q, const char *name )
{
    node_t *goal = graph_get( q->nodes, span_lit( name ) );

    if ( goal )
        queue_create_jobs( q, goal );
    else
        error( "goal %s not defined", name );
}

void queue_init( queue_t *q, cb_tree *nodes, const char *srcdir )
{
    q->outdir_fd = -1;
    q->srcdir = srcdir;
    q->nodes = nodes;

    cb_init( &q->jobs );

    q->started = time( NULL );
    q->failed_count = 0;
    q->skipped_count = 0;
    q->ok_count = 0;
    q->running_count = 0;
    q->queued_count = 0;
    q->todo_count = 0;
    q->running_max = 4;

    q->job_next = NULL;
    q->job_last = NULL;

    for ( int i = 0; i < MAX_FD; ++i )
        q->running[ i ] = 0;
}

void queue_monitor( queue_t *q, bool endmsg )
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

    while ( queue_loop( q ) )
    {
        elapsed = time( NULL ) - q->started;
        fprintf( stderr, "%d/%d running + %d/%d queued | %d ok + %d failed | %lld:%02lld elapsed\r",
                 q->running_count, q->running_max, q->queued_count, q->todo_count - q->running_count,
                 q->ok_count, q->failed_count, elapsed / 60, elapsed % 60 );
    }

    elapsed = time( NULL ) - q->started;

    if ( endmsg )
        fprintf( stderr, "build finished: %d ok, %d failed, %d skipped, %lld:%02lld elapsed\n",
                 q->ok_count, q->failed_count, q->skipped_count, elapsed / 60, elapsed % 60 );
}
