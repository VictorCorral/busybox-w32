/* vi: set sw=4 ts=4: */
/*
 * Mini tar implementation for busybox 
 *
 * Note, that as of BusyBox-0.43, tar has been completely rewritten from the
 * ground up.  It still has remnents of the old code lying about, but it is
 * very different now (i.e. cleaner, less global variables, etc)
 *
 * Copyright (C) 2000 by Lineo, inc.
 * Written by Erik Andersen <andersen@lineo.com>, <andersee@debian.org>
 *
 * Based in part in the tar implementation in sash
 *  Copyright (c) 1999 by David I. Bell
 *  Permission is granted to use, distribute, or modify this source,
 *  provided that this copyright notice remains intact.
 *  Permission to distribute sash derived code under the GPL has been granted.
 *
 * Based in part on the tar implementation from busybox-0.28
 *  Copyright (C) 1995 Bruce Perens
 *  This is free software under the GNU General Public License.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */


#include "busybox.h"
#define BB_DECLARE_EXTERN
#define bb_need_io_error
#define bb_need_name_longer_than_foo
#include "messages.c"
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <utime.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <getopt.h>

#ifdef BB_FEATURE_TAR_GZIP
extern int unzip(int in, int out);
extern int gunzip_init();
#endif

/* Tar file constants  */
#ifndef MAJOR
#define MAJOR(dev) (((dev)>>8)&0xff)
#define MINOR(dev) ((dev)&0xff)
#endif

#define NAME_SIZE	100

/* POSIX tar Header Block, from POSIX 1003.1-1990  */
struct TarHeader
{
                                /* byte offset */
	char name[NAME_SIZE];         /*   0-99 */
	char mode[8];                 /* 100-107 */
	char uid[8];                  /* 108-115 */
	char gid[8];                  /* 116-123 */
	char size[12];                /* 124-135 */
	char mtime[12];               /* 136-147 */
	char chksum[8];               /* 148-155 */
	char typeflag;                /* 156-156 */
	char linkname[NAME_SIZE];     /* 157-256 */
	char magic[6];                /* 257-262 */
	char version[2];              /* 263-264 */
	char uname[32];               /* 265-296 */
	char gname[32];               /* 297-328 */
	char devmajor[8];             /* 329-336 */
	char devminor[8];             /* 337-344 */
	char prefix[155];             /* 345-499 */
	char padding[12];             /* 500-512 (pad to exactly the TAR_BLOCK_SIZE) */
};
typedef struct TarHeader TarHeader;


/* A few useful constants */
#define TAR_MAGIC          "ustar"        /* ustar and a null */
#define TAR_VERSION        "  "           /* Be compatable with GNU tar format */
#define TAR_MAGIC_LEN       6
#define TAR_VERSION_LEN     2
#define TAR_BLOCK_SIZE      512

/* A nice enum with all the possible tar file content types */
enum TarFileType 
{
	REGTYPE  = '0',            /* regular file */
	REGTYPE0 = '\0',           /* regular file (ancient bug compat)*/
	LNKTYPE  = '1',            /* hard link */
	SYMTYPE  = '2',            /* symbolic link */
	CHRTYPE  = '3',            /* character special */
	BLKTYPE  = '4',            /* block special */
	DIRTYPE  = '5',            /* directory */
	FIFOTYPE = '6',            /* FIFO special */
	CONTTYPE = '7',            /* reserved */
	GNULONGLINK = 'K',         /* GNU long (>100 chars) link name */
	GNULONGNAME = 'L',         /* GNU long (>100 chars) file name */
};
typedef enum TarFileType TarFileType;

/* This struct ignores magic, non-numeric user name, 
 * non-numeric group name, and the checksum, since
 * these are all ignored by BusyBox tar. */ 
struct TarInfo
{
	int              tarFd;          /* An open file descriptor for reading from the tarball */
	char *           name;           /* File name */
	mode_t           mode;           /* Unix mode, including device bits. */
	uid_t            uid;            /* Numeric UID */
	gid_t            gid;            /* Numeric GID */
	size_t           size;           /* Size of file */
	time_t           mtime;          /* Last-modified time */
	enum TarFileType type;           /* Regular, directory, link, etc */
	char *           linkname;       /* Name for symbolic and hard links */
	long             devmajor;       /* Major number for special device */
	long             devminor;       /* Minor number for special device */
};
typedef struct TarInfo TarInfo;

/* Local procedures to restore files from a tar file.  */
static int readTarFile(int tarFd, int extractFlag, int listFlag, 
		int tostdoutFlag, int verboseFlag, char** extractList,
		char** excludeList);

#ifdef BB_FEATURE_TAR_CREATE
/* Local procedures to save files into a tar file.  */
static int writeTarFile(const char* tarName, int verboseFlag, char **argv,
		char** excludeList);
#endif

#ifdef BB_FEATURE_TAR_GZIP
/* Signal handler for when child gzip process dies...  */
void child_died()
{
	fflush(stdout);
	fflush(stderr);
	exit(EXIT_FAILURE);
}

