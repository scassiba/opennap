/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.

   $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "opennap.h"
#include "debug.h"

#if 0
static void
compute_soundex (char *d, int dsize, const char *s)
{
    int n = 0;

    /* if it's not big enough to hold one soundex word, quit without
       doing anything */
    if (dsize < 4)
    {
	ASSERT (0);		/* this is a programming error */
	if (dsize > 0)
	    *d = 0;
	return;
    }
    dsize--;			/* save room for the terminatin nul (\0) */

    while (*s && !isalpha ((unsigned char) *s))
	s++;
    if (!*s)
    {
	*d = 0;
	return;
    }

    *d++ = toupper ((unsigned char) *s);
    dsize--;
    s++;

    while (*s && dsize > 0)
    {
	switch (tolower ((unsigned char) *s))
	{
	case 'b':
	case 'p':
	case 'f':
	case 'v':
	    if (n < 3)
	    {
		*d++ = '1';
		dsize--;
		n++;
	    }
	    break;
	case 'c':
	case 's':
	case 'k':
	case 'g':
	case 'j':
	case 'q':
	case 'x':
	case 'z':
	    if (n < 3)
	    {
		*d++ = '2';
		dsize--;
		n++;
	    }
	    break;
	case 'd':
	case 't':
	    if (n < 3)
	    {
		*d++ = '3';
		dsize--;
		n++;
	    }
	    break;
	case 'l':
	    if (n < 3)
	    {
		*d++ = '4';
		dsize--;
		n++;
	    }
	    break;
	case 'm':
	case 'n':
	    if (n < 3)
	    {
		*d++ = '5';
		dsize--;
		n++;
	    }
	    break;
	case 'r':
	    if (n < 3)
	    {
		*d++ = '6';
		dsize--;
		n++;
	    }
	    break;
	default:
	    if (!isalpha ((unsigned char) *s))
	    {
		/* pad short words with 0's */
		while (n < 3 && dsize > 0)
		{
		    *d++ = '0';
		    dsize--;
		    n++;
		}
		n = 0;		/* reset */
		/* skip forward until we find the next word */
		s++;
		while (*s && !isalpha ((unsigned char) *s))
		    s++;
		if (!*s)
		{
		    *d = 0;
		    return;
		}
		if (dsize > 0)
		{
		    *d++ = ',';
		    dsize--;
		    if (dsize > 0)
		    {
			*d++ = toupper ((unsigned char) *s);
			dsize--;
		    }
		}
	    }
	    /* else it's a vowel and we ignore it */
	    break;
	}
	/* skip over duplicate letters */
	while (*(s + 1) == *s)
	    s++;

	/* next letter */
	s++;
    }
    /* pad short words with 0's */
    while (n < 3 && dsize > 0)
    {
	*d++ = '0';
	dsize--;
	n++;
    }
    *d = 0;
}
#endif

static void
fdb_add (HASH * table, char *key, DATUM * d)
{
    FLIST *files;
    LIST *list;

    ASSERT (table != 0);
    ASSERT (key != 0);
    ASSERT (d != 0);
    files = hash_lookup (table, key);
    /* if there is no entry for this particular word, create one now */
    if (!files)
    {
	files = CALLOC (1, sizeof (FLIST));
	if (!files)
	{
	    OUTOFMEMORY ("fdb_add");
	    return;
	}
	files->key = STRDUP (key);
	if (!files->key)
	{
	    OUTOFMEMORY ("fdb_add");
	    FREE (files);
	    return;
	}
	if (hash_add (table, files->key, files))
	{
	    FREE (files->key);
	    FREE (files);
	    return;
	}
    }
    list = CALLOC (1, sizeof (LIST));
    if (!list)
    {
	OUTOFMEMORY ("list");
	if (!files->list)
	    hash_remove (table, files->key);
	return;
    }
    list->data = d;
    files->list = list_append (files->list, list);
    files->count++;
    d->refcount++;
}

