/*
  This file is part of the Astrometry.net suite.
  Copyright 2007-2008 Dustin Lang, Keir Mierle and Sam Roweis.

  The Astrometry.net suite is free software; you can redistribute
  it and/or modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation, version 2.

  The Astrometry.net suite is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the Astrometry.net suite ; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/


/**
   A command-line interface to the blind solver system.

TODO:

(2) It assumes you have netpbm tools installed which the main build
doesn't require.

> I think it will only complain if it needs one of the netpbm programs to do
> its work - and it cannot do anything sensible (except print a friendly
> error message) if they don't exist.

(6)  by default, we do not produce an entirely new fits file but this can
be turned on

(7) * by default, we output to stdout a single line for each file something like:
myimage.png: unsolved using X field objects
or
myimage.png: solved using X field objects, RA=rr,DEC=dd, size=AxB
pixels=UxV arcmin

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>
#include <errors.h>
#include <getopt.h>

#include "an-bool.h"
#include "bl.h"
#include "ioutils.h"
#include "xylist.h"
#include "matchfile.h"
#include "scriptutils.h"
#include "fitsioutils.h"
#include "augment-xylist.h"
#include "an-opts.h"
#include "log.h"
#include "errors.h"
#include "sip_qfits.h"
#include "sip-utils.h"
#include "wcs-rd2xy.h"

static an_option_t options[] = {
	{'h', "help",		   no_argument, NULL,
     "print this help message" },
	{'v', "verbose",       no_argument, NULL,
     "be more chatty" },
    {'D', "dir", required_argument, "directory",
     "place all output files in this directory"},
    {'o', "out", required_argument, "base-filename",
     "name the output files with this base name"},
    {'b', "backend-config", required_argument, "filename",
     "use this config file for the \"backend\" program"},
	{'f', "files-on-stdin", no_argument, NULL,
     "read filenames to solve on stdin, one per line"},
	{'p', "no-plots",       no_argument, NULL,
     "don't create any plots of the results"},
    //{"solved-in-dir",  required_argument, 0, 'i'},
    //directory containing input solved files  (-i)\n"
    {'G', "use-wget",       no_argument, NULL,
     "use wget instead of curl"},
  	{'O', "overwrite",      no_argument, NULL,
     "overwrite output files if they already exist"},
    {'K', "continue",       no_argument, NULL,
     "don't overwrite output files if they already exist; continue a previous run"},
    {'J', "skip-solved",    no_argument, NULL,
     "skip input files for which the 'solved' output file already exists;\n"
     "                  NOTE: this assumes single-field input files"},
};

static void print_help(const char* progname, bl* opts) {
	printf("\nUsage:   %s [options]  [<image-file-1> <image-file-2> ...] [<xyls-file-1> <xyls-file-2> ...]\n"
           "\n"
           "You can specify http:// or ftp:// URLs instead of filenames.  The \"wget\" or \"curl\" program will be used to retrieve the URL.\n"
	       "\n", progname);
    printf("Options include:\n");
    opts_print_help(opts, stdout, augment_xylist_print_special_opts, NULL);
    printf("\n\n");
}

static int run_command(const char* cmd, bool* ctrlc) {
	int rtn;
    logverb("Running: %s\n", cmd);
    fflush(NULL);
	rtn = system(cmd);
    fflush(NULL);
	if (rtn == -1) {
		SYSERROR("Failed to run command \"%s\"", cmd);
		return -1;
	}
	if (WIFSIGNALED(rtn)) {
        if (ctrlc && (WTERMSIG(rtn) == SIGTERM))
            *ctrlc = TRUE;
		return -1;
	}
	rtn = WEXITSTATUS(rtn);
	if (rtn)
		ERROR("Command exited with exit status %i", rtn);
	return rtn;
}

static void append_escape(sl* list, const char* fn) {
    sl_append_nocopy(list, shell_escape(fn));
}
static void append_executable(sl* list, const char* fn, const char* me) {
    char* exec = find_executable(fn, me);
    if (!exec) {
        ERROR("Error, couldn't find executable \"%s\"", fn);
        exit(-1);
    }
    sl_append_nocopy(list, shell_escape(exec));
    free(exec);
}

int main(int argc, char** args) {
	int c;
	bool help = FALSE;
	char* outdir = NULL;
	char* cmd;
	int i, j, f;
    int inputnum;
	int rtn;
	sl* backendargs;
	int nbeargs;
	bool fromstdin = FALSE;
	bool overwrite = FALSE;
	bool cont = FALSE;
    bool skip_solved = FALSE;
    bool makeplots = TRUE;
    char* me;
    char* tempdir = "/tmp";
    bool verbose = FALSE;
    char* baseout = NULL;
    char* xcol = NULL;
    char* ycol = NULL;
    char* solvedin = NULL;
    char* solvedindir = NULL;
	bool usecurl = TRUE;
    bl* opts;
    augment_xylist_t theallaxy;
    augment_xylist_t* allaxy = &theallaxy;
    int nmyopts;
    char* removeopts = "ixo\x01";

    errors_print_on_exit(stderr);
    fits_use_error_system();

    me = find_executable(args[0], NULL);

	backendargs = sl_new(16);
	append_executable(backendargs, "backend", me);

	rtn = 0;

    nmyopts = sizeof(options)/sizeof(an_option_t);
    opts = opts_from_array(options, nmyopts, NULL);
    augment_xylist_add_options(opts);

    // remove duplicate short options.
    for (i=0; i<nmyopts; i++) {
        an_option_t* opt1 = bl_access(opts, i);
        for (j=nmyopts; j<bl_size(opts); j++) {
            an_option_t* opt2 = bl_access(opts, j);
            if (opt2->shortopt == opt1->shortopt)
                bl_remove_index(opts, j);
        }
    }

    // remove unwanted augment-xylist options.
    for (i=0; i<strlen(removeopts); i++) {
        for (j=nmyopts; j<bl_size(opts); j++) {
            an_option_t* opt2 = bl_access(opts, j);
            if (opt2->shortopt == removeopts[i])
                bl_remove_index(opts, j);
        }
    }

    augment_xylist_init(allaxy);

	while (1) {
        int res;
		c = opts_getopt(opts, argc, args);
        if (c == -1)
            break;
        switch (c) {
            /*
             case 'i':
             solvedindir = optarg;
             break;
             }
             */
		case 'h':
			help = TRUE;
			break;
        case 'v':
            sl_append(backendargs, "--verbose");
            verbose = TRUE;
            break;
		case 'D':
			outdir = optarg;
			break;
        case 'o':
            baseout = optarg;
            break;
		case 'b':
			sl_append(backendargs, "--config");
			append_escape(backendargs, optarg);
			break;
		case 'f':
			fromstdin = TRUE;
			break;
        case 'O':
            overwrite = TRUE;
            break;
        case 'p':
            makeplots = FALSE;
            break;
        case 'G':
            usecurl = FALSE;
            break;
        case 'K':
            cont = TRUE;
            break;
        case 'J':
            skip_solved = TRUE;
            break;
        default:
            res = augment_xylist_parse_option(c, optarg, allaxy);
            if (res) {
                rtn = -1;
                goto dohelp;
            }
        }
    }

	if (optind == argc) {
		printf("ERROR: You didn't specify any files to process.\n");
		help = TRUE;
	}

	if (help) {
    dohelp:
		print_help(args[0], opts);
		exit(rtn);
	}

	if (outdir) {
        if (mkdir_p(outdir)) {
            ERROR("Failed to create output directory %s", outdir);
            exit(-1);
        }
	}

	// number of backend args not specific to a particular file
	nbeargs = sl_size(backendargs);

	f = optind;
    inputnum = 0;
	while (1) {
		char* infile = NULL;
		bool isxyls;
		char* reason;
		int len;
		char* cpy;
		char* base;
		char *objsfn, *redgreenfn;
		char *ngcfn, *ppmfn=NULL, *indxylsfn;
        char* downloadfn;
        char* suffix = NULL;
		sl* outfiles;
		sl* tempfiles;
		sl* cmdline;
        bool ctrlc;
        augment_xylist_t theaxy;
        augment_xylist_t* axy = &theaxy;
        int j;

        // reset augment-xylist args.
        memcpy(axy, allaxy, sizeof(augment_xylist_t));

		if (fromstdin) {
            char fnbuf[1024];
			if (!fgets(fnbuf, sizeof(fnbuf), stdin)) {
				if (ferror(stdin))
					SYSERROR("Failed to read a filename from stdin");
				break;
			}
			len = strlen(fnbuf);
			if (fnbuf[len-1] == '\n')
				fnbuf[len-1] = '\0';
			infile = fnbuf;
            logmsg("Reading input file \"%s\"...\n", infile);
		} else {
			if (f == argc)
				break;
			infile = args[f];
			f++;
            logmsg("Reading input file %i of %i: \"%s\"...\n",
                   f - optind, argc - optind, infile);
		}
        inputnum++;

        cmdline = sl_new(16);

        // Remove arguments that might have been added in previous trips through this loop
		sl_remove_from(backendargs,  nbeargs);

		// Choose the base path/filename for output files.
        if (baseout)
            asprintf_safe(&cpy, baseout, inputnum, infile);
        else
            cpy = strdup(infile);
		if (outdir)
			asprintf_safe(&base, "%s/%s", outdir, basename(cpy));
		else
			base = strdup(basename(cpy));
		free(cpy);
		len = strlen(base);
		// trim .xx / .xxx / .xxxx
		if (len > 4) {
            for (j=3; j<=5; j++) {
                if (base[len - j] == '.') {
                    base[len - j] = '\0';
                    suffix = base + len - j + 1;
                    break;
                }
            }
		}

		// the output filenames.
		outfiles = sl_new(16);
		tempfiles = sl_new(4);

		axy->outfn    = sl_appendf(outfiles, "%s.axy",       base);
		axy->matchfn  = sl_appendf(outfiles, "%s.match",     base);
		axy->rdlsfn   = sl_appendf(outfiles, "%s.rdls",      base);
		axy->solvedfn = sl_appendf(outfiles, "%s.solved",    base);
		axy->wcsfn    = sl_appendf(outfiles, "%s.wcs",       base);
		objsfn     = sl_appendf(outfiles, "%s-objs.png",  base);
		redgreenfn = sl_appendf(outfiles, "%s-indx.png",  base);
		ngcfn      = sl_appendf(outfiles, "%s-ngc.png",   base);
		indxylsfn  = sl_appendf(outfiles, "%s-indx.xyls", base);
        if (suffix)
            downloadfn = sl_appendf(outfiles, "%s-downloaded.%s", base, suffix);
        else
            downloadfn = sl_appendf(outfiles, "%s-downloaded", base);


        if (solvedin || solvedindir) {
            if (solvedin && solvedindir)
                asprintf(&axy->solvedinfn, "%s/%s", solvedindir, solvedin);
            else if (solvedin)
                axy->solvedinfn = strdup(solvedin);
            else {
                char* bc = strdup(base);
                char* bn = strdup(basename(bc));
                asprintf(&axy->solvedinfn, "%s/%s.solved", solvedindir, bn);
                free(bn);
                free(bc);
            }
        }
        if (axy->solvedinfn && (strcmp(axy->solvedfn, axy->solvedinfn) == 0)) {
            // solved input and output files are the same: don't delete the input!
            sl_pop(outfiles);
            // MEMLEAK
        }

		free(base);
		base = NULL;

        if (skip_solved) {
            char* tocheck[] = { axy->solvedinfn, axy->solvedfn };
            for (j=0; j<sizeof(tocheck)/sizeof(char*); j++) {
                if (!tocheck[j])
                    continue;
                logverb("Checking for solved file %s\n", tocheck[j]);
                if (file_exists(axy->solvedinfn)) {
                    logmsg("Solved file exists: %s; skipping this input file.\n", axy->solvedinfn);
                    goto nextfile;
                }
            }
        }

		// Check for (and possibly delete) existing output filenames.
		for (i = 0; i < sl_size(outfiles); i++) {
			char* fn = sl_get(outfiles, i);
			if (!file_exists(fn))
				continue;
            if (cont) {
            } else if (overwrite) {
				if (unlink(fn)) {
					SYSERROR("Failed to delete an already-existing output file \"%s\"", fn);
					exit(-1);
				}
			} else {
				logmsg("Output file \"%s\" already exists."
				       "Use the --overwrite flag to overwrite existing files,\n"
                       " or the --continue  flag to not overwrite existing files but still try solving.\n", fn);
				logmsg("Continuing to next input file.\n");
                goto nextfile;
			}
		}

        // Download URL...
        if (!file_exists(infile) &&
            ((strncasecmp(infile, "http://", 7) == 0) ||
             (strncasecmp(infile, "ftp://", 6) == 0))) {

            sl_append(cmdline, usecurl ? "curl" : "wget");
            if (!verbose)
                sl_append(cmdline, usecurl ? "--silent" : "--quiet");
            sl_append(cmdline, usecurl ? "--output" : "-O");
            append_escape(cmdline, downloadfn);
            append_escape(cmdline, infile);

            cmd = sl_implode(cmdline, " ");
            sl_remove_all(cmdline);

            logmsg("Downloading...\n");
            if (run_command(cmd, &ctrlc)) {
                ERROR("%s command %s", sl_get(cmdline, 0),
                      (ctrlc ? "was cancelled" : "failed"));
                exit(-1);
            }
            free(cmd);

            infile = downloadfn;
        }

        logverb("Checking if file \"%s\" is xylist or image: ", infile);
        fflush(NULL);
        reason = NULL;
		isxyls = xylist_is_file_xylist(infile, xcol, ycol, &reason);
        logverb(isxyls ? "xyls\n" : "image\n");
        if (!isxyls)
            logverb("  (not xyls because: %s)\n", reason);
        free(reason);
        fflush(NULL);

		if (isxyls)
			axy->xylsfn = infile;
        else
			axy->imagefn = infile;

		if (axy->imagefn) {
            ppmfn = create_temp_file("ppm", tempdir);
            sl_append_nocopy(tempfiles, ppmfn);

            axy->pnmfn = ppmfn;
            axy->force_ppm = TRUE;
		}

        if (augment_xylist(axy, me)) {
            ERROR("augment-xylist failed");
            exit(-1);
        }

        if (makeplots) {
            // source extraction overlay
            // plotxy -i harvard.axy -I /tmp/pnm -C red -P -w 2 -N 50 | plotxy -w 2 -r 3 -I - -i harvard.axy -C red -n 50 > harvard-objs.png
            append_executable(cmdline, "plotxy", me);
            sl_append(cmdline, "-i");
            append_escape(cmdline, axy->outfn);
            if (axy->imagefn) {
                sl_append(cmdline, "-I");
                append_escape(cmdline, ppmfn);
            }
            if (xcol) {
                sl_append(cmdline, "-X");
                append_escape(cmdline, xcol);
            }
            if (ycol) {
                sl_append(cmdline, "-Y");
                append_escape(cmdline, ycol);
            }
            sl_append(cmdline, "-P");
            sl_append(cmdline, "-C red -w 2 -N 50 -x 1 -y 1");
            
            sl_append(cmdline, "|");

            append_executable(cmdline, "plotxy", me);
            sl_append(cmdline, "-i");
            append_escape(cmdline, axy->outfn);
            if (xcol) {
                sl_append(cmdline, "-X");
                append_escape(cmdline, xcol);
            }
            if (ycol) {
                sl_append(cmdline, "-Y");
                append_escape(cmdline, ycol);
            }
            sl_append(cmdline, "-I - -w 2 -r 3 -C red -n 50 -N 200 -x 1 -y 1");

            sl_append(cmdline, ">");
            append_escape(cmdline, objsfn);

            cmd = sl_implode(cmdline, " ");
            sl_remove_all(cmdline);
            
            if (run_command(cmd, &ctrlc)) {
                ERROR("Plotting command %s",
                      (ctrlc ? "was cancelled" : "failed"));
                if (ctrlc)
                    exit(-1);
                // don't try any more plots...
                errors_print_stack(stdout);
                errors_clear_stack();
                logmsg("Maybe you didn't build the plotting programs?");
                makeplots = FALSE;
            }
            free(cmd);
        }

		append_escape(backendargs, axy->outfn);
		cmd = sl_implode(backendargs, " ");

        logmsg("Solving...\n");
        logverb("Running:\n  %s\n", cmd);
        fflush(NULL);
        if (run_command_get_outputs(cmd, NULL, NULL)) {
            ERROR("backend failed.  Command that failed was:\n  %s", cmd);
			exit(-1);
		}
        free(cmd);
        fflush(NULL);

		if (!file_exists(axy->solvedfn)) {
			// boo hoo.
			//printf("Field didn't solve.\n");
		} else {
			matchfile* mf;
			MatchObj* mo;
            sip_t wcs;
            double ra, dec, fieldw, fieldh;
            char rastr[32], decstr[32];
            char* fieldunits;

			// index rdls to xyls.
            if (wcs_rd2xy(axy->wcsfn, axy->rdlsfn, indxylsfn,
                          NULL, NULL, FALSE, NULL)) {
                ERROR("Failed to project index stars into field coordinates using wcs-rd2xy");
                exit(-1);
            }

            // print info about the field.
            if (!sip_read_header_file(axy->wcsfn, &wcs)) {
                ERROR("Failed to read WCS header from file %s", axy->wcsfn);
                exit(-1);
            }
            sip_get_radec_center(&wcs, &ra, &dec);
            sip_get_radec_center_hms_string(&wcs, rastr, decstr);
            sip_get_field_size(&wcs, &fieldw, &fieldh, &fieldunits);
            logmsg("Field center: (RA,Dec) = (%.4g, %.4g) deg.\n", ra, dec);
            logmsg("Field center: (RA H:M:S, Dec D:M:S) = (%s, %s).\n", rastr, decstr);
            logmsg("Field size: %g x %g %s\n", fieldw, fieldh, fieldunits);

            if (makeplots) {
                // sources + index overlay
                append_executable(cmdline, "plotxy", me);
                sl_append(cmdline, "-i");
                append_escape(cmdline, axy->outfn);
                if (axy->imagefn) {
                    sl_append(cmdline, "-I");
                    append_escape(cmdline, ppmfn);
                }
                if (xcol) {
                    sl_append(cmdline, "-X");
                    append_escape(cmdline, xcol);
                }
                if (ycol) {
                    sl_append(cmdline, "-Y");
                    append_escape(cmdline, ycol);
                }
                sl_append(cmdline, "-P");
                sl_append(cmdline, "-C red -w 2 -r 6 -N 200 -x 1 -y 1");
                sl_append(cmdline, "|");
                append_executable(cmdline, "plotxy", me);
                sl_append(cmdline, "-i");
                append_escape(cmdline, indxylsfn);
                sl_append(cmdline, "-I - -w 2 -r 4 -C green -x 1 -y 1");

                mf = matchfile_open(axy->matchfn);
                if (!mf) {
                    ERROR("Failed to read matchfile %s", axy->matchfn);
                    exit(-1);
                }
                // just read the first match...
                mo = matchfile_read_match(mf);
                if (!mo) {
                    ERROR("Failed to read a match from matchfile %s", axy->matchfn);
                    exit(-1);
                }

                sl_append(cmdline, " -P |");
                append_executable(cmdline, "plotquad", me);
                sl_append(cmdline, "-I -");
                sl_append(cmdline, "-C green");
                sl_append(cmdline, "-w 2");
				sl_appendf(cmdline, "-d %i", mo->dimquads);
                for (i=0; i<(2 * mo->dimquads); i++)
                    sl_appendf(cmdline, " %g", mo->quadpix[i]);

                matchfile_close(mf);
			
                sl_append(cmdline, ">");
                append_escape(cmdline, redgreenfn);

                cmd = sl_implode(cmdline, " ");
                sl_remove_all(cmdline);
                if (verbose)
                    printf("Running:\n  %s\n", cmd);
                fflush(NULL);
                if (run_command(cmd, &ctrlc)) {
                    fflush(NULL);
                    ERROR("Plotting commands %s; exiting.",
                          (ctrlc ? "were cancelled" : "failed"));
                    exit(-1);
                }
                free(cmd);
            }

            if (axy->imagefn && makeplots) {
                sl* lines;

                append_executable(cmdline, "plot-constellations", me);
                if (verbose)
                    sl_append(cmdline, "-v");
				sl_append(cmdline, "-w");
				append_escape(cmdline, axy->wcsfn);
				sl_append(cmdline, "-i");
				append_escape(cmdline, ppmfn);
				sl_append(cmdline, "-N");
				sl_append(cmdline, "-C");
				sl_append(cmdline, "-o");
				append_escape(cmdline, ngcfn);

				cmd = sl_implode(cmdline, " ");
				sl_remove_all(cmdline);
                logverb("Running:\n  %s\n", cmd);
                fflush(NULL);
                if (run_command_get_outputs(cmd, &lines, NULL)) {
                    fflush(NULL);
                    ERROR("plot-constellations failed");
                    exit(-1);
                }
				free(cmd);
                if (lines && sl_size(lines)) {
                    int i;
                    logmsg("Your field contains:\n");
                    for (i=0; i<sl_size(lines); i++) {
                        logmsg("  %s\n", sl_get(lines, i));
                    }
                }
                if (lines)
                    sl_free2(lines);
			}

			// create field rdls?
		}
        fflush(NULL);

    nextfile:        // clean up and move on to the next file.
        free(axy->solvedinfn);
		for (i=0; i<sl_size(tempfiles); i++) {
			char* fn = sl_get(tempfiles, i);
			if (unlink(fn))
				SYSERROR("Failed to delete temp file \"%s\"", fn);
		}
        sl_free2(cmdline);
		sl_free2(outfiles);
		sl_free2(tempfiles);

        errors_print_stack(stdout);
        errors_clear_stack();
	}

	sl_free2(backendargs);
    free(me);

    augment_xylist_free_contents(allaxy);

	return 0;
}

