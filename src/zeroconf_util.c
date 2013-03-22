/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78 -*-
 * 
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2003, 2005 by Apple Computer, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


/**
 * @file
 *
 * Utility functions for zeroconfiguration feature.
 **/


#if defined(DARWIN)


#include <dns_sd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "config.h"
#include "distcc.h"
#include "exitcode.h"
#include "exec.h"
#include "trace.h"
#include "zeroconf_util.h"


// Utility functions


/**
 * Return the concatenation of <code>aName</code>, <code>aRegType</code> and
 * <code>aDomain</code>.
 **/
char *dcc_zc_full_name(const char *aName, const char *aRegType,
                       const char *aDomain)
{
    char *fullName = malloc(1025);

    fullName[0] = '\0';
    strcat(fullName, aName);
    strcat(fullName, ".");
    strcat(fullName, aRegType);
    strcat(fullName, aDomain);

    return fullName;
}


// TXT record generation


/**
 * Simple wrapper around <code>fork</code> and <code>exec</code>.
 * Blocks until the child process terminates.
 * Intended for use by <code>dcc_output_from_simple_execution</code>.
 **/
int dcc_simple_spawn(const char *path, char *const argv[])
{
    int pid = fork();

    if ( pid < 0 ) {
        rs_log_error("Unable to execute %s: (%d) %s", path, errno,
                     strerror(errno));
        return -1;
    } else if ( pid == 0 ) {
        if ( execve(path, argv, NULL) ) {
            rs_log_error("Unexpected error while trying to execute %s: (%d) %s",
                         path, errno, strerror(errno));
            exit(EXIT_DISTCC_FAILED);
        }
    } else {
        int  status;
        long s_us;
        long u_us;

        rs_trace("Executed %s with pid %d", path, pid);

        return dcc_collect_child(pid, &status, &u_us, &s_us);
    }

    return 0;
}


/**
 * Harvest output from <code>dcc_simple_spawn</code> for
 * <code>dcc_generate_txt_record</code>.
 **/
static char *dcc_output_from_simple_execution(const char *path,
                                              char *const argv[])
{
    int   exitVal       = -1;
    char *output        = NULL;
    int   outputPipe[2];
    int   stderrBackup  = -1;
    int   stdoutBackup  = -1;

    stderrBackup = dup(2);

    if ( stderrBackup < 0 ) {
        rs_log_error("Unable to backup stderr: (%d) %s", errno, 
                     strerror(errno));
    }

    stdoutBackup = dup(1);

    if ( stdoutBackup < 0 ) {
        rs_log_error("Unable to backup stdout: (%d) %s", errno,
                     strerror(errno));
    }

    exitVal = pipe(outputPipe);

    if ( exitVal < 0 ) {
        rs_log_error("Unable to create output pipe: (%d) %s", errno,
                     strerror(errno));
    }

    if ( stderrBackup < 0 || stdoutBackup < 0 || exitVal < 0 ) {
        if ( exitVal == 0 ) {
            close(outputPipe[0]);
            close(outputPipe[1]);
        }

        if ( stderrBackup > 0 ) {
            close(stderrBackup);
        }

        if ( stdoutBackup > 0 ) {
            close(stdoutBackup);
        }
    } else {
        int   stderrDup = -1;
        int   stdoutDup = -1;

        // Redirect stdout and stderr temporarily to a pipe.

        stdoutDup = dup2(outputPipe[1], 1);

        if ( stdoutDup < 0 ) {
            rs_log_error("Unable to use pipe for stdout: (%d) %s", errno,
                         strerror(errno));
        }

        stderrDup = dup2(outputPipe[1], 2);

        if ( stderrDup < 0 ) {
            rs_log_error("Unable to use pipe for stderr: (%d) %s", errno,
                         strerror(errno));
        }

        // Close the write end of the pipe.
        // stdout and stderr are now copies of this.
        close(outputPipe[1]);

        if ( stderrDup < 0 || stdoutDup < 0 ) {
            // Restore stderr and stdout

            if ( stdoutDup > 0 ) {
                stdoutDup = dup2(stdoutBackup, 1);

                if ( stdoutDup < 0 ) {
                    rs_fatal("Unable to restore stdout: (%d) %s", errno,
                             strerror(errno));
                }
            }

            if ( stderrDup > 0 ) {
                stderrDup = dup2(stderrBackup, 2);

                if ( stderrDup < 0 ) {
                    rs_fatal("Unable to restore stderr: (%d) %s", errno,
                             strerror(errno));
                }
            }
        } else {
            // In the child, close the read end of the pipe.
            fcntl(outputPipe[0], F_SETFD, 1);

            exitVal = dcc_simple_spawn(path, argv);

            // Restore stderr and stdout

            if ( stdoutDup > 0 ) {
                stdoutDup = dup2(stdoutBackup, 1);

                if ( stdoutDup < 0 ) {
                    rs_fatal("Unable to restore stdout: (%d) %s", errno,
                             strerror(errno));
                }
            }

            if ( stderrDup > 0 ) {
                stderrDup = dup2(stderrBackup, 2);

                if ( stderrDup < 0 ) {
                    rs_fatal("Unable to restore stderr: (%d) %s", errno,
                             strerror(errno));
                }
            }

            if ( exitVal == 0 ) {
                char    bytes[1024];
                ssize_t readBytes   = 0;
                size_t  totalBytes  = 1;

                output = malloc(totalBytes);

                if ( output == NULL ) {
                    rs_log_error("Unable to allocate memory for spawn output");
                } else {
                    output[0] = '\0';

                    do {
                        readBytes = read(outputPipe[0], bytes, 1024);

                        if ( readBytes > 0 ) {
                            char *newOutput;

                            bytes[readBytes] = '\0';
                            totalBytes += (size_t) readBytes;

                            newOutput = realloc(output, totalBytes);

                            if ( newOutput == NULL ) {
                                rs_log_error("Unable to reallocate spawn output memory");
                                free(output);
                                output = NULL;
                            } else {
                                output = newOutput;
                                strcat(output, bytes);
                            }
                        }
                    } while ( output != NULL && readBytes > 0 );
                }
            }
        }

        // Close the read end of the pipe.
        close(outputPipe[0]);

        // Close the backups.

        close(stderrBackup);
        close(stdoutBackup);

        if ( stderrDup < 0 || stdoutDup < 0 ) {
            // Exit, since behavior could be indeterminate at this point.
            exit(EXIT_DISTCC_FAILED);
        }
    }

    return output;
}

