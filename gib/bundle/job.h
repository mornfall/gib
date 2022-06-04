#include "common.h"
#include "env.h"
#include <sys/socket.h>

typedef struct job
{
    node_t *node;
    pid_t pid;
    bool queued:1;
    bool warned:1;
    bool changed:1;
    int pipe_fd;
    reader_t *reader;
    struct job *next;
    char name[];
} job_t;

void job_exec( job_t *j, int outdir_fd, int logdir_fd, int childfd )
{
    int argv_size = 16, i = 0;
    char **argv = malloc( argv_size * sizeof( char * ) );

    for ( value_t *n = j->node->cmd; n; n = n->next )
    {
        if ( i == argv_size - 1 )
            if ( !( argv = realloc( argv, sizeof( char * ) * ( argv_size += argv_size ) ) ) )
                sys_error( NULL, "realloc" );

        argv[ i++ ] = n->data;
    }

    argv[ i ] = 0;

    fchdir( outdir_fd );
    char logname[ strlen( j->name ) + 5 ], *p = logname;

    for ( char *i = j->name; *i; ++p, ++i )
        *p = *i == '/' || *i == ' ' ? '_' : *i;
    p = stpcpy( p, ".txt" );

    unlinkat( logdir_fd, logname, 0 );
    int logfd = openat( logdir_fd, logname, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0666 );
    int nullfd = open( "/dev/null", O_RDONLY | O_CLOEXEC );

    if ( logfd < 0 )
        sys_error( NULL, "opening logfile %s", logname );

    if ( nullfd < 0 )
        sys_error( NULL, "opening /dev/null" );

    dup2( nullfd, 0 );
    dup2( logfd, 1 );
    dup2( logfd, 2 );
    dup2( childfd, 3 );

    fprintf( stderr, "gib# out %s\n", j->node->name );
    for ( int i = 0; argv[ i ]; ++i )
        fprintf( stderr, "gib# %s%s\n", i ? "    " : "cmd ", argv[ i ] );

    execv( argv[ 0 ], argv );
    sys_error( NULL, "execv %s (job %s):", argv[ 0 ], j->name );
}

void job_fork( job_t *j, int outdir_fd, int logdir_fd )
{
    int fds[ 2 ];

    if ( socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds ) )
        sys_error( NULL, "socketpair %s", j->name );

    j->pipe_fd = fds[ 0 ];
    j->pid = fork();

    if ( j->pid == 0 ) /* child */
        job_exec( j, outdir_fd, logdir_fd, fds[ 1 ] );

    close( fds[ 1 ] );

    if ( j->pid == -1 )
        sys_error( NULL, "fork %s [%s]:", j->name, j->node->cmd->data );
}

job_t *job_find( cb_tree *jobs, node_t *node )
{
    job_t *j = cb_find( jobs, span_lit( node->name ) ).leaf;
    if ( j && !strcmp( j->name, node->name ) )
        return j;
    else
        return 0;
}

job_t *job_add( cb_tree *jobs, node_t *build )
{
    span_t name = span_lit( build->name );
    job_t *j = job_find( jobs, build );

    if ( !j )
    {
        j = calloc( 1, SIZE_NAMED( job_t, span_len( name ) ) );
        span_copy( j->name, name );
        j->node = build;
        j->changed = true;
        cb_insert( jobs, j, offsetof( job_t, name ), -1 );
    }

    return j;
}

span_t path_normalize( span_t in, char *buffer )
{
    span_t out = span_mk( buffer, buffer );

    while ( !span_empty( in ) )
    {
        span_t comp = fetch_until( &in, "/", 0 );

        if ( span_eq( comp, ".." ) )
        {
            if ( !span_empty( out ) ) -- out.end;
            while ( out.end > out.str && *--out.end != '/' );
        }
        else
            out.end = span_copy( ( char * ) out.end, comp );

        if ( !span_empty( in ) )
            *( char * )out.end++ = '/';

        if ( span_empty( out ) )
            break;
    }

    return out;
}

span_t job_normalize_dep( span_t path, const char *srcdir, char *buffer )
{
    /* assume no symlink shenanigans in the source tree */
    span_t relative = path;

    if ( span_starts_with( path, srcdir ) )
    {
        relative.str += strlen( srcdir ) + 1;
        relative = path_normalize( relative, buffer );
    }

    return span_empty( relative ) ? path : relative;
}

bool job_update( job_t *j, cb_tree *nodes, const char *srcdir )
{
    if ( !j->reader )
    {
        j->reader = malloc( sizeof( reader_t ) );
        reader_init( j->reader, -1, NULL );
        fcntl( j->pipe_fd, F_SETFD, O_NONBLOCK );
        j->reader->fd = j->pipe_fd;
    }

    while ( read_line( j->reader ) )
    {
        span_t arg = j->reader->span;
        span_t cmd = fetch_word( &arg );

        if ( span_eq( cmd, "dep" ) )
        {
            char buffer[ span_len( arg ) ];
            span_t target = job_normalize_dep( arg, srcdir, buffer );
            graph_add_dep( nodes, j->node, target, true );
        }

        if ( span_eq( cmd, "unchanged" ) )
            j->changed = false;
        if ( span_eq( cmd, "warning" ) )
            j->warned = true;
    }

    return j->reader->fd < 0;
}
