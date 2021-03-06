#include "common.h"
#include "writer.h"
#include "error.h"
#include <sys/stat.h>
#include <dirent.h>

struct todo
{
    struct todo *next;
    struct dirent dt;
};

typedef struct state
{
    bool is_root;
    span_t path;
    int dirfd;
    writer_t *out, *dep;
} state_t;

void dump( state_t s )
{
    DIR *list = fdopendir( s.dirfd );
    struct dirent *dirp;
    struct todo *todo = 0;

    writer_append( s.dep, span_lit( "dep " ) );
    writer_append( s.dep, span_empty( s.path ) ? span_lit( "." ) : s.path );
    writer_append( s.dep, span_lit( "\n" ) );

    if ( !list )
        sys_error( NULL, "fdopendir on %.*s failed", span_len( s.path ), s.path.str );

    while ( ( dirp = readdir( list ) ) )
    {
        int last = strlen( dirp->d_name ) - 1;

        if ( dirp->d_name[ 0 ] == '.' ||
             dirp->d_name[ last ] == '~' ||
             s.is_root && dirp->d_name[ 0 ] == '_' ||
             s.is_root && !strcmp( dirp->d_name, "gibfile" ) ||
             s.is_root && !strcmp( dirp->d_name, "gib" ) )
            continue;

        if ( dirp->d_type == DT_UNKNOWN )
        {
            struct stat st;

            if ( fstatat( s.dirfd, dirp->d_name, &st, 0 ) )
                sys_error( NULL, "fstatat %s", dirp->d_name );

            if ( S_ISREG( st.st_mode ) ) dirp->d_type = DT_REG;
            if ( S_ISDIR( st.st_mode ) ) dirp->d_type = DT_DIR;
        }

        if ( dirp->d_type == DT_DIR )
        {
            struct todo *n = malloc( sizeof( struct todo ) );
            n->next = todo;
            n->dt   = *dirp;
            todo = n;
        }

        if ( dirp->d_type == DT_REG )
        {
            writer_append( s.out, span_lit( "f " ) );
            writer_append( s.out, span_lit( dirp->d_name ) );
            writer_append( s.out, span_lit( "\n" ) );
        }
    }

    writer_append( s.out, span_lit( "\n" ) );

    state_t sub = s;
    sub.is_root = false;

    while ( todo )
    {
        char subpath[ span_len( s.path ) + strlen( todo->dt.d_name ) + 2 ], *spp = subpath;
        spp = span_copy( spp, s.path );
        if ( span_len( s.path ) )
            *spp++ = '/';
        spp = stpcpy( spp, todo->dt.d_name );

        writer_append( s.out, span_lit( "d " ) );
        writer_append( s.out, span_mk( subpath, spp ) );
        writer_append( s.out, span_lit( "\n" ) );

        sub.path   = span_mk( subpath, spp );
        sub.dirfd  = openat( s.dirfd, todo->dt.d_name, O_DIRECTORY | O_RDONLY );

        if ( sub.dirfd < 0 )
            sys_error( NULL, "opening directory %s", todo->dt.d_name );

        dump( sub );

        struct todo *n = todo->next;
        free( todo );
        todo = n;
    }

    closedir( list );
}

int main( int argc, const char *argv[] )
{
    if ( argc != 3 )
        error( "usage: %s <rootdir> <outfile>", argv[ 0 ] );

    writer_t out, dep;

    state_t s =
    {
        .dirfd   = open( argv[ 1 ], O_DIRECTORY | O_RDONLY ),
        .out     = &out,
        .dep     = &dep,
        .is_root = true,
        .path    = span_lit( "" )
    };

    if ( s.dirfd < 0 )
        sys_error( NULL, "opening directory %s", argv[ 1 ] );

    writer_open( &out, AT_FDCWD, argv[ 2 ] );
    dep.file = dep.tmp = NULL;
    dep.ptr = 0;
    dep.fd = 3;

    dump( s );

    writer_close( &out );
    writer_flush( &dep );
}