/* common code for inserting a file into the various hash tables */
static void
insert_datum (DATUM * info, char *av)
{
    LIST *tokens, *ptr;
    int fsize;

    ASSERT (info != 0);
    ASSERT (av != 0);

    if (!info->user->con->uopt->files)
    {
	/* create the hash table */
	info->user->con->uopt->files =
	    hash_init (257, (hash_destroy) free_datum);
	if (!info->user->con->uopt->files)
	{
	    OUTOFMEMORY ("insert_datum");
	    return;
	}
    }

    /* split the filename into words */
    tokens = tokenize (av);
    ASSERT (tokens != 0);

    /* add this entry to the hash table for this user's files */
    hash_add (info->user->con->uopt->files, info->filename, info);
    info->refcount++;

    /* add this entry to the global file list */
    for (ptr = tokens; ptr; ptr = ptr->next)
	fdb_add (File_Table, ptr->data, info);

    list_free (tokens, 0);

    /* index by md5 hash */
    fdb_add (MD5, info->hash, info);

    fsize = info->size / 1024;
    info->user->shared++;
    info->user->libsize += fsize;
    Num_Gigs += fsize;		/* this is actually kB, not gB */
    Num_Files++;
    Local_Files++;
    info->user->sharing = 1;	/* note that we began sharing */
}

static DATUM *
new_datum (char *filename, char *hash)
{
    DATUM *info = CALLOC (1, sizeof (DATUM));

    if (!info)
    {
	OUTOFMEMORY ("new_datum");
	return 0;
    }
    info->filename = STRDUP (filename);
    if (!info->filename)
    {
	OUTOFMEMORY ("new_datum");
	FREE (info);
	return 0;
    }
    info->hash = STRDUP (hash);
    if (!info->hash)
    {
	OUTOFMEMORY ("new_datum");
	FREE (info->filename);
	FREE (info);
	return 0;
    }
    return info;
}

/* 100 "<filename>" <md5sum> <size> <bitrate> <frequency> <time>
   client adding file to the shared database */
HANDLER (add_file)
{
    char *av[6];
    DATUM *info;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));

    CHECK_USER_CLASS ("add_file");

    ASSERT (validate_user (con->user));

    if (Max_Shared && con->user->shared > Max_Shared)
    {
	log ("add_file(): %s is sharing %d files", con->user->nick,
	     con->user->shared);
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "You may only share %d files", Max_Shared);
	return;
    }

    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 6)
    {
	log ("add_file: wrong number of fields in message");
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "invalid parameter data to add a file");
	return;
    }

    /* make sure this isn't a duplicate */
    if (con->uopt->files && hash_lookup (con->uopt->files, av[0]))
    {
	log ("add_file(): duplicate for %s: %s", con->user->nick, av[0]);
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "duplicate file");
	return;
    }

    /* create the db record for this file */
    if (!(info = new_datum (av[0], av[1])))
	return;
    info->user = con->user;
    info->size = atoi (av[2]);
    info->bitrate = atoi (av[3]);
    info->frequency = atoi (av[4]);
    info->duration = atoi (av[5]);
    info->type = CT_MP3;
    info->valid = 1;

    insert_datum (info, av[0]);
}

char *Content_Types[] = {
    "mp3",			/* not a real type, but what we use for audio/mp3 */
    "audio",
    "video",
    "application",
    "image",
    "text"
};