static int tar_unzip_init(int tarFd)
{
	int child_pid;
	static int unzip_pipe[2];
	/* Cope if child dies... Otherwise we block forever in read()... */
	signal(SIGCHLD, child_died);

	if (pipe(unzip_pipe)!=0)
		error_msg_and_die("pipe error\n");
			
	if ( (child_pid = fork()) == -1)
		error_msg_and_die("fork failure\n");

	if (child_pid==0) {
		/* child process */
		gunzip_init();
		unzip(tarFd, unzip_pipe[1]);
		exit(EXIT_SUCCESS);
	}
	else
		/* return fd of uncompressed data to parent process */
		return(unzip_pipe[0]);
}
#endif

extern int tar_main(int argc, char **argv)
{
	char** excludeList=NULL;
	char** extractList=NULL;
	const char *tarName="-";
#if defined BB_FEATURE_TAR_EXCLUDE
	int excludeListSize=0;
	char *excludeFileName ="-";
	FILE *fileList;
	char file[256];
#endif
#if defined BB_FEATURE_TAR_GZIP
	int unzipFlag    = FALSE;
#endif
	int listFlag     = FALSE;
	int extractFlag  = FALSE;
	int createFlag   = FALSE;
	int verboseFlag  = FALSE;
	int tostdoutFlag = FALSE;
	int status       = FALSE;
	int firstOpt     = TRUE;
	int stopIt;

	if (argc <= 1)
		usage(tar_usage);

	while (*(++argv) && (**argv == '-' || firstOpt == TRUE)) {
		firstOpt=FALSE;
		stopIt=FALSE;
		while (stopIt==FALSE && **argv) {
			switch (*((*argv)++)) {
				case 'c':
					if (extractFlag == TRUE || listFlag == TRUE)
						goto flagError;
					createFlag = TRUE;
					break;
				case 'x':
					if (listFlag == TRUE || createFlag == TRUE)
						goto flagError;
					extractFlag = TRUE;
					break;
				case 't':
					if (extractFlag == TRUE || createFlag == TRUE)
						goto flagError;
					listFlag = TRUE;
					break;
#ifdef BB_FEATURE_TAR_GZIP
				case 'z':
					unzipFlag = TRUE;
					break;
#endif
				case 'v':
					verboseFlag = TRUE;
					break;
				case 'O':
					tostdoutFlag = TRUE;
					break;					
				case 'f':
					if (*tarName != '-')
						error_msg_and_die( "Only one 'f' option allowed\n");
					tarName = *(++argv);
					if (tarName == NULL)
						error_msg_and_die( "Option requires an argument: No file specified\n");
					stopIt=TRUE;
					break;
#if defined BB_FEATURE_TAR_EXCLUDE
				case 'e':
					if (strcmp(*argv, "xclude")==0) {
						excludeList=xrealloc( excludeList,
								sizeof(char *) * (excludeListSize+2));
						excludeList[excludeListSize] = *(++argv);
						if (excludeList[excludeListSize] == NULL)
							error_msg_and_die( "Option requires an argument: No file specified\n");
						/* Remove leading "/"s */
						while (*excludeList[excludeListSize] =='/')
							excludeList[excludeListSize]++;
						/* Tack a NULL onto the end of the list */
						excludeList[++excludeListSize] = NULL;
						stopIt=TRUE;
						break;
					}
				case 'X':
					if (*excludeFileName != '-')
						error_msg_and_die("Only one 'X' option allowed\n");
					excludeFileName = *(++argv);
					if (excludeFileName == NULL)
						error_msg_and_die("Option requires an argument: No file specified\n");
					fileList = fopen (excludeFileName, "r");
					if (! fileList)
						error_msg_and_die("Exclude file: file not found\n");
					while (fgets(file, sizeof(file), fileList) != NULL) {
						excludeList = xrealloc(excludeList,
								sizeof(char *) * (excludeListSize+2));
						if (file[strlen(file)-1] == '\n')
							file[strlen(file)-1] = '\0';
						excludeList[excludeListSize] = xstrdup(file);
						/* Remove leading "/"s */
						while (*excludeList[excludeListSize] == '/')
							excludeList[excludeListSize]++;
						/* Tack a NULL onto the end of the list */
						excludeList[++excludeListSize] = NULL;
					}
					fclose(fileList);
					stopIt=TRUE;
					break;
#endif
				case '-':
						break;
				default:
					usage(tar_usage);
			}
		}
	}

	/* 
	 * Do the correct type of action supplying the rest of the
	 * command line arguments as the list of files to process.
	 */
	if (createFlag == TRUE) {
#ifndef BB_FEATURE_TAR_CREATE
		error_msg_and_die( "This version of tar was not compiled with tar creation support.\n");
#else
#ifdef BB_FEATURE_TAR_GZIP
		if (unzipFlag==TRUE)
			error_msg_and_die("Creation of compressed not internally support by tar, pipe to busybox gunzip\n");
#endif
		status = writeTarFile(tarName, verboseFlag, argv, excludeList);
#endif
	}
	if (listFlag == TRUE || extractFlag == TRUE) {
		int tarFd;
		if (*argv)
			extractList = argv;
		/* Open the tar file for reading.  */
		if (!strcmp(tarName, "-"))
			tarFd = fileno(stdin);
		else
			tarFd = open(tarName, O_RDONLY);
		if (tarFd < 0)
			perror_msg_and_die("Error opening '%s'", tarName);

#ifdef BB_FEATURE_TAR_GZIP	
		/* unzip tarFd in a seperate process */
		if (unzipFlag == TRUE)
			tarFd = tar_unzip_init(tarFd);
#endif			
		status = readTarFile(tarFd, extractFlag, listFlag, tostdoutFlag,
					verboseFlag, extractList, excludeList);
	}

	if (status == TRUE)
		return EXIT_SUCCESS;
	else
		return EXIT_FAILURE;

  flagError:
	error_msg_and_die( "Exactly one of 'c', 'x' or 't' must be specified\n");
}
					
