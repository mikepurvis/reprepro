/*  This file is part of "reprepro"
 *  Copyright (C) 2008 Bernhard R. Link
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "globals.h"
#include "error.h"
#include "filecntl.h"
#include "checksums.h"
#include "mprintf.h"
#include "dirs.h"
#include "names.h"
#include "aptmethod.h"
#include "signature.h"
#include "readrelease.h"
#include "uncompression.h"
#include "remoterepository.h"

/* This is code to handle lists from remote repositories.
   Those are stored in the lists/ (or --listdir) directory
   and needs some maintaince:

   - cleaning (unneeded) lists from that directory
   - deciding what to download from a remote repository
     (needs knowledge what is there and what is there)
   - in the future: implement ed to use remote .diffs
*/


struct remote_repository {
	struct remote_repository *next, *prev;

	/* repository is determined by pattern name currently.
	 * That might change if there is some safe way to combine
	 * some. (note that method options might make equally looking
	 * repositories different ones, so that is hard to decide).
	 *
	 * This is possible as pattern is not modifyable in options
	 * or method by the using distribution.
	 */
	const char *name;
	const char *method;
	const char *fallback;
	const struct strlist *config;

	struct aptmethod *download;

	struct remote_distribution *distributions;
};
static struct remote_repository *repositories = NULL;

struct remote_distribution {
	struct remote_distribution *next;

	/* repository and suite uniquely identify it,
	   as the only thing the distribution can change is the suite.
	   Currently most of the other fields would also fit in the
	   remote_repository structure, but I plan to add new patters
	   allowing this by distribution...
	*/
	struct remote_repository *repository;
	char *suite;

	/* flat repository */
	bool flat;
	char *suite_base_dir;

	/* if true, do not download or check Release file */
	bool ignorerelease;
	/* if != NULL, get Release.gpg and check with this options */
	/*@null@*/char *verifyrelease;
	/* hashes to ignore */
	bool ignorehashes[cs_hashCOUNT];

	/* local copy of Release and Release.gpg file, once and if available */
	/*@null@*/char *releasefile;
	/*@null@*/char *releasegpgfile;

	/* filenames and checksums from the Release file */
	struct checksumsarray remotefiles;

	/* the index files we need */
	struct remote_index *indices;
};

struct remote_index {
	/* next index in remote distribution */
	struct remote_index *next;

	struct remote_distribution *from;

	/* remote filename as to be found in Release file*/
	char *filename_in_release;

	/* the name without suffix in the lists/ dir */
	char *cachefilename;
	/* the basename of the above */
	const char *cachebasename;

	/* index in checksums for the different types, -1 = not avail */
	int ofs[c_COUNT], diff_ofs;

	/* the choosen file */
	/* - the compression used */
	enum compression compression;

	bool queued;
	bool needed;
};

struct cachedlistfile {
	struct cachedlistfile *next;
	const char *basename;
	int partcount;
	const char *parts[5];
	/* might be used by some rule */
	bool needed, deleted;
	char fullfilename[];
};


static void remote_index_free(/*@only@*/struct remote_index *i) {
	if( i == NULL )
		return;
	free(i->cachefilename);
	free(i->filename_in_release);
	free(i);
}

static void remote_distribution_free(/*@only@*/struct remote_distribution *d) {
	if( d == NULL )
		return;
	free(d->suite);
	free(d->verifyrelease);
	free(d->releasefile);
	free(d->releasegpgfile);
	free(d->suite_base_dir);
	checksumsarray_done(&d->remotefiles);
	while( d->indices != NULL ) {
		struct remote_index *h = d->indices;
		d->indices = h->next;
		remote_index_free(h);
	}
	free(d);
}

void remote_repository_free(struct remote_repository *remote) {
	if( remote == NULL )
		return;
	while( remote->distributions != NULL ) {
		struct remote_distribution *h = remote->distributions;
		remote->distributions = h->next;
		remote_distribution_free(h);
	}
	if( remote->next != NULL )
		remote->next->prev = remote->prev;
	if( remote->prev != NULL )
		remote->prev->next = remote->next;
	free(remote);
	return;
}