/* 10300 [ :<user> ] "<filename>" <size> <hash> <content-type> */
HANDLER (share_file)
{
    char *av[4];
    USER *user;
    DATUM *info;
    int i, type;

    (void) len;
    (void) tag;

    ASSERT (validate_connection (con));

    if (pop_user (con, &pkt, &user) != 0)
	return;

    if (Max_Shared && user->shared > Max_Shared)
    {
	log ("add_file(): %s is already sharing %d files", user->nick,
	     user->shared);
	if (con->class == CLASS_USER)
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "You may only share %d files", Max_Shared);
	return;
    }

    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 4)
    {
	log ("share_file(): wrong number of fields");
	if (ISUSER (con))
	    unparsable (con);
	return;
    }

    /* make sure the content-type looks correct */
    type = -1;
    for (i = CT_AUDIO; i < CT_UNKNOWN; i++)
    {
	if (!strcasecmp (Content_Types[i], av[3]))
	{
	    type = i;
	    break;
	}
    }
    if (type == -1)
    {
	log ("share_file(): not a valid type: %s", av[3]);
	if (con->class == CLASS_USER)
	    send_cmd (con, MSG_SERVER_NOSUCH, "%s is not a valid type",
		      av[3]);
	return;
    }

    if (!(info = new_datum (av[0], av[2])))
	return;
    info->user = user;
    info->size = atoi (av[1]);
    info->type = type;
    info->valid = 1;

    insert_datum (info, av[0]);
}

/* 10012 <nick> <shared> <size>
   remote server is notifying us that one of its users is sharing files */
HANDLER (user_sharing)
{
    char *av[3];
    USER *user;
    int deltanum, deltasize;

    (void) len;
    ASSERT (validate_connection (con));
    CHECK_SERVER_CLASS ("user_sharing");
    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 3)
    {
	log ("user_sharing(): wrong number of arguments");
	return;
    }
    user = hash_lookup (Users, av[0]);
    if (!user)
    {
	log ("user_sharing(): no such user %s", av[0]);
	return;
    }
    deltanum = atoi (av[1]) - user->shared;
    Num_Files += deltanum;
    user->shared += deltanum;
    deltasize = atoi (av[2]) - user->libsize;
    Num_Gigs += deltasize;
    user->libsize += deltasize;
    pass_message_args (con, tag, "%s %d %d", user->nick, user->shared,
		       user->libsize);
}

/* 870 "<directory>" "<basename>" <md5> <size> <bitrate> <freq> <duration> [ ... ]
   client command to add multiple files in the same directory */
HANDLER (add_directory)
{
    char *dir, *basename, *md5, *size, *bitrate, *freq, *duration;
    char path[_POSIX_PATH_MAX];
    int pathlen;
    DATUM *info;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    CHECK_USER_CLASS ("add_directory");
    dir = next_arg (&pkt);	/* directory */

    strncpy (path, dir, sizeof (path) - 1);
    pathlen = strlen (path);
    /* ensure filename ends with a trailing slash */
    if (pathlen > 0 && path[pathlen - 1] != '\\' &&
	    (unsigned)pathlen < sizeof (path))
    {
	strcat (path, "\\");
	pathlen++;
    }

    while (pkt)
    {
	if (Max_Shared && con->user->shared > Max_Shared)
	{
	    log ("add_directory(): %s is sharing %d files", con->user->nick,
		 con->user->shared);
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH,
			  "You may only share %d files", Max_Shared);
	    return;
	}
	basename = next_arg (&pkt);
	md5 = next_arg (&pkt);
	size = next_arg (&pkt);
	bitrate = next_arg (&pkt);
	freq = next_arg (&pkt);
	duration = next_arg (&pkt);
	if (!basename || !md5 || !size || !bitrate || !freq || !duration)
	{
	    unparsable (con);
	    return;
	}
	/* make sure this isn't a duplicate */
	strncpy (path + pathlen, basename, sizeof (path) - pathlen - 1);
	if (con->uopt->files && hash_lookup (con->uopt->files, path))
	{
	    log ("add_directory(): duplicate for %s: %s", con->user->nick,
		 path);
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH, "duplicate file");
	    return;
	}

	/* create the db record for this file */
	if (!(info = new_datum (path, md5)))
	    return;
	info->user = con->user;
	info->size = atoi (size);
	info->bitrate = atoi (bitrate);
	info->frequency = atoi (freq);
	info->duration = atoi (duration);
	info->type = CT_MP3;
	info->valid = 1;

	insert_datum (info, path);
    }
}