static void
fixUpPermissions(TarInfo *header)
{
	struct utimbuf t;
	/* Now set permissions etc for the new file */
	chown(header->name, header->uid, header->gid);
	chmod(header->name, header->mode);
	/* Reset the time */
	t.actime = time(0);
	t.modtime = header->mtime;
	utime(header->name, &t);
}
				
static int
tarExtractRegularFile(TarInfo *header, int extractFlag, int tostdoutFlag)
{
	size_t  writeSize;
	size_t  readSize;
	size_t  actualWriteSz;
	char    buffer[BUFSIZ];
	size_t  size = header->size;
	int outFd=fileno(stdout);

	/* Open the file to be written, if a file is supposed to be written */
	if (extractFlag==TRUE && tostdoutFlag==FALSE) {
		/* Create the path to the file, just in case it isn't there...
		 * This should not screw up path permissions or anything. */
		create_path(header->name, 0777);
		if ((outFd=open(header->name, O_CREAT|O_TRUNC|O_WRONLY, 
						header->mode & ~S_IFMT)) < 0) {
			error_msg(io_error, header->name, strerror(errno)); 
			return( FALSE);
		}
	}

	/* Write out the file, if we are supposed to be doing that */
	while ( size > 0 ) {
		actualWriteSz=0;
		if ( size > sizeof(buffer) )
			writeSize = readSize = sizeof(buffer);
		else {
			int mod = size % 512;
			if ( mod != 0 )
				readSize = size + (512 - mod);
			else
				readSize = size;
			writeSize = size;
		}
		if ( (readSize = full_read(header->tarFd, buffer, readSize)) <= 0 ) {
			/* Tarball seems to have a problem */
			error_msg("Unexpected EOF in archive\n"); 
			return( FALSE);
		}
		if ( readSize < writeSize )
			writeSize = readSize;

		/* Write out the file, if we are supposed to be doing that */
		if (extractFlag==TRUE) {

			if ((actualWriteSz=full_write(outFd, buffer, writeSize)) != writeSize ) {
				/* Output file seems to have a problem */
				error_msg(io_error, header->name, strerror(errno)); 
				return( FALSE);
			}
		} else {
			actualWriteSz=writeSize;
		}

		size -= actualWriteSz;
	}

	/* Now we are done writing the file out, so try 
	 * and fix up the permissions and whatnot */
	if (extractFlag==TRUE && tostdoutFlag==FALSE) {
		close(outFd);
		fixUpPermissions(header);
	}
	return( TRUE);
}

static int
tarExtractDirectory(TarInfo *header, int extractFlag, int tostdoutFlag)
{

	if (extractFlag==FALSE || tostdoutFlag==TRUE)
		return( TRUE);

	if (create_path(header->name, header->mode) != TRUE) {
		perror_msg("%s: Cannot mkdir", header->name); 
		return( FALSE);
	}
	/* make the final component, just in case it was
	 * omitted by create_path() (which will skip the
	 * directory if it doesn't have a terminating '/') */
	if (mkdir(header->name, header->mode) == 0) {
		fixUpPermissions(header);
	}
	return( TRUE);
}

static int
tarExtractHardLink(TarInfo *header, int extractFlag, int tostdoutFlag)
{
	if (extractFlag==FALSE || tostdoutFlag==TRUE)
		return( TRUE);

	if (link(header->linkname, header->name) < 0) {
		perror_msg("%s: Cannot create hard link to '%s'", header->name,
				header->linkname); 
		return( FALSE);
	}

	/* Now set permissions etc for the new directory */
	fixUpPermissions(header);
	return( TRUE);
}

static int
tarExtractSymLink(TarInfo *header, int extractFlag, int tostdoutFlag)
{
	if (extractFlag==FALSE || tostdoutFlag==TRUE)
		return( TRUE);

#ifdef	S_ISLNK
	if (symlink(header->linkname, header->name) < 0) {
		perror_msg("%s: Cannot create symlink to '%s'", header->name,
				header->linkname); 
		return( FALSE);
	}
	/* Try to change ownership of the symlink.
	 * If libs doesn't support that, don't bother.
	 * Changing the pointed-to-file is the Wrong Thing(tm).
	 */
#if (__GLIBC__ >= 2) && (__GLIBC_MINOR__ >= 1)
	lchown(header->name, header->uid, header->gid);
#endif

	/* Do not change permissions or date on symlink,
	 * since it changes the pointed to file instead.  duh. */
#else
	error_msg("%s: Cannot create symlink to '%s': %s\n", 
			header->name, header->linkname, 
			"symlinks not supported"); 
#endif
	return( TRUE);
}

