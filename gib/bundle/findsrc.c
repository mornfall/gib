#include "common.h"
#include "writer.h"
#include <sys/stat.h>
#include <dirent.h>

struct todo
{
    struct todo *next;
    struct dirent dt;
};

void dump( int dirfd, writer_t *w, const char *path, bool is_root )
{
    DIR *list = fdopendir( dirfd );
    struct dirent *dirp;
    struct todo *todo = 0;

    if ( !list )
        sys_error( "fdopendir on %s failed", path );

    while ( ( dirp = readdir( list ) ) )
    {
        if ( dirp->d_name[ 0 ] == '.' || dirp->d_name[ 0 ] == '_' )
            continue;

        if ( is_root && ( !strncmp( dirp->d_name, "bin.", 4 ) ||
                          !strncmp( dirp->d_name, "gib.", 4 ) ) )
            continue;

        if ( dirp->d_type == DT_UNKNOWN )
        {
            struct stat st;

            if ( fstatat( dirfd, dirp->d_name, &st, 0 ) )
                sys_error( "fstatat %s", dirp->d_name );

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
            writer_append( w, span_lit( "f " ) );
            writer_append( w, span_lit( dirp->d_name ) );
            writer_append( w, span_lit( "\n" ) );
        }
    }

    writer_append( w, span_lit( "\n" ) );

    while ( todo )
    {
        char subpath[ strlen( path ) + strlen( todo->dt.d_name ) + 2 ], *spp = subpath;
        spp = stpcpy( spp, path );
        if ( strlen( path ) )
            *spp++ = '/';
        spp = stpcpy( spp, todo->dt.d_name );

        writer_append( w, span_lit( "d " ) );
        writer_append( w, span_mk( subpath, spp ) );
        writer_append( w, span_lit( "\n" ) );

        int subfd = openat( dirfd, todo->dt.d_name, O_DIRECTORY | O_RDONLY );
        if ( subfd < 0 )
            sys_error( "opening directory %s", todo->dt.d_name );
        dump( subfd, w, subpath, false );

        struct todo *n = todo->next;
        free( todo );
        todo = n;
    }

    closedir( list );
}

int main( int argc, const char *argv[] )
{
    if ( argc < 2 )
        return 1;

    writer_t out;
    int rootfd = open( argv[ 1 ], O_DIRECTORY | O_RDONLY );

    if ( rootfd < 0 )
        sys_error( "opening directory %s", argv[ 1 ] );

    writer_open( &out, AT_FDCWD, argv[ 2 ] );
    dump( rootfd, &out, "", true );
    writer_close( &out );
}
