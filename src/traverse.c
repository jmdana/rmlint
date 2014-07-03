/**
 *  This file is part of rmlint.
 *
 *  rmlint is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  rmlint is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with rmlint.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *
 *  - Christopher <sahib> Pahl 2010-2014 (https://github.com/sahib)
 *  - Daniel <SeeSpotRun> T.   2014-2014 (https://github.com/SeeSpotRun)
 *
 * Hosted on http://github.com/sahib/rmlint
 *
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include "list.h"
#include "rmlint.h"
#include "filter.h"
#include "linttests.h"

static int const MAX_EMPTYDIR_DEPTH = 100;

static int process_file (RmSession *session, FTSENT *ent, bool is_ppath, int pnum, RmLintType file_type) {
    RmSettings *settings = session->settings;
    RmFileList *list = session->list;

    if (file_type == 0) {
        RmLintType gid_check;

        /*see if we can find a lint type*/
        if ((gid_check = uid_gid_check(ent, session))) {
            file_type = gid_check;
        } else if(is_nonstripped(ent, settings)) {
            file_type = TYPE_NBIN;
        } else if(ent->fts_statp->st_size == 0) {
            file_type = TYPE_EFILE;
        } else {
            guint64 file_size = ent->fts_statp->st_size;
            if(!settings->limits_specified || (settings->minsize <= file_size && file_size <= settings->maxsize)) {
                file_type = TYPE_DUPE_CANDIDATE;
            } else {
                return 0;
            }
        }
    }

    switch (ent->fts_info) {
    case FTS_D:         /* preorder directory */
    case FTS_DC:        /* directory that causes cycles */
    case FTS_DNR:       /* unreadable directory */
    case FTS_DOT:       /* dot or dot-dot */
    case FTS_DP:        /* postorder directory */
    case FTS_ERR:       /* error; errno is set */
    case FTS_INIT:      /* initialized only */
    case FTS_SLNONE:    /* symbolic link without target */
    case FTS_W:         /* whiteout object */
    case FTS_NS:        /* stat(2) failed */
    case FTS_NSOK:      /* no stat(2) requested */
        rm_file_list_append(list, rm_file_new(ent->fts_path, ent->fts_statp, file_type, is_ppath, pnum));
        break;
    case FTS_F:         /* regular file */
    case FTS_SL:        /* symbolic link */
    case FTS_DEFAULT:   /* none of the above */
        rm_file_list_append(list, rm_file_new(ent->fts_path, ent->fts_statp, file_type, is_ppath, pnum));
        break;
    default:
        break;
    } /* end switch(p->fts_info)*/

    return 1;
}

/* Traverse the file hierarchies named in PATHS, the last entry of which
 * is NULL.  FTS_FLAGS controls how fts works.
 * Return true if successful.  */