static inline void cachedlistfile_freelist(/*|only@*/struct cachedlistfile *c) {
	while( c != NULL ) {
		struct cachedlistfile *n = c->next;
		free(c);
		c = n;
	}
}

static /*@null@*/ struct cachedlistfile *cachedlistfile_new(const char *basename, size_t len, size_t listdirlen) {
	struct cachedlistfile *c;
	size_t l;
	char *p;
	char ch;

	c = malloc(sizeof(struct cachedlistfile) + listdirlen + 2*len + 3);
	if( FAILEDTOALLOC(c) )
		return NULL;
	c->next = NULL;
	c->needed = false;
	c->deleted = false;
	p = c->fullfilename;
	assert( (size_t)(p - (char*)c) <= sizeof(struct cachedlistfile) );
	memcpy(p, global.listdir, listdirlen);
	p += listdirlen;
	*(p++) = '/';
	assert( (size_t)(p - c->fullfilename) == listdirlen + 1 );
	c->basename = p;
	memcpy(p, basename, len); p += len;
	*(p++) = '\0';
	assert( (size_t)(p - c->fullfilename) == listdirlen + len + 2 );

	c->parts[0] = p;
	c->partcount = 1;
	l = len;
	while( l-- > 0 && (ch = *(basename++)) != '\0' ) {
		if( ch == '_' ) {
			*(p++) = '\0';
			if( c->partcount < 5 )
				c->parts[c->partcount] = p;
			c->partcount++;
		} else if( ch == '%' ) {
			char first, second;

			if( len <= 1 ) {
				c->partcount = 0;
				return c;
			}
			first = *(basename++);
			second = *(basename++);
			if( first >= '0' && first <= '9' )
				*p = (first - '0') << 4;
			else if( first >= 'a' && first <= 'f' )
				*p = (first - 'a' + 10) << 4;
			else {
				c->partcount = 0;
				return c;
			}
			if( second >= '0' && second <= '9' )
				*p |= (second - '0');
			else if( second >= 'a' && second <= 'f' )
				*p |= (second - 'a' + 10);
			else {
				c->partcount = 0;
				return c;
			}
			p++;
		} else
			*(p++) = ch;
	}
	*(p++) = '\0';
	assert( (size_t)(p - c->fullfilename) <= listdirlen + 2*len + 3 );
	return c;
}

static retvalue cachedlists_scandir(/*@out@*/struct cachedlistfile **cachedfiles_p) {
	struct cachedlistfile *cachedfiles = NULL, **next_p;
	struct dirent *r;
	size_t listdirlen = strlen(global.listdir);
	DIR *dir;

	// TODO: check if it is always created before...
	dir = opendir(global.listdir);
	if( dir == NULL ) {
		int e = errno;
		fprintf(stderr,"Error %d opening directory '%s': %s!\n",
				e, global.listdir, strerror(e));
		return RET_ERRNO(e);
	}
	next_p = &cachedfiles;
	while( true ) {
		size_t namelen;
		int e;

		errno = 0;
		r = readdir(dir);
		if( r == NULL ) {
			e = errno;
			if( e == 0 )
				break;
			/* this should not happen... */
			e = errno;
			fprintf(stderr, "Error %d reading dir '%s': %s!\n",
					e, global.listdir, strerror(e));
			(void)closedir(dir);
			cachedlistfile_freelist(cachedfiles);
			return RET_ERRNO(e);
		}
		namelen = _D_EXACT_NAMLEN(r);
		if( namelen == 1 && r->d_name[0] == '.' )
			continue;
		if( namelen == 2 && r->d_name[0] == '.' && r->d_name[1] == '.' )
			continue;
		*next_p = cachedlistfile_new(r->d_name, namelen, listdirlen);
		if( FAILEDTOALLOC(*next_p) ) {
			(void)closedir(dir);
			cachedlistfile_freelist(cachedfiles);
			return RET_ERROR_OOM;
		}
		next_p = &(*next_p)->next;
	}
	if( closedir(dir) != 0 ) {
		int e = errno;
		fprintf(stderr, "Error %d closing directory '%s': %s!\n",
				e, global.listdir, strerror(e));
		cachedlistfile_freelist(cachedfiles);
		return RET_ERRNO(e);
	}
	*cachedfiles_p = cachedfiles;
	return RET_OK;
}