char *dcc_get_compiler_versions(void)
{
    char  *gcc33Path       = (char *) "/usr/bin/gcc-3.3";
    char  *gcc40Path       = (char *) "/usr/bin/gcc-4.0";
    char  *gcc33Args[]     = { gcc33Path, (char *) "-v", NULL };
    char  *gcc40Args[]     = { gcc40Path, (char *) "-v", NULL };
    char  *gcc33Version;
    char  *gcc33VersionShort;
    char  *gcc40Version;
    char  *gcc40VersionShort;
    char  *gccVersions[10];	// support at most 10 compiler versions
    char  *gccVersionString;
    int   versionIndex = 0;

    // get the compiler versions

    if (!access(gcc33Path, X_OK)) {
        gcc33Version = dcc_output_from_simple_execution(gcc33Path, gcc33Args);

        if ( gcc33Version == NULL ) {
            rs_log_error("Unable to get gcc version");
        } else {
            // only return the last line of the string
	    gcc33VersionShort = strrchr(gcc33Version, '\n');
	    *gcc33VersionShort = '\0';
	    gcc33VersionShort = strrchr(gcc33Version, '\n') + 1;
	    if (gcc33VersionShort != (char *)1)
		    gccVersions[versionIndex++] = gcc33VersionShort;
        }
    }
    
    if (!access(gcc40Path, X_OK)) {
        gcc40Version = dcc_output_from_simple_execution(gcc40Path, gcc40Args);

        if ( gcc40Version == NULL ) {
            rs_log_error("Unable to get gcc version");
        } else {
            // only return the last line of the string
	    gcc40VersionShort = strrchr(gcc40Version, '\n');
	    *gcc40VersionShort = '\0';
	    gcc40VersionShort = strrchr(gcc40Version, '\n') + 1;
	    if (gcc40VersionShort != (char *)1)
		gccVersions[versionIndex++] = gcc40VersionShort;
        }
    }

    {
	int len = 8;
	int i;
	for (i=0; i< versionIndex; i++ )
		len += strlen(gccVersions[i]);
	gccVersionString = malloc(len);
	sprintf(gccVersionString, "%d", versionIndex);
	for (i=0; i < versionIndex; i++) {
		strcat(gccVersionString, " ");
		strcat(gccVersionString, gccVersions[i]);
	}
    }


    return gccVersionString;
}

#define PROTOCOL_VERSION "373"

char *dcc_get_protocol_version(void)
{
	return PROTOCOL_VERSION;
}