static int
tarExtractSpecial(TarInfo *header, int extractFlag, int tostdoutFlag)
{
	if (extractFlag==FALSE || tostdoutFlag==TRUE)
		return( TRUE);

	if (S_ISCHR(header->mode) || S_ISBLK(header->mode) || S_ISSOCK(header->mode)) {
		if (mknod(header->name, header->mode, makedev(header->devmajor, header->devminor)) < 0) {
			perror_msg("%s: Cannot mknod", header->name); 
			return( FALSE);
		}
	} else if (S_ISFIFO(header->mode)) {
		if (mkfifo(header->name, header->mode) < 0) {
			perror_msg("%s: Cannot mkfifo", header->name); 
			return( FALSE);
		}
	}

	/* Now set permissions etc for the new directory */
	fixUpPermissions(header);
	return( TRUE);
}

/* Read an octal value in a field of the specified width, with optional
 * spaces on both sides of the number and with an optional null character
 * at the end.  Returns -1 on an illegal format.  */
static long getOctal(const char *cp, int size)
{
	long val = 0;

	for(;(size > 0) && (*cp == ' '); cp++, size--);
	if ((size == 0) || !is_octal(*cp))
		return -1;
	for(; (size > 0) && is_octal(*cp); size--) {
		val = val * 8 + *cp++ - '0';
	}
	for (;(size > 0) && (*cp == ' '); cp++, size--);
	if ((size > 0) && *cp)
		return -1;
	return val;
}


/* Parse the tar header and fill in the nice struct with the details */
static int
readTarHeader(struct TarHeader *rawHeader, struct TarInfo *header)
{
	int i;
	long chksum, sum=0;
	unsigned char *s = (unsigned char *)rawHeader;

	header->name  = rawHeader->name;
	/* Check for and relativify any absolute paths */
	if ( *(header->name) == '/' ) {
		static int alreadyWarned=FALSE;

		while (*(header->name) == '/')
			++*(header->name);

		if (alreadyWarned == FALSE) {
			error_msg("Removing leading '/' from member names\n");
			alreadyWarned = TRUE;
		}
	}

	header->mode  = getOctal(rawHeader->mode, sizeof(rawHeader->mode));
	header->uid   =  getOctal(rawHeader->uid, sizeof(rawHeader->uid));
	header->gid   =  getOctal(rawHeader->gid, sizeof(rawHeader->gid));
	header->size  = getOctal(rawHeader->size, sizeof(rawHeader->size));
	header->mtime = getOctal(rawHeader->mtime, sizeof(rawHeader->mtime));
	chksum = getOctal(rawHeader->chksum, sizeof(rawHeader->chksum));
	header->type  = rawHeader->typeflag;
	header->linkname  = rawHeader->linkname;
	header->devmajor  = getOctal(rawHeader->devmajor, sizeof(rawHeader->devmajor));
	header->devminor  = getOctal(rawHeader->devminor, sizeof(rawHeader->devminor));

	/* Check the checksum */
	for (i = sizeof(*rawHeader); i-- != 0;) {
		sum += *s++;
	}
	/* Remove the effects of the checksum field (replace 
	 * with blanks for the purposes of the checksum) */
	s = rawHeader->chksum;
	for (i = sizeof(rawHeader->chksum) ; i-- != 0;) {
		sum -= *s++;
	}
	sum += ' ' * sizeof(rawHeader->chksum);
	if (sum == chksum )
		return ( TRUE);
	return( FALSE);
}


/*
 * Read a tar file and extract or list the specified files within it.
 * If the list is empty than all files are extracted or listed.
 */