static retvalue cachedlistfile_delete(struct cachedlistfile *old) {
	int e;
	if( old->deleted )
		return RET_OK;
	e = deletefile(old->fullfilename);
	if( e != 0 )
		return RET_ERRNO(e);
	old->deleted = true;
	return RET_OK;
}

struct remote_repository *remote_repository_prepare(const char *name, const char *method, const char *fallback, const struct strlist *config) {
	struct remote_repository *n;

	/* calling code ensures no two with the same name are created,
	 * so just create it... */

	n = calloc(1, sizeof(struct remote_repository));
	if( FAILEDTOALLOC(n) )
		return NULL;
	n->name = name;
	n->method = method;
	n->fallback = fallback;
	n->config = config;

	n->next = repositories;
	if( n->next != NULL )
		n->next->prev = n;
	repositories = n;

	return n;
}

/* This escaping is quite harsh, but so nothing bad can happen... */
static inline size_t escapedlen(const char *p) {
	size_t l = 0;
	if( *p == '-' ) {
		l = 3;
		p++;
	}
	while( *p != '\0' ) {
		if( (*p < 'A' || *p > 'Z' ) && (*p < 'a' || *p > 'z' ) &&
		    ( *p < '0' || *p > '9') && *p != '-' )
			l +=3;
		else
			l++;
		p++;
	}
	return l;
}

static inline char *escapedcopy(char *dest, const char *orig) {
	static char hex[16] = "0123456789ABCDEF";
	if( *orig == '-' ) {
		orig++;
		*dest = '%'; dest++;
		*dest = '2'; dest++;
		*dest = 'D'; dest++;
	}
	while( *orig != '\0' ) {
		if( (*orig < 'A' || *orig > 'Z' ) && (*orig < 'a' || *orig > 'z' ) && ( *orig < '0' || *orig > '9') && *orig != '-' ) {
			*dest = '%'; dest++;
			*dest = hex[(*orig >> 4)& 0xF ]; dest++;
			*dest = hex[*orig & 0xF ]; dest++;
		} else {
			*dest = *orig;
			dest++;
		}
		orig++;
	}
	return dest;
}

char *genlistsfilename(const char *type, unsigned int count, ...) {
	const char *fields[count];
	unsigned int i;
	size_t listdir_len, type_len, len;
	char *result, *p;
	va_list ap;

	len = 0;
	va_start(ap, count);
	for( i = 0 ; i < count ; i++ ) {
		fields[i] = va_arg(ap, const char*);
		assert( fields[i] != NULL );
		len += escapedlen(fields[i]) + 1;
	}
	/* check sentinel */
	assert( va_arg(ap, const char*) == NULL );
	va_end(ap);
	listdir_len = strlen(global.listdir);
	if( type != NULL )
		type_len = strlen(type);
	else
		type_len = 0;

	result = malloc(listdir_len + type_len + len + 2);
	if( FAILEDTOALLOC(result) )
		return NULL;
	memcpy(result, global.listdir, listdir_len);
	p = result + listdir_len;
	*(p++) = '/';
	for( i = 0 ; i < count ; i++ ) {
		p = escapedcopy(p, fields[i]);
		*(p++) = '_';
	}
	assert( (size_t)(p - result) == listdir_len + len + 1);
	if( type != NULL )
		memcpy(p, type, type_len + 1);
	else
		*(--p) = '\0';
	return result;
}

struct remote_distribution *remote_distribution_prepare(struct remote_repository *repository, const char *suite, bool ignorerelease, const char *verifyrelease, bool flat, bool *ignorehashes) {
	struct remote_distribution *n, **last;

