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

void log_argv( int fd, char **argv )
{
    int size = 512, ptr = 0;
    char *buf = malloc( size );

    for ( int i = 0; argv[ i ]; ++i )
    {
        if ( ptr + strlen( argv[ i ] ) + 8 >= size )
            if ( !( buf = realloc( buf, size += size + strlen( argv[ i ] ) + 8 ) ) )
                sys_error( "realloc" );

        ptr += sprintf( buf + ptr, "%s%s\n", i ? "      " : "execv ", argv[ i ] );
    }

    write( fd, buf, ptr );
    free( buf );
}

void job_exec( job_t *j, int dirfd, int childfd )
{
    const char *log_dir = "_log";
    int argv_size = 16, i = 0;
    char **argv = malloc( argv_size * sizeof( char * ) );

    for ( value_t *n = j->node->cmd; n; n = n->next )
    {
        if ( i == argv_size - 1 )
            if ( !( argv = realloc( argv, sizeof( char * ) * ( argv_size += argv_size ) ) ) )
                sys_error( "realloc" );

        argv[ i++ ] = n->data;
    }

    argv[ i ] = 0;

    fchdir( dirfd );
    mkdir( log_dir, 0777 );

    char *path;

    if ( asprintf( &path, "%s/%s.txt", log_dir, j->name ) == -1 )
        sys_error( "asprintf" );

    for ( char *c = path + strlen( log_dir ) + 1; *c; ++c )
        if ( *c == '/' || *c == ' ' )
            *c = '_';

    unlink( path );
    int logfd = open( path, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0666 );
    int nullfd = open( "/dev/null", O_RDONLY | O_CLOEXEC );

    if ( logfd < 0 )
        sys_error( "opening logfile %s", path );

    if ( nullfd < 0 )
        sys_error( "opening /dev/null" );

    dup2( nullfd, 0 );
    dup2( logfd, 1 );
    dup2( logfd, 2 );
    dup2( childfd, 3 );

    log_argv( 2, argv );
    execv( argv[ 0 ], argv );
    sys_error( "execv %s (job %s):", argv[ 0 ], j->name );
}

void job_fork( job_t *j, int dirfd )
{
    int fds[ 2 ];

    if ( socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds ) )
        sys_error( "socketpair %s", j->name );

    j->pipe_fd = fds[ 0 ];
    j->pid = fork();

    if ( j->pid == 0 ) /* child */
        job_exec( j, dirfd, fds[ 1 ] );

    close( fds[ 1 ] );

    if ( j->pid == -1 )
        sys_error( "fork %s [%s]:", j->name, j->node->cmd->data );
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
        j = calloc( 1, VSIZE( j, name ) + span_len( name ) + 1 );
        span_copy( j->name, name );
        j->node = build;
        j->changed = true;
        cb_insert( jobs, j, VSIZE( j, name ), -1 );
    }

    return j;
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
            if ( span_starts_with( arg, srcdir ) )
                arg.str += strlen( srcdir ) + 1;

            graph_add_dep( nodes, j->node, arg, true );
        }

        if ( span_eq( cmd, "unchanged" ) )
            j->changed = false;
        if ( span_eq( cmd, "warning" ) )
            j->warned = true;
    }

    return j->reader->fd < 0;
}