extern int readTarFile(int tarFd, int extractFlag, int listFlag, 
		int tostdoutFlag, int verboseFlag, char** extractList,
		char** excludeList)
{
	int status;
	int errorFlag=FALSE;
	int skipNextHeaderFlag=FALSE;
	TarHeader rawHeader;
	TarInfo header;
	char** tmpList;

	/* Set the umask for this process so it doesn't 
	 * screw up permission setting for us later. */
	umask(0);

	/* Read the tar file, and iterate over it one file at a time */
	while ( (status = full_read(tarFd, (char*)&rawHeader, TAR_BLOCK_SIZE)) == TAR_BLOCK_SIZE ) {

		/* Try to read the header */
		if ( readTarHeader(&rawHeader, &header) == FALSE ) {
			if ( *(header.name) == '\0' ) {
				goto endgame;
			} else {
				errorFlag=TRUE;
				error_msg("Bad tar header, skipping\n");
				continue;
			}
		}
		if ( *(header.name) == '\0' )
				goto endgame;
		header.tarFd = tarFd;

		/* Skip funky extra GNU headers that precede long files */
		if ( (header.type == GNULONGNAME) || (header.type == GNULONGLINK) ) {
			skipNextHeaderFlag=TRUE;
			if (tarExtractRegularFile(&header, FALSE, FALSE) == FALSE)
				errorFlag = TRUE;
			continue;
		}
		if ( skipNextHeaderFlag == TRUE ) { 
			skipNextHeaderFlag=FALSE;
			error_msg(name_longer_than_foo, NAME_SIZE); 
			if (tarExtractRegularFile(&header, FALSE, FALSE) == FALSE)
				errorFlag = TRUE;
			continue;
		}

#if defined BB_FEATURE_TAR_EXCLUDE
		{
			int skipFlag=FALSE;
			/* Check for excluded files....  */
			for (tmpList=excludeList; tmpList && *tmpList; tmpList++) {
				/* Do some extra hoop jumping for when directory names
				 * end in '/' but the entry in tmpList doesn't */
				if (strncmp( *tmpList, header.name, strlen(*tmpList))==0 || (
							header.name[strlen(header.name)-1]=='/'
							&& strncmp( *tmpList, header.name, 
								MIN(strlen(header.name)-1, strlen(*tmpList)))==0)) {
					/* If it is a regular file, pretend to extract it with
					 * the extractFlag set to FALSE, so the junk in the tarball
					 * is properly skipped over */
					if ( header.type==REGTYPE || header.type==REGTYPE0 ) {
						if (tarExtractRegularFile(&header, FALSE, FALSE) == FALSE)
							errorFlag = TRUE;
					}
					skipFlag=TRUE;
					break;
				}
			}
			/* There are not the droids you're looking for, move along */
			if (skipFlag==TRUE)
				continue;
		}
#endif
		if (extractList != NULL) {
			int skipFlag = TRUE;
			for (tmpList = extractList; *tmpList != NULL; tmpList++) {
				if (strncmp( *tmpList, header.name, strlen(*tmpList))==0 || (
							header.name[strlen(header.name)-1]=='/'
							&& strncmp( *tmpList, header.name, 
								MIN(strlen(header.name)-1, strlen(*tmpList)))==0)) {
					/* If it is a regular file, pretend to extract it with
					 * the extractFlag set to FALSE, so the junk in the tarball
					 * is properly skipped over */
					skipFlag = FALSE;
					memmove(extractList+1, extractList,
								sizeof(*extractList)*(tmpList-extractList));
					extractList++;
					break;
				}
			}
			/* There are not the droids you're looking for, move along */
			if (skipFlag == TRUE) {
				if ( header.type==REGTYPE || header.type==REGTYPE0 )
					if (tarExtractRegularFile(&header, FALSE, FALSE) == FALSE)
						errorFlag = TRUE;
				continue;
			}
		}

		if (listFlag == TRUE) {
			/* Special treatment if the list (-t) flag is on */
			if (verboseFlag == TRUE) {
				int len, len1;
				char buf[35];
				struct tm *tm = localtime (&(header.mtime));

				len=printf("%s ", mode_string(header.mode));
				my_getpwuid(buf, header.uid);
				if (! *buf)
					len+=printf("%d", header.uid);
				else
					len+=printf("%s", buf);
				my_getgrgid(buf, header.gid);
				if (! *buf)
					len+=printf("/%-d ", header.gid);
				else
					len+=printf("/%-s ", buf);

				if (header.type==CHRTYPE || header.type==BLKTYPE) {
					len1=snprintf(buf, sizeof(buf), "%ld,%-ld ", 
							header.devmajor, header.devminor);
				} else {
					len1=snprintf(buf, sizeof(buf), "%lu ", (long)header.size);
				}
				/* Jump through some hoops to make the columns match up */
				for(;(len+len1)<31;len++)
					printf(" ");
				printf(buf);

				/* Use ISO 8610 time format */
				if (tm) { 
					printf ("%04d-%02d-%02d %02d:%02d:%02d ", 
							tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, 
							tm->tm_hour, tm->tm_min, tm->tm_sec);
				}
			}
			printf("%s", header.name);
			if (verboseFlag == TRUE) {
				if (header.type==LNKTYPE)	/* If this is a link, say so */
					printf(" link to %s", header.linkname);
				else if (header.type==SYMTYPE)
					printf(" -> %s", header.linkname);
			}
			printf("\n");
		}

		/* List contents if we are supposed to do that */
		if (verboseFlag == TRUE && extractFlag == TRUE) {
			/* Now the normal listing */
			FILE *vbFd = stdout;
			if (tostdoutFlag == TRUE)	// If the archive goes to stdout, verbose to stderr
				vbFd = stderr;
			fprintf(vbFd, "%s\n", header.name);
		}
			
		/* Remove files if we would overwrite them */
		if (extractFlag == TRUE && tostdoutFlag == FALSE)
			unlink(header.name);

		/* If we got here, we can be certain we have a legitimate 
		 * header to work with.  So work with it.  */
		switch ( header.type ) {
			case REGTYPE:
			case REGTYPE0:
				/* If the name ends in a '/' then assume it is
				 * supposed to be a directory, and fall through */
				if (header.name[strlen(header.name)-1] != '/') {
					if (tarExtractRegularFile(&header, extractFlag, tostdoutFlag)==FALSE)
						errorFlag=TRUE;
					break;
				}
			case DIRTYPE:
				if (tarExtractDirectory( &header, extractFlag, tostdoutFlag)==FALSE)
					errorFlag=TRUE;
				break;
			case LNKTYPE:
				if (tarExtractHardLink( &header, extractFlag, tostdoutFlag)==FALSE)
					errorFlag=TRUE;
				break;
			case SYMTYPE:
				if (tarExtractSymLink( &header, extractFlag, tostdoutFlag)==FALSE)
					errorFlag=TRUE;
				break;
			case CHRTYPE:
			case BLKTYPE:
			case FIFOTYPE:
				if (tarExtractSpecial( &header, extractFlag, tostdoutFlag)==FALSE)
					errorFlag=TRUE;
				break;
#if 0
			/* Handled earlier */
			case GNULONGNAME:
			case GNULONGLINK:
				skipNextHeaderFlag=TRUE;
				break;
#endif
			default:
				error_msg("Unknown file type '%c' in tar file\n", header.type);
				close( tarFd);
				return( FALSE);
		}
	}
	close(tarFd);
	if (status > 0) {
		/* Bummer - we read a partial header */
		perror_msg("Error reading tar file");
		return ( FALSE);
	}
	else if (errorFlag==TRUE) {
		error_msg( "Error exit delayed from previous errors\n");
		return( FALSE);
	} else 
		return( status);

	/* Stuff to do when we are done */
endgame:
	close( tarFd);
	if (extractList != NULL) {
		for (; *extractList != NULL; extractList++) {
			error_msg("%s: Not found in archive\n", *extractList);
			errorFlag = TRUE;
		}
	}
	if ( *(header.name) == '\0' ) {
		if (errorFlag==TRUE)
			error_msg( "Error exit delayed from previous errors\n");
		else
			return( TRUE);
	} 
	return( FALSE);
}