int traverse_path (RmSession *session, int  pathnum, int fts_flags) {
    RmSettings *settings = session->settings;

    int numfiles = 0;
    char is_ppath = settings->is_ppath[pathnum];
    char *paths[2];

    FTS *ftsp;
    FTSENT *p, *chp;
    if (settings->paths[pathnum]) {
        /* convert into char** structure for passing to fts */
        paths[0] = settings->paths[pathnum];
        paths[1] = NULL;
    } else {
        error("Error: no paths defined for traverse_files");
        numfiles = -1;
        goto cleanup;
    }

    if ((ftsp = fts_open(paths, fts_flags, NULL)) == NULL) {
        error("fts_open failed");
        numfiles = -1;
        goto cleanup;
    }

    /* Initialize ftsp */
    chp = fts_children(ftsp, 0);
    if (chp == NULL) {
        warning("fts_children: can't initialise");
        numfiles = -1;
        goto cleanup;
    } else {
        char is_emptydir[MAX_EMPTYDIR_DEPTH];
        bool have_open_emptydirs = false;
        bool clear_emptydir_flags = false;
        memset(&is_emptydir[0], 'N', sizeof(is_emptydir) - 1);
        is_emptydir[sizeof(is_emptydir) - 1] = '\0';

        int emptydir_stack_overflow = 0;
        while (!session->aborted && (p = fts_read(ftsp)) != NULL) {
            switch (p->fts_info) {
            case FTS_D:         /* preorder directory */
                if (
                    (settings->depth != 0 && p->fts_level >= settings->depth) ||
                    /* continuing into folder would exceed maxdepth*/
                    (settings->ignore_hidden && p->fts_level > 0 && p->fts_name[0] == '.')
                ) {
                    fts_set(ftsp, p, FTS_SKIP); /* do not recurse */
                    clear_emptydir_flags = true; /*current dir not empty*/
                } else {
                    is_emptydir[ (p->fts_level + 1) % ( MAX_EMPTYDIR_DEPTH + 1 )] = 'E';
                    have_open_emptydirs = true;
                    /* assume dir is empty until proven otherwise */
                }
                break;
            case FTS_DC:        /* directory that causes cycles */
                warning(RED"Warning: filesystem loop detected at %s (skipping)\n"NCO,
                        p->fts_path);
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_DNR:       /* unreadable directory */
                warning(RED"Warning: cannot read directory %s (skipping)\n"NCO, p->fts_path);
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_DOT:       /* dot or dot-dot */
                break;
            case FTS_DP:        /* postorder directory */
                if ((p->fts_level >= emptydir_stack_overflow) &&
                        (is_emptydir[ (p->fts_level + 1) % ( MAX_EMPTYDIR_DEPTH + 1 )] == 'E')) {
                    numfiles += process_file(session, p, is_ppath, pathnum, TYPE_EDIR);
                }
                break;
            case FTS_ERR:       /* error; errno is set */
                warning(RED"Warning: error %d in fts_read for %s (skipping)\n"NCO, errno, p->fts_path);
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_INIT:      /* initialized only */
                break;
            case FTS_SLNONE:    /* symbolic link without target */
                warning(RED"Warning: symlink without target: %s\n"NCO, p->fts_path);
                numfiles += process_file(session, p, is_ppath, pathnum, TYPE_BLNK);
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_W:         /* whiteout object */
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_NS:        /* stat(2) failed */
                clear_emptydir_flags = true; /*current dir not empty*/
                warning(RED"Warning: cannot stat file %s (skipping)\n", p->fts_path);
                break;
            case FTS_SL:        /* symbolic link */
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            case FTS_NSOK:      /* no stat(2) requested */
            case FTS_F:         /* regular file */
            case FTS_DEFAULT:   /* any file type not explicitly described by one of the above*/
                clear_emptydir_flags = true; /*current dir not empty*/
                numfiles += process_file(session, p, is_ppath, pathnum, 0); /* this is for any of FTS_NSOK, FTS_SL, FTS_F, FTS_DEFAULT*/
            default:
                clear_emptydir_flags = true; /*current dir not empty*/
                break;
            } /* end switch(p->fts_info)*/
            if (clear_emptydir_flags) {
                /* non-empty dir found above; need to clear emptydir flags for all open levels*/
                if (have_open_emptydirs) {
                    memset(&is_emptydir[0], 'N', sizeof(is_emptydir) - 1);
                    have_open_emptydirs = false;
                }
                clear_emptydir_flags = false;
            }

            /*current dir may not be empty; by association, all open dirs are non-empty*/

        } /*end while ((p = fts_read(ftsp)) != NULL)*/
    }
    if (errno != 0) {
        error ("Error '%s': fts_read failed on %s", g_strerror(errno), ftsp->fts_path);
        numfiles = -1;
    }

    fts_close(ftsp);

cleanup:
    return numfiles;
}

/*--------------------------------------------------------------------*/
/* Traverse file hierarchies based on settings contained in SETTINGS;
 * add the files found into LIST
 * Return file count if successful.  */

int rmlint_search_tree(RmSession *session) {
    RmSettings *settings = session->settings;
    int numfiles = 0;
    int cpindex = 0;

    /* Set Bit flags for fts options.  */
    int bit_flags = 0;
    if (!settings->followlinks) {
        bit_flags |= FTS_COMFOLLOW | FTS_PHYSICAL;
    } else {
        bit_flags |= FTS_LOGICAL;
    }

    /* don't follow symlinks except those passed in command line */
    if (settings->samepart) {
        bit_flags |= FTS_XDEV;
    }

    while(settings->paths[cpindex] != NULL) {
        /* The path points to a dir - recurse it! */
        info("Now scanning "YEL"\"%s\""NCO" (%spreferred path)...",
             settings->paths[cpindex],
             settings->is_ppath[cpindex] ? "" : "non-"
            );
        numfiles += traverse_path (session, cpindex, bit_flags);
        info(" done: %d files added.\n", numfiles);

        cpindex++;
    }

    return numfiles;
}