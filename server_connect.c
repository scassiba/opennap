/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.

   $Id$ */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mysql.h>
#include "opennap.h"
#include "debug.h"

extern MYSQL *Db;

static void
try_connect (char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sin;
    unsigned long ip;
    int f;
    CONNECTION *cli;

    log ("try_connect(): attempting to establish server connection to %s:%d",
	    host, port);

    /* attempt a connection */
    memset (&sin, 0, sizeof (sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons (port);
    he = gethostbyname(host);
    if (!he)
    {
	log ("try_connect(): can't find ip for host %s", host);
	return;
    }
    memcpy (&ip, &he->h_addr[0], he->h_length);
    endhostent ();
    sin.sin_addr.s_addr = ip;

    f=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(f<0)
    {
	log("server_connect(): socket: %s", strerror (errno));
	return;
    }
    if (connect(f,(struct sockaddr*)&sin,sizeof(sin))<0)
    {
	log("server_connect(): connect: %s", strerror (errno));
	close (f);
	return;
    }

    /* set the socket to be nonblocking */
    if (fcntl (f, F_SETFL, O_NONBLOCK) != 0)
    {
	log("server_connect(): fcntl: %s", strerror (errno));
	close (f);
	return;
    }

    cli = new_connection ();
    cli->class = CLASS_UNKNOWN; /* not authenticated yet */
    cli->fd = f;
    cli->host = STRDUP (host);
    cli->nonce = generate_nonce ();
    cli->ip = ip;

    add_client (cli);

    /* send the login request */
    ASSERT (Server_Name != 0);
    send_cmd (cli, MSG_SERVER_LOGIN, "%s %s", Server_Name, cli->nonce);

    /* we handle the response to the login request in the main event loop so
       that we don't block while waiting for th reply.  if the server does
       not accept our connection it will just drop it and we will detect
       it by the normal means that every other connection is checked */
}

/* process client request to link another server */
/* 10100 [ :<user> ] <server-name> <port> [ <remote_server> ] */
HANDLER (server_connect)
{
    USER *user;
    char *fields[3];
    int argc;

    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &user) != 0)
	return;
    ASSERT (validate_user (user));

    if (user->level < LEVEL_ADMIN)
    {
	log ("server_connect(): user %s tried to connect to %s", pkt);
	if (con->class == CLASS_USER)
	    permission_denied (con);
	return; /* no privilege */
    }

    argc = split_line (fields, sizeof (fields) / sizeof (char *), pkt);
    if (argc < 2)
    {
	log ("server_connect(): too few fields");
	return;
    }

    if (argc == 2 || (argc == 3 && !strcasecmp (fields[2], Server_Name)))
    {
	try_connect (fields[0], atoi (fields[1]));
    }
    else if (con->class == CLASS_USER)
    {
	/* pass the message on the target server */
	ASSERT (argc == 3);
	pass_message_args (con, MSG_CLIENT_CONNECT, ":%s %s %s %s",
	    user->nick, fields[0], fields[1], fields[2]);
    }

    notify_mods ("%s requested server link from %s to %s:%s",
	user->nick, argc == 3 ? fields[2] : Server_Name, fields[0], fields[1]);
}

/* 10101 [ :<nick> ] <server> <reason> */
HANDLER (server_disconnect)
{
    USER *user;
    char *reason;
    int i;
    CONNECTION *serv;

    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &user) != 0)
	return;
    ASSERT (validate_user (user));
    if (user->level < LEVEL_ADMIN)
    {
	if (con->class == CLASS_USER)
	    permission_denied (con);
	return;
    }
    reason = strchr (pkt, ' ');
    if (reason)
	*reason++ = 0;
    for (i = 0; i < Num_Servers; i++)
	if (!strcasecmp (Servers[i]->host, pkt))
	    break;
    if (i == Num_Servers)
    {
	if (con->class == CLASS_USER)
	    pass_message_args (con, MSG_CLIENT_DISCONNECT, ":%s %s %s",
		user->nick, pkt, reason ? reason : "disconnect");
	return;
    }
    notify_mods ("%s disconnected server %s: %s", user->nick, pkt,
	reason ? reason : "");
    serv = Servers[i];
    Servers = array_remove (Servers, &Num_Servers, Servers[i]);
    remove_connection (serv);
}

/* 10110 [ :<user> ] <server> <reason> */
HANDLER (kill_server)
{
    USER *user;
    char *reason;

    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &user) != 0)
	return;
    ASSERT (validate_user (user));
    if (user->level < LEVEL_ELITE)
    {
	if (con->class == CLASS_USER)
	    permission_denied (con);
	return;
    }
    reason = strchr (pkt, ' ');
    if (reason)
	*reason++ = 0;

    if (con->class == CLASS_USER)
    	pass_message_args (con, MSG_CLIENT_KILL_SERVER, ":%s %s %s",
	    user->nick, pkt, reason ? reason : "");
    notify_mods ("%s killed server %s: %s", user->nick, pkt,
	reason ? reason : "");

    if (!strcasecmp (pkt, Server_Name))
	SigCaught = 1; /* this causes the main event loop to exit */
}

/* 10111 <server> [ <reason> ] */
HANDLER (remove_server)
{
    char *reason;

    ASSERT (validate_connection (con));
    /* TODO: should we be able to remove any server, or just from the local
       server? */
    CHECK_USER_CLASS ("remove_server");
    ASSERT (validate_user (con->user));
    if (con->user->level < LEVEL_ELITE)
    {
	permission_denied (con);
	return;
    }
    reason = strchr (pkt, ' ');
    if (reason)
	*reason++ = 0;
    snprintf (Buf, sizeof (Buf), "DELETE FROM servers WHERE server = '%s'",
	pkt);
    if (mysql_query (Db, Buf) != 0)
    {
	sql_error ("remove_server", Buf);
	send_cmd (con, MSG_SERVER_NOSUCH,
	    "error removing %s from SQL database", pkt);
    }
    else
    {
	notify_mods ("%s removed server %s from database: %s",
	    con->user->nick, pkt, reason ? reason : "");
    }
}

/* 801 [ :<user> ] [ <server> ] */
HANDLER (server_version)
{
    USER *user;

    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &user) != 0)
	return;
    ASSERT (validate_user (user));
    if (user->level < LEVEL_MODERATOR)
    {
	if (con->class == CLASS_USER)
	    permission_denied (con);
	return;
    }
    if (!*pkt || !strcmp (Server_Name, pkt))
    {
	if (user->con)
	{
	    send_cmd (user->con, MSG_SERVER_NOSUCH, "--");
	    send_cmd (user->con, MSG_SERVER_NOSUCH, "%s %s", PACKAGE, VERSION);
	    send_cmd (user->con, MSG_SERVER_NOSUCH, "--");
	}
	else
	{
	    ASSERT (0);
	    log ("server_version(): haven't implemented sending error messages to remote users");
	}
    }
}
