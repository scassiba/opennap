/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <unistd.h>
#include <mysql.h>
#include <string.h>
#include <stdio.h>
#include "opennap.h"
#include "debug.h"

extern MYSQL *Db;

char *Levels[LEVEL_ELITE+1] = {
    "Leech",
    "User",
    "Moderator",
    "Admin",
    "Elite"
};

static void
synch_user (USER *user, CONNECTION *con)
{
    int i;
    ASSERT (validate_connection (con));
    ASSERT (validate_user (user));

    /* we should never tell a peer server about a user that is behind
       them */
    ASSERT (user->con != con);
    if (user->con == con)
    {
	/* this really shouldnt happen! */
	ASSERT (0);
	return;
    }

    /* send a login message for this user */
    send_cmd (con, MSG_CLIENT_LOGIN, "%s - %d \"%s\" %d",
	    user->nick, user->port, user->clientinfo, user->speed);

    /* send the user's host */
    send_cmd (con, MSG_SERVER_USER_IP, "%s %lu %hu %s", user->nick,
	user->host, user->conport, user->server);

    /* update the user's level */
    if (user->level != LEVEL_USER)
    {
	send_cmd (con, MSG_CLIENT_SETUSERLEVEL, ":%s %s %s",
	    Server_Name, user->nick, Levels[user->level]);
    }

    /* send the channels this user is listening on */
    for (i = 0; i < user->numchannels; i++)
    {
	send_cmd (con, MSG_CLIENT_JOIN, ":%s %s",
		user->nick, user->channels[i]->name);
    }
}

void
synch_server (CONNECTION *con)
{
#ifndef MINIDB
    MYSQL_RES *result;
    MYSQL_ROW row;
    int n;
#endif
    int i;

    ASSERT (validate_connection (con));

    log ("synch_server(): syncing user list");

    /* send our peer server a list of all users we know about */
    hash_foreach (Users, (hash_callback_t) synch_user, (void *) con);

    log ("synch_server(): done");

    /* now dump the contents of the library db and send in bulk */
    log ("sync_server(): syncing db");

#if MINIDB
    ASSERT (File_Table_Count == Num_Files);
    for (i = 0; i < File_Table_Count; i++)
    {
	if (!strcasecmp (File_Table[i]->type, "audio/mp3"))
	    send_cmd (con, MSG_CLIENT_ADD_FILE, ":%s \"%s\" %s %d %d %d %d",
		    File_Table[i]->user->nick, File_Table[i]->filename,
		    File_Table[i]->hash, File_Table[i]->size,
		    File_Table[i]->bitrate, File_Table[i]->samplerate,
		    File_Table[i]->length);
	else
	    send_cmd (con, MSG_CLIENT_SHARE_FILE, ":%s \"%s\" %s %d %s",
		    File_Table[i]->user->nick, File_Table[i]->filename,
		    File_Table[i]->hash, File_Table[i]->size,
		    File_Table[i]->type);
    }
#else
    if (mysql_query (Db, "SELECT * FROM library") != 0)
    {
	sql_error ("synch_user", "SELECT * FROM library");
	return;
    }
    result = mysql_store_result (Db);
    n = mysql_num_rows (result);
    ASSERT (n == Num_Files);
    for (i = 0; i < n; i++)
    {
	row = mysql_fetch_row (result);
	if (!strcasecmp (row[IDX_TYPE], "audio/mp3"))
	    send_cmd (con, MSG_CLIENT_ADD_FILE, ":%s \"%s\" %s %s %s %s %s",
		    row[IDX_NICK], row[IDX_FILENAME], row[IDX_MD5],
		    row[IDX_SIZE], row[IDX_BITRATE], row[IDX_FREQ],
		    row[IDX_LENGTH]);
	else
	    send_cmd (con, MSG_CLIENT_SHARE_FILE, ":%s \"%s\" %s %s %s",
		    row[IDX_NICK], row[IDX_FILENAME], row[IDX_SIZE],
		    row[IDX_MD5], row[IDX_TYPE]);
    }
    mysql_free_result (result);
#endif /* MINIDB */
    log ("synch_server(): done");
}