#ifdef BB_FEATURE_TAR_CREATE

/*
** writeTarFile(),  writeFileToTarball(), and writeTarHeader() are
** the only functions that deal with the HardLinkInfo structure.
** Even these functions use the xxxHardLinkInfo() functions.
*/
typedef struct HardLinkInfo HardLinkInfo;
struct HardLinkInfo
{
	HardLinkInfo *next;           /* Next entry in list */
	dev_t dev;                    /* Device number */
	ino_t ino;                    /* Inode number */
	short linkCount;              /* (Hard) Link Count */
	char name[1];                 /* Start of filename (must be last) */
};

/* Some info to be carried along when creating a new tarball */
struct TarBallInfo
{
	char* fileName;               /* File name of the tarball */
	int tarFd;                    /* Open-for-write file descriptor
									 for the tarball */
	struct stat statBuf;          /* Stat info for the tarball, letting
									 us know the inode and device that the
									 tarball lives, so we can avoid trying 
									 to include the tarball into itself */
	int verboseFlag;              /* Whether to print extra stuff or not */
	char** excludeList;           /* List of files to not include */
	HardLinkInfo *hlInfoHead;     /* Hard Link Tracking Information */
	HardLinkInfo *hlInfo;         /* Hard Link Info for the current file */
};
typedef struct TarBallInfo TarBallInfo;


/* Might be faster (and bigger) if the dev/ino were stored in numeric order;) */
static void
addHardLinkInfo (HardLinkInfo **hlInfoHeadPtr, dev_t dev, ino_t ino,
		short linkCount, const char *name)
{
	/* Note: hlInfoHeadPtr can never be NULL! */
	HardLinkInfo *hlInfo;

	hlInfo = (HardLinkInfo *)xmalloc(sizeof(HardLinkInfo)+strlen(name)+1);
	if (hlInfo) {
		hlInfo->next = *hlInfoHeadPtr;
		*hlInfoHeadPtr = hlInfo;
		hlInfo->dev = dev;
		hlInfo->ino = ino;
		hlInfo->linkCount = linkCount;
		strcpy(hlInfo->name, name);
	}
	return;
}

static void
freeHardLinkInfo (HardLinkInfo **hlInfoHeadPtr)
{
	HardLinkInfo *hlInfo = NULL;
	HardLinkInfo *hlInfoNext = NULL;

	if (hlInfoHeadPtr) {
		hlInfo = *hlInfoHeadPtr;
		while (hlInfo) {
			hlInfoNext = hlInfo->next;
			free(hlInfo);
			hlInfo = hlInfoNext;
		}
		*hlInfoHeadPtr = NULL;
	}
	return;
}

/* Might be faster (and bigger) if the dev/ino were stored in numeric order;) */
static HardLinkInfo *
findHardLinkInfo (HardLinkInfo *hlInfo, dev_t dev, ino_t ino)
{
	while(hlInfo) {
		if ((ino == hlInfo->ino) && (dev == hlInfo->dev))
			break;
		hlInfo = hlInfo->next;
	}
	return(hlInfo);
}

/* Put an octal string into the specified buffer.
 * The number is zero and space padded and possibly null padded.
 * Returns TRUE if successful.  */ 
static int putOctal (char *cp, int len, long value)
{
	int tempLength;
	char tempBuffer[32];
	char *tempString = tempBuffer;

	/* Create a string of the specified length with an initial space,
	 * leading zeroes and the octal number, and a trailing null.  */
	sprintf (tempString, "%0*lo", len - 1, value);

	/* If the string is too large, suppress the leading space.  */
	tempLength = strlen (tempString) + 1;
	if (tempLength > len) {
		tempLength--;
		tempString++;
	}

	/* If the string is still too large, suppress the trailing null.  */
	if (tempLength > len)
		tempLength--;

	/* If the string is still too large, fail.  */
	if (tempLength > len)
		return FALSE;

	/* Copy the string to the field.  */
	memcpy (cp, tempString, len);

	return TRUE;
}