	last = &repository->distributions;
	while( *last != NULL && strcmp((*last)->suite, suite) != 0 )
		last = &(*last)->next;

	if( *last != NULL ) {
		n = *last;
		assert( strcmp(n->verifyrelease, verifyrelease) == 0 );
		assert( n->ignorerelease == ignorerelease );
		return n;
	}

	n = calloc(1, sizeof(struct remote_distribution));
	if( FAILEDTOALLOC(n) )
		return NULL;
	n->repository = repository;
	n->suite = strdup(suite);
	n->ignorerelease = ignorerelease;
	if( verifyrelease != NULL )
		n->verifyrelease = strdup(verifyrelease);
	else
		n->verifyrelease = NULL;
	memcpy(n->ignorehashes, ignorehashes, sizeof(bool [cs_hashCOUNT]));
	n->flat = flat;
	if( flat )
		n->suite_base_dir = strdup(suite);
	else
		n->suite_base_dir = calc_dirconcat("dists", suite);
	if( FAILEDTOALLOC(n->suite) ||
			(verifyrelease != NULL
			   && FAILEDTOALLOC(n->verifyrelease)) ||
			FAILEDTOALLOC(n->suite_base_dir) ) {
		remote_distribution_free(n);
		return NULL;
	}
	if( !ignorerelease ) {
		n->releasefile = genlistsfilename("Release", 2,
				repository->name, suite, ENDOFARGUMENTS);
		if( FAILEDTOALLOC(n->releasefile) ) {
			remote_distribution_free(n);
			return NULL;
		}
		if( verifyrelease != NULL ) {
			n->releasegpgfile = calc_addsuffix(n->releasefile, "gpg");
			if( FAILEDTOALLOC(n->releasefile) ) {
				remote_distribution_free(n);
				return NULL;
			}
		}
	}
	*last = n;
	return n;
}

