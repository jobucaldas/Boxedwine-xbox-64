/* Debian Wine temporary-directory.patch; Michael Gilbert; LGPL-2.1-or-later. */
#ifndef __WINE_WINE_SERVER_TMPDIR_H
#define __WINE_WINE_SERVER_TMPDIR_H

#include <dirent.h>

/***********************************************************************
 *           server_tmpdir
 */
static inline char* server_tmpdir( const char* cfg_dir )
{
    char *tmp_dir = NULL;
    char *run_dir = NULL;
    const char *tmp_prefix = "/tmp";
    const char *run_prefix = "/run/user";

    /* use $run_prefix/$uid as wineserver's tmpdir if possible */
    if (asprintf( &run_dir, "%s/%u", run_prefix, getuid() ) < 0)
    {
        fatal_error("error building run_dir\n");
    }
    if (opendir( run_dir ))
    {
        if (asprintf( &tmp_dir, "%s/wine", run_dir ) < 0)
        {
            fatal_error("error building temporary directory in /run/user\n");
        }
    }
    free( run_dir );

    /* otherwise, use a randomly named directory under TMPDIR */
    if (!tmp_dir)
    {
        FILE *stream;
        char *root_dir = NULL;
        char *wineserver_file = NULL;
        mode_t mode = S_IRUSR;

        /* determine the temporary directory */
        root_dir = secure_getenv("TMPDIR");
        if (!root_dir)
        {
            if (asprintf( &root_dir, "%s", tmp_prefix ) < 0)
            {
                fatal_error("error building root_dir\n");
            }
        }

        /* remove existing wineserver tmpdir file if permissions are wrong */
        if (asprintf( &wineserver_file, "%s/wineserver", cfg_dir ) < 0)
        {
            fatal_error("error building wineserver_file\n");
        }
        if (access( wineserver_file, F_OK ) != -1)
        {
            struct stat statbuf;
            mode_t mode_mask = S_IRWXU | S_IRWXG | S_IRWXO;
            if (stat( wineserver_file, &statbuf ) != 0)
            {
                fatal_error("error reading wineserver tmpdir file permissions\n");
            }
            if ((statbuf.st_mode & mode_mask) != mode)
            {
                if (remove( wineserver_file ) != 0)
                {
                    fatal_error("error removing wineserver tmpdir file\n");
                }
            }
        }

        /* create or read the name of the directory */
        stream = fopen( wineserver_file, "r" );
        if (!stream)
        {
            int fd;
            char *tmp_wine = NULL;

            /* create a new randomized name for the directory */
            if (asprintf( &tmp_wine, "%s/wine-XXXXXX", root_dir ) < 0)
            {
                fatal_error("error building tmp_wine/n");
            }
            tmp_dir = mkdtemp( tmp_wine );
            if (!tmp_dir)
            {
                fatal_error("unable to create wineserver tmpdir\n");
            }
            if (tmp_dir != tmp_wine)
            {
                fatal_error("this condition is not handled\n");
            }

            /* save location of the chosen temporary directory */
            fd = creat( wineserver_file, mode );
            if (fd < 0)
            {
                fatal_error("error opening file descriptor for wineserver tmpdir file\n");
            }
            stream = fdopen( fd, "w" );
            if (!stream)
            {
                fatal_error("error opening wineserver tmpdir file\n");
            }
            if (fputs( tmp_dir + strlen(root_dir) + 1, stream ) < 0)
            {
                fatal_error("error writing to wineserver tmpdir file\n");
            }
            if (fclose( stream ) != 0)
            {
                fatal_error("error closing wineserver tmpdir file\n");
            }
        }
        else
        {
            char *tmp_wineserver = NULL;
            size_t length = 0;
            ssize_t nread;

            /* try to read a previously generated wineserver tmpdir */
            nread = getline( &tmp_wineserver, &length, stream );
            if (fclose( stream ) != 0)
            {
                fatal_error("error closing wineserver tmpdir file\n");
            }

            /* if something went wrong, remove the broken file */
            if (nread < 0)
            {
                remove( wineserver_file );
                fatal_error("fixed a temporary directory error, please try running the same command again\n");
            }

            /* build the full path to the temporary directory */
            if (asprintf( &tmp_dir, "%s/%s", root_dir, tmp_wineserver ) < 0)
            {
                fatal_error("error building temporary directory in /tmp\n");
            }
            free( tmp_wineserver );
        }
        free( root_dir );
        free( wineserver_file );
    }

    return tmp_dir;
}

#endif