/* Write out a tar header for the specified file/directory/whatever */
static int
writeTarHeader(struct TarBallInfo *tbInfo, const char *fileName, struct stat *statbuf)
{
	long chksum=0;
	struct TarHeader header;
	const unsigned char *cp = (const unsigned char *) &header;
	ssize_t size = sizeof(struct TarHeader);
		
	memset( &header, 0, size);

	strncpy(header.name, fileName, sizeof(header.name)); 

	putOctal(header.mode, sizeof(header.mode), statbuf->st_mode);
	putOctal(header.uid, sizeof(header.uid), statbuf->st_uid);
	putOctal(header.gid, sizeof(header.gid), statbuf->st_gid);
	putOctal(header.size, sizeof(header.size), 0); /* Regular file size is handled later */
	putOctal(header.mtime, sizeof(header.mtime), statbuf->st_mtime);
	strncpy(header.magic, TAR_MAGIC TAR_VERSION, 
			TAR_MAGIC_LEN + TAR_VERSION_LEN );

	/* Enter the user and group names (default to root if it fails) */
	my_getpwuid(header.uname, statbuf->st_uid);
	if (! *header.uname)
		strcpy(header.uname, "root");
	my_getgrgid(header.gname, statbuf->st_gid);
	if (! *header.uname)
		strcpy(header.uname, "root");

	if (tbInfo->hlInfo) {
		/* This is a hard link */
		header.typeflag = LNKTYPE;
		strncpy(header.linkname, tbInfo->hlInfo->name, sizeof(header.linkname));
	} else if (S_ISLNK(statbuf->st_mode)) {
		int link_size=0;
		char buffer[BUFSIZ];
		header.typeflag  = SYMTYPE;
		link_size = readlink(fileName, buffer, sizeof(buffer) - 1);
		if ( link_size < 0) {
			perror_msg("Error reading symlink '%s'", header.name);
			return ( FALSE);
		}
		buffer[link_size] = '\0';
		strncpy(header.linkname, buffer, sizeof(header.linkname)); 
	} else if (S_ISDIR(statbuf->st_mode)) {
		header.typeflag  = DIRTYPE;
		strncat(header.name, "/", sizeof(header.name)); 
	} else if (S_ISCHR(statbuf->st_mode)) {
		header.typeflag  = CHRTYPE;
		putOctal(header.devmajor, sizeof(header.devmajor), MAJOR(statbuf->st_rdev));
		putOctal(header.devminor, sizeof(header.devminor), MINOR(statbuf->st_rdev));
	} else if (S_ISBLK(statbuf->st_mode)) {
		header.typeflag  = BLKTYPE;
		putOctal(header.devmajor, sizeof(header.devmajor), MAJOR(statbuf->st_rdev));
		putOctal(header.devminor, sizeof(header.devminor), MINOR(statbuf->st_rdev));
	} else if (S_ISFIFO(statbuf->st_mode)) {
		header.typeflag  = FIFOTYPE;
	} else if (S_ISREG(statbuf->st_mode)) {
		header.typeflag  = REGTYPE;
		putOctal(header.size, sizeof(header.size), statbuf->st_size);
	} else {
		error_msg("%s: Unknown file type\n", fileName);
		return ( FALSE);
	}

	/* Calculate and store the checksum (i.e. the sum of all of the bytes of
	 * the header).  The checksum field must be filled with blanks for the
	 * calculation.  The checksum field is formatted differently from the
	 * other fields: it has [6] digits, a null, then a space -- rather than
	 * digits, followed by a null like the other fields... */
	memset(header.chksum, ' ', sizeof(header.chksum));
	cp = (const unsigned char *) &header;
	while (size-- > 0)
		chksum += *cp++;
	putOctal(header.chksum, 7, chksum);
	
	/* Now write the header out to disk */
	if ((size=full_write(tbInfo->tarFd, (char*)&header, sizeof(struct TarHeader))) < 0) {
		error_msg(io_error, fileName, strerror(errno)); 
		return ( FALSE);
	}
	/* Pad the header up to the tar block size */
	for (; size<TAR_BLOCK_SIZE; size++) {
		write(tbInfo->tarFd, "\0", 1);
	}
	/* Now do the verbose thing (or not) */
	if (tbInfo->verboseFlag==TRUE) {
		FILE *vbFd = stdout;
		if (tbInfo->tarFd == fileno(stdout))	// If the archive goes to stdout, verbose to stderr
			vbFd = stderr;
		fprintf(vbFd, "%s\n", header.name);
	}

	return ( TRUE);
}