char *dcc_get_system_version(void)
{
    char  *osRelease;
    int    releaseMib[2];
    size_t releaseLen;
    char  *osType;
    int    typeMib[2];
    size_t typeLen;
    char *ret;

    typeMib[0] = CTL_KERN;
    typeMib[1] = KERN_OSTYPE;
    
    releaseMib[0] = CTL_KERN;
    releaseMib[1] = KERN_OSRELEASE;
    
    if ( sysctl(typeMib, 2, NULL, &typeLen, NULL, 0) ) {
        rs_log_error("Unable to get length for kern.ostype: (%d) %s", errno,
                     strerror(errno));
    }
    
    if ( sysctl(releaseMib, 2, NULL, &releaseLen, NULL, 0) ) { 
        rs_log_error("Unable to get length for kern.osrelease: (%d) %s", errno,
                     strerror(errno));
    }
    
    osType    = (char *) malloc(typeLen);
    osRelease = (char *) malloc(releaseLen);
    
    if ( sysctl(typeMib, 2, osType, &typeLen, NULL, 0) ) { 
        rs_log_error("Unable to get kern.ostype: (%d) %s", errno,
                     strerror(errno));
    }

    if ( sysctl(releaseMib, 2, osRelease, &releaseLen, NULL, 0) ) {
        rs_log_error("Unable to get kern.osrelease: (%d) %s", errno,
                     strerror(errno));
    }

    ret = malloc(releaseLen + typeLen + 4);
    sprintf(ret, "%s %s", osRelease, osType);
    return ret;
}

/**
 * Generate the TXT record used to determine whether a given client and
 * server will be compatible.  This will no longer be necessary when
 * certain protocol changes are made so that local preprocessing of files
 * isn't always necessary and other things we require.
 * <br>
 * The TXT record is currently of the form:
 * <br><code>
 * txtvers=1;
 * protovers=373;
 * SystemVersion="</code>OS version<code>";
 * GCCVersions="</code>(number of compiler versions) (version of gcc 3.3) (version of gcc 4.0)<code>";
 * </code>
 *
 * Each key/value pair is preceded by a length byte, per the zeroconf spec.
 **/
char *dcc_generate_txt_record(void)
{
    char  *systemVersion;
    char  *gccVersionString;
    char  *txtRecord     = NULL;
    char  *comAppleEnd   = (char *) "\";";
    char  *comAppleGCC   = (char *) "GCCVersions=\"";
    char  *comAppleOS    = (char *) "SystemVersion=\"";
    char  *protoVers     = (char *) "protovers=" PROTOCOL_VERSION  ";";
    char  *txtVers       = (char *) "txtvers=1;";

    gccVersionString = dcc_get_compiler_versions();
    systemVersion = dcc_get_system_version();

    if ( gccVersionString != NULL && systemVersion != NULL ) {
        txtRecord = malloc(1 + strlen(txtVers) + 1 + 1 + strlen(protoVers) + 1 +
                           1 + strlen(comAppleOS) + strlen(systemVersion) +
                           strlen(comAppleEnd) + 1 + 1 + strlen(comAppleGCC) +
                           strlen(gccVersionString) + strlen(comAppleEnd) + 1);

        if ( txtRecord == NULL ) {
            rs_log_error("Unable to allocate memory for txtRecord");
        } else {
            size_t endOfRecord;

            txtRecord[1] = '\0';
            txtRecord[0] = (char) ( strlen(txtVers) + 1 );

            strcat(txtRecord, txtVers);
            strcat(txtRecord, "\n");

            endOfRecord              = strlen(txtRecord);
            txtRecord[endOfRecord+1] = '\0';
            txtRecord[endOfRecord]   = (char) ( strlen(protoVers) + 1 );

            strcat(txtRecord, protoVers);
            strcat(txtRecord, "\n");

            endOfRecord              = strlen(txtRecord);
            txtRecord[endOfRecord+1] = '\0';
            txtRecord[endOfRecord]   = (char) ( strlen(comAppleOS)  +
						strlen(systemVersion) +
                                                strlen(comAppleEnd) + 1 );

            strcat(txtRecord, comAppleOS);
	    strcat(txtRecord, systemVersion);
            strcat(txtRecord, comAppleEnd);
            strcat(txtRecord, "\n");

            endOfRecord              = strlen(txtRecord);
            txtRecord[endOfRecord+1] = '\0';
            txtRecord[endOfRecord]   = (char) ( strlen(comAppleGCC) +
                                                strlen(gccVersionString)  +
                                                strlen(comAppleEnd) + 1 + 1 );

            strcat(txtRecord, comAppleGCC);
            strcat(txtRecord, gccVersionString);
            strcat(txtRecord, comAppleEnd);
            strcat(txtRecord, "\n");

        }
    }

    if (gccVersionString  != NULL ) {
        free(gccVersionString);
    }

    if (systemVersion != NULL) {
	free(systemVersion);
    }

    return txtRecord;
}


#endif // DARWIN