static retvalue remote_distribution_metalistqueue(struct remote_distribution *d) {
	struct remote_repository *repository = d->repository;
	retvalue r;

	assert( repository->download != NULL );

	if( d->ignorerelease )
		return RET_NOTHING;

	(void)unlink(d->releasefile);
	r = aptmethod_queueindexfile(repository->download, d->suite_base_dir,
			"Release", d->releasefile, NULL, c_none, NULL);
	if( RET_WAS_ERROR(r) )
		return r;

	if( d->verifyrelease != NULL ) {
		(void)unlink(d->releasegpgfile);
		r = aptmethod_queueindexfile(repository->download,
				d->suite_base_dir, "Release.gpg",
				d->releasegpgfile, NULL, c_none, NULL);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	return RET_OK;
}

retvalue remote_startup(struct aptmethodrun *run) {
	struct remote_repository *rr;
	retvalue r;

	if( interrupted() )
		return RET_ERROR_INTERRUPTED;

	for( rr = repositories ; rr != NULL ; rr = rr->next ) {
		assert( rr->download == NULL );

		r = aptmethod_newmethod(run,
				rr->method, rr->fallback,
				rr->config, &rr->download);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	return RET_OK;
}

static void find_index(const struct strlist *files, struct remote_index *ri) {
	const char *filename = ri->filename_in_release;
	size_t len = strlen(filename);
	int i;
	enum compression c;

	for( i = 0 ; i < files->count ; i++ ) {
		const char *value = files->values[i];

		if( strncmp(value, filename, len) != 0 )
			continue;

		value += len;

		if( *value == '\0' ) {
			ri->ofs[c_none] = i;
			continue;
		}
		if( *value != '.' )
			continue;
		if( strcmp(value, ".diff/Index") == 0 ) {
			ri->diff_ofs = i;
			continue;
		}

		for( c = 0 ; c < c_COUNT ; c++ )
			if( strcmp(value, uncompression_suffix[c]) == 0 ) {
				ri->ofs[c] = i;
				break;
			}
	}
}

static retvalue process_remoterelease(struct remote_distribution *rd) {
	struct remote_repository *rr = rd->repository;
	struct remote_index *ri;
	retvalue r;

	if( rd->releasegpgfile != NULL ) {
		r = signature_check(rd->verifyrelease,
				rd->releasegpgfile,
				rd->releasefile);
		if( r == RET_NOTHING ) {
			fprintf(stderr,
					"Error: No accepted signature found for remote repository %s (%s %s)!\n",
					rr->name,
					rr->method, rd->suite);
			r = RET_ERROR_BADSIG;
		}
		if( RET_WAS_ERROR(r) )
			return r;
	}
	r = release_getchecksums(rd->releasefile, rd->ignorehashes,
			&rd->remotefiles);
	if( RET_WAS_ERROR(r) )
		return r;

	/* Check for our files in there */
	for( ri = rd->indices ; ri != NULL ; ri = ri->next ) {
		find_index(&rd->remotefiles.names, ri);
	}
	// TODO: move checking if not exists at all to here?
	return RET_OK;
}

retvalue remote_preparemetalists(struct aptmethodrun *run, bool nodownload) {
	struct remote_repository *rr;
	struct remote_distribution *rd;
	retvalue r;
	bool tobecontinued;

	if( !nodownload ) {
		for( rr = repositories ; rr != NULL ; rr = rr->next ) {
			for( rd = rr->distributions ; rd != NULL ;
			                              rd = rd->next ) {
				r = remote_distribution_metalistqueue(rd);
				if( RET_WAS_ERROR(r) )
					return r;
			}
		}
		r = aptmethod_download(run, NULL);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	tobecontinued = false;
	for( rr = repositories ; rr != NULL ; rr = rr->next ) {
		for( rd = rr->distributions ; rd != NULL ; rd = rd->next ) {
			if( !rd->ignorerelease ) {
				r = process_remoterelease(rd);
				if( RET_WAS_ERROR(r) )
					return r;
			}
		}
	}
	return RET_OK;
}

bool remote_index_isnew(/*@null@*/const struct remote_index *ri, struct donefile *done) {
	const char *basename;
	struct checksums *checksums;
	bool hashes_missing, improves;

	/* files without uncompressed checksum cannot be tested */
	if( ri->ofs[c_none] < 0 )
		return true;
	/* if not there or the wrong files comes next, then something
	 * has changed and we better reload everything */
	if( !donefile_nextindex(done, &basename, &checksums) )
		return true;
	if( strcmp(basename, ri->cachebasename) != 0 ) {
		checksums_free(checksums);
		return true;
	}
	/* otherwise check if the file checksums match */
	if( !checksums_check(checksums,
			ri->from->remotefiles.checksums[ri->ofs[c_none]],
			&hashes_missing) ) {
		checksums_free(checksums);
		return true;
	}
	if( hashes_missing ) {
		/* if Release has checksums we do not yet know about,
		 * process it to make sure those match as well */
		checksums_free(checksums);
		return true;
	}
	if( !checksums_check(ri->from->remotefiles.checksums[ri->ofs[c_none]],
				checksums, &improves) ) {
		/* this should not happen, but ... */
		checksums_free(checksums);
		return true;
	}
	if( improves ) {
		/* assume this is our file and add the other hashes so they
		 * will show up in the file again the next time.
		 * This is a bit unelegant in mixing stuff, but otherwise this
		 * will cause redownloading when remote adds more hashes.
		 * The only downside of mixing can reject files that have the
		 * same recorded hashes as a previously processed files.
		 * But that is quite inlikely unless on attack, so getting some
		 * hint in that case cannot harm.*/
		(void)checksums_combine(&ri->from->remotefiles.checksums[
				ri->ofs[c_none]], checksums, NULL);
	}
	checksums_free(checksums);
	return false;
}

static inline void remote_index_oldfiles(struct remote_index *ri, /*@null@*/struct cachedlistfile *oldfiles, /*@ouz@*/struct cachedlistfile *old[c_COUNT]) {
	struct cachedlistfile *o;
	size_t l;
	enum compression c;

	for( c = 0 ; c < c_COUNT ; c++ )
		old[c] = NULL;

	l = strlen(ri->cachebasename);
	for( o = oldfiles ; o != NULL ; o = o->next ) {
		if( o->deleted )
			continue;
		if( strncmp(o->basename, ri->cachebasename, l) != 0 )
			continue;
		for( c = 0 ; c < c_COUNT ; c++ )
			if( strcmp(o->basename + l,
			           uncompression_suffix[c]) == 0 ) {
				old[c] = o;
				o->needed = true;
				break;
			}
	}
}

static inline retvalue queueindex(struct remote_distribution *rd, struct remote_index *ri, bool nodownload, /*@null@*/struct cachedlistfile *oldfiles, bool *tobecontinued) {
	struct remote_repository *rr = rd->repository;
	enum compression c;
	retvalue r;
	int ofs;
	struct cachedlistfile *old[c_COUNT];

	if( rd->ignorerelease ) {
		char *toget;

		ri->queued = true;
		if( nodownload )
			return RET_OK;

		/* we do not know what upstream uses, just assume .gz */
		toget = calc_addsuffix(ri->filename_in_release, "gz");
		ri->compression = c_gzip;

		r = aptmethod_queueindexfile(rr->download, rd->suite_base_dir,
				toget, ri->cachefilename, NULL, c_gzip, NULL);
		free(toget);
		return r;
	}

	/* check if this file is still available from an earlier download */
	remote_index_oldfiles(ri, oldfiles, old);
	if( old[c_none] != NULL ) {
		if( ri->ofs[c_none] < 0 ) {
			r = cachedlistfile_delete(old[c_none]);
			/* we'll need to download this there,
			 * so errors to remove are fatal */
			if( RET_WAS_ERROR(r) )
				return r;
			old[c_none] = NULL;
			r = RET_NOTHING;
		} else
			r = checksums_test(old[c_none]->fullfilename,
					rd->remotefiles.checksums[ri->ofs[c_none]],
					&rd->remotefiles.checksums[ri->ofs[c_none]]);
		if( RET_IS_OK(r) ) {
			/* already there, nothing to do to get it... */
			ri->queued = true;
			return r;
		}
		if( r == RET_ERROR_WRONG_MD5 ) {
			// TODO: implement diff
			if( 0 )
				*tobecontinued = true;
			r = cachedlistfile_delete(old[c_none]);
			/* we'll need to download this there,
			 * so errors to remove are fatal */
			if( RET_WAS_ERROR(r) )
				return r;
			old[c_none] = NULL;
			r = RET_NOTHING;
		}
		if( RET_WAS_ERROR(r) )
			return r;
	}

	/* make sure everything old is deleted or check if it can be used */
	for( c = 0 ; c < c_COUNT ; c++ ) {
		if( old[c] == NULL )
			continue;
		if( ri->ofs[c_none] >= 0 ) {
			// TODO: check if it could be used
			// (might be good if it could not be unpacked last
			// time due to no space left on device)
			r = checksums_test(old[c]->fullfilename,
					rd->remotefiles.checksums[ri->ofs[c]],
					&rd->remotefiles.checksums[ri->ofs[c]]);
			if( r == RET_ERROR_WRONG_MD5 )
				r = RET_NOTHING;
			if( RET_WAS_ERROR(r) )
				return r;
			if( RET_IS_OK(r) ) {
				r = uncompress_file(old[c]->fullfilename,
						ri->cachefilename,
						c);
				assert( r != RET_NOTHING );
				if( RET_WAS_ERROR(r) )
					return r;
				if( ri->ofs[c_none] >= 0 ) {
					r = checksums_test(ri->cachefilename,
						rd->remotefiles.checksums[ri->ofs[c_none]],
						&rd->remotefiles.checksums[ri->ofs[c_none]]);
					if( r == RET_ERROR_WRONG_MD5 ) {
						fprintf(stderr,
"Error: File '%s' looked correct according to '%s',\n"
"but after unpacking '%s' looks wrong.\n"
"Something is seriously broken!\n",
							old[c]->fullfilename,
							rd->releasefile,
							ri->cachefilename);
					}
					if( r == RET_NOTHING ) {
						fprintf(stderr, "File '%s' mysteriously vanished!\n", ri->cachefilename);
						r = RET_ERROR_MISSING;
					}
					if( RET_WAS_ERROR(r) )
						return r;
				}
				/* already there, nothing to do to get it... */
				ri->queued = true;
				return RET_OK;
			}
		}
		r = cachedlistfile_delete(old[c]);
		if( RET_WAS_ERROR(r) )
		return r;
		old[c] = NULL;
	}

	/* nothing found, we'll have to download: */

	if( nodownload ) {
		fprintf(stderr, "Error: Missing '%s', try without --nolistsdownload to download it!\n",
				ri->cachefilename);
		return RET_ERROR_MISSING;
	}

	/* assume the more newer the compression the better
	 * (though on low end architectures
	 * the opposite holds, so TODO: make this configurable */

	ri->compression = c_COUNT;
	for( c = 0 ; c < c_COUNT ; c++ ) {
		if( ri->ofs[c] >= 0 && uncompression_supported(c) )
			ri->compression = c;
	}
	if( ri->compression == c_COUNT ) {
		fprintf(stderr,
"Could not find '%s' within '%s'\n",
				ri->filename_in_release, rd->releasefile);
		return RET_ERROR_WRONG_MD5;

	}
	ofs = ri->ofs[ri->compression];

/* as those checksums might be overwritten with completed data,
 * this assumes that the uncompressed checksums for one index is never
 * the compressed checksum for another... */

	ri->queued = true;
	return aptmethod_queueindexfile(rr->download, rd->suite_base_dir,
			rd->remotefiles.names.values[ofs],
			ri->cachefilename,
/* not having this defeats the point, but it only hurts when it is missing
 * now but next update it will be there... */
			(ri->ofs[c_none] < 0)?NULL:
			 &rd->remotefiles.checksums[ri->ofs[c_none]],
			ri->compression,
			(ri->compression == c_none)?NULL:
			rd->remotefiles.checksums[ofs]);
}


static retvalue remote_distribution_listqueue(struct remote_distribution *rd, bool nodownload, struct cachedlistfile *oldfiles, bool *tobecontinued) {
	struct remote_index *ri;
	retvalue r;
	/* check what to get for the requested indicies */
	for( ri = rd->indices ; ri != NULL ; ri = ri->next ) {
		if( ri->queued )
			continue;
		if( !ri->needed ) {
			/* if we do not know anything about it, it cannot have got
			 * marked as old or otherwise as unneeded */
			assert( !rd->ignorerelease );
			continue;
		}
		r = queueindex(rd, ri, nodownload, oldfiles, tobecontinued);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	return RET_OK;
}

retvalue remote_preparelists(struct aptmethodrun *run, bool nodownload) {
	struct remote_repository *rr;
	struct remote_distribution *rd;
	retvalue r;
	bool tobecontinued;
	struct cachedlistfile *oldfiles IFSTUPIDCC(=NULL);

	r = cachedlists_scandir(&oldfiles);
	if( RET_WAS_ERROR(r) )
		return r;
	if( r == RET_NOTHING )
		oldfiles = NULL;

	do {
		tobecontinued = false;
		for( rr = repositories ; rr != NULL ; rr = rr->next ) {
			for( rd = rr->distributions ; rd != NULL
			                            ; rd = rd->next ) {
				r = remote_distribution_listqueue(rd,
						nodownload,
						oldfiles, &tobecontinued);
				if( RET_WAS_ERROR(r) ) {
					cachedlistfile_freelist(oldfiles);
					return r;
				}
			}
		}
		r = aptmethod_download(run, NULL);
		if( RET_WAS_ERROR(r) ) {
			cachedlistfile_freelist(oldfiles);
			return r;
		}
	} while( tobecontinued );

	cachedlistfile_freelist(oldfiles);
	return RET_OK;
}

static struct remote_index *addindex(struct remote_distribution *rd, /*@only@*/char *cachefilename, /*@only@*/char *filename) {
	struct remote_index *ri, **last;
	enum compression c;
	const char *cachebasename;

	if( FAILEDTOALLOC(cachefilename) || FAILEDTOALLOC(filename) )
		return NULL;

	cachebasename = dirs_basename(cachefilename);
	last = &rd->indices;
	while( *last != NULL && strcmp((*last)->cachebasename, cachebasename) != 0 )
		last = &(*last)->next;
	if( *last != NULL ) {
		free(cachefilename); free(filename);
		return *last;
	}

	ri = calloc(1, sizeof(struct remote_index));
	if( FAILEDTOALLOC(ri) ) {
		free(cachefilename); free(filename);
		return NULL;
	}

	*last = ri;
	ri->from = rd;
	ri->cachefilename = cachefilename;
	ri->cachebasename = cachebasename;
	ri->filename_in_release = filename;
	for( c = 0 ; c < c_COUNT ; c++ )
		ri->ofs[c] = -1;
	ri->diff_ofs = -1;
	return ri;
}

struct remote_index *remote_index(struct remote_distribution *rd, const char *architecture, const char *component, const char *packagetype) {
	char *cachefilename, *filename_in_release;

	assert( !rd->flat );
	if( strcmp(packagetype, "deb") == 0 ) {
		filename_in_release = mprintf("%s/binary-%s/Packages",
				component, architecture);
		cachefilename = genlistsfilename("Packages", 5,
				rd->repository->name, rd->suite,
				packagetype, component, architecture,
				ENDOFARGUMENTS);
	} else if( strcmp(packagetype, "udeb") == 0 ) {
		filename_in_release = mprintf(
				"%s/debian-installer/binary-%s/Packages",
				component, architecture);
		cachefilename = genlistsfilename("uPackages", 5,
				rd->repository->name, rd->suite,
				packagetype, component, architecture,
				ENDOFARGUMENTS);
	} else if( strcmp(packagetype, "dsc") == 0 ) {
		filename_in_release = mprintf("%s/source/Sources",
				component);
		cachefilename = genlistsfilename("Sources", 3,
				rd->repository->name, rd->suite,
				component, ENDOFARGUMENTS);
	} else {
		assert( "Unexpected package type" == NULL );
	}
	return addindex(rd, cachefilename, filename_in_release);
}

struct remote_index *remote_flat_index(struct remote_distribution *rd, const char *packagetype) {
	char *cachefilename, *filename_in_release;

	assert( rd->flat );
	if( strcmp(packagetype, "deb") == 0 ) {
		filename_in_release = strdup("Packages");
		cachefilename = genlistsfilename("Packages", 3,
				rd->repository->name, rd->suite,
				packagetype, ENDOFARGUMENTS);
	} else if( strcmp(packagetype, "dsc") == 0 ) {
		filename_in_release = strdup("Sources");
		cachefilename = genlistsfilename("Sources", 2,
				rd->repository->name, rd->suite,
				ENDOFARGUMENTS);
	} else {
		assert( "Unexpected package type" == NULL );
	}
	return addindex(rd, cachefilename, filename_in_release);
}

const char *remote_index_file(const struct remote_index *ri) {
	assert( ri->needed && ri->queued );
	return ri->cachefilename;
}
const char *remote_index_basefile(const struct remote_index *ri) {
	assert( ri->needed && ri->queued );
	return ri->cachebasename;
}

struct aptmethod *remote_aptmethod(const struct remote_distribution *rd) {
	return rd->repository->download;
}

void remote_index_markdone(const struct remote_index *ri, struct markdonefile *done) {
	if( ri->ofs[c_none] < 0 )
		return;
	markdone_index(done, ri->cachebasename,
			ri->from->remotefiles.checksums[ri->ofs[c_none]]);
}
void remote_index_needed(struct remote_index *ri) {
	ri->needed = true;
}