static int writeFileToTarball(const char *fileName, struct stat *statbuf, void* userData)
{
	struct TarBallInfo *tbInfo = (struct TarBallInfo *)userData;
#if defined BB_FEATURE_TAR_EXCLUDE
	char** tmpList;
#endif

	/*
	** Check to see if we are dealing with a hard link.
	** If so -
	** Treat the first occurance of a given dev/inode as a file while
	** treating any additional occurances as hard links.  This is done
	** by adding the file information to the HardLinkInfo linked list.
	*/
	tbInfo->hlInfo = NULL;
	if (statbuf->st_nlink > 1) {
		tbInfo->hlInfo = findHardLinkInfo(tbInfo->hlInfoHead, statbuf->st_dev, 
				statbuf->st_ino);
		if (tbInfo->hlInfo == NULL)
			addHardLinkInfo (&tbInfo->hlInfoHead, statbuf->st_dev,
					statbuf->st_ino, statbuf->st_nlink, fileName);
	}

	/* It is against the rules to archive a socket */
	if (S_ISSOCK(statbuf->st_mode)) {
		error_msg("%s: socket ignored\n", fileName);
		return( TRUE);
	}

	/* It is a bad idea to store the archive we are in the process of creating,
	 * so check the device and inode to be sure that this particular file isn't
	 * the new tarball */
	if (tbInfo->statBuf.st_dev == statbuf->st_dev &&
			tbInfo->statBuf.st_ino == statbuf->st_ino) {
		error_msg("%s: file is the archive; skipping\n", fileName);
		return( TRUE);
	}

	while (fileName[0] == '/') {
		static int alreadyWarned=FALSE;
		if (alreadyWarned==FALSE) {
			error_msg("Removing leading '/' from member names\n");
			alreadyWarned=TRUE;
		}
		fileName++;
	}

	if (strlen(fileName) >= NAME_SIZE) {
		error_msg(name_longer_than_foo, NAME_SIZE);
		return ( TRUE);
	}

	if (fileName[0] == '\0')
		return TRUE;

#if defined BB_FEATURE_TAR_EXCLUDE
	/* Check for excluded files....  */
	for (tmpList=tbInfo->excludeList; tmpList && *tmpList; tmpList++) {
		/* Do some extra hoop jumping for when directory names
		 * end in '/' but the entry in tmpList doesn't */
		if (strncmp( *tmpList, fileName, strlen(*tmpList))==0 || (
					fileName[strlen(fileName)-1]=='/'
					&& strncmp( *tmpList, fileName, 
						MIN(strlen(fileName)-1, strlen(*tmpList)))==0)) {
			return SKIP;
		}
	}
#endif

	if (writeTarHeader(tbInfo, fileName, statbuf)==FALSE) {
		return( FALSE);
	} 

	/* Now, if the file is a regular file, copy it out to the tarball */
	if ((tbInfo->hlInfo == NULL)
	&&  (S_ISREG(statbuf->st_mode))) {
		int  inputFileFd;
		char buffer[BUFSIZ];
		ssize_t size=0, readSize=0;

		/* open the file we want to archive, and make sure all is well */
		if ((inputFileFd = open(fileName, O_RDONLY)) < 0) {
			error_msg("%s: Cannot open: %s\n", fileName, strerror(errno));
			return( FALSE);
		}
		
		/* write the file to the archive */
		while ( (size = full_read(inputFileFd, buffer, sizeof(buffer))) > 0 ) {
			if (full_write(tbInfo->tarFd, buffer, size) != size ) {
				/* Output file seems to have a problem */
				error_msg(io_error, fileName, strerror(errno)); 
				return( FALSE);
			}
			readSize+=size;
		}
		if (size == -1) {
			error_msg(io_error, fileName, strerror(errno)); 
			return( FALSE);
		}
		/* Pad the file up to the tar block size */
		for (; (readSize%TAR_BLOCK_SIZE) != 0; readSize++) {
			write(tbInfo->tarFd, "\0", 1);
		}
		close( inputFileFd);
	}

	return( TRUE);
}

static int writeTarFile(const char* tarName, int verboseFlag, char **argv,
		char** excludeList)
{
	int tarFd=-1;
	int errorFlag=FALSE;
	ssize_t size;
	struct TarBallInfo tbInfo;
	tbInfo.verboseFlag = verboseFlag;
	tbInfo.hlInfoHead = NULL;

	/* Make sure there is at least one file to tar up.  */
	if (*argv == NULL)
		error_msg_and_die("Cowardly refusing to create an empty archive\n");

	/* Open the tar file for writing.  */
	if (!strcmp(tarName, "-"))
		tbInfo.tarFd = fileno(stdout);
	else
		tbInfo.tarFd = open (tarName, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (tbInfo.tarFd < 0) {
		perror_msg( "Error opening '%s'", tarName);
		freeHardLinkInfo(&tbInfo.hlInfoHead);
		return ( FALSE);
	}
	tbInfo.excludeList=excludeList;
	/* Store the stat info for the tarball's file, so
	 * can avoid including the tarball into itself....  */
	if (fstat(tbInfo.tarFd, &tbInfo.statBuf) < 0)
		error_msg_and_die(io_error, tarName, strerror(errno)); 

	/* Set the umask for this process so it doesn't 
	 * screw up permission setting for us later. */
	umask(0);

	/* Read the directory/files and iterate over them one at a time */
	while (*argv != NULL) {
		if (recursive_action(*argv++, TRUE, FALSE, FALSE,
					writeFileToTarball, writeFileToTarball, 
					(void*) &tbInfo) == FALSE) {
			errorFlag = TRUE;
		}
	}
	/* Write two empty blocks to the end of the archive */
	for (size=0; size<(2*TAR_BLOCK_SIZE); size++) {
		write(tbInfo.tarFd, "\0", 1);
	}

	/* To be pedantically correct, we would check if the tarball
	 * is smaller than 20 tar blocks, and pad it if it was smaller,
	 * but that isn't necessary for GNU tar interoperability, and
	 * so is considered a waste of space */

	/* Hang up the tools, close up shop, head home */
	close(tarFd);
	if (errorFlag == TRUE) {
		error_msg("Error exit delayed from previous errors\n");
		freeHardLinkInfo(&tbInfo.hlInfoHead);
		return(FALSE);
	}
	freeHardLinkInfo(&tbInfo.hlInfoHead);
	return( TRUE);
}


#endif

