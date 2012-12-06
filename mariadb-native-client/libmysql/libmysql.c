/************************************************************************************
    Copyright (C) 2000, 2011 MySQL AB & MySQL Finland AB & TCX DataKonsult AB,
                 Monty Program AB
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc., 
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA

   Part of this code includes code from the PHP project which
   is freely available from http://www.php.net
*************************************************************************************/

#include <my_global.h>

#include <my_sys.h>
#include <mysys_err.h>
#include <m_string.h>
#include <m_ctype.h>
#include "mysql.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "errmsg.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#if !defined(MSDOS) && !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_SELECT_H
#  include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#endif
#ifdef HAVE_SYS_UN_H
#  include <sys/un.h>
#endif
#if defined(THREAD) && !defined(_WIN32)
#include <my_pthread.h> /* because of signal() */
#endif
#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif
#include <sha1.h>
#include <violite.h>
#ifdef HAVE_OPENSSL
#include <my_secure.h>
#endif

static my_bool mysql_client_init=0;
extern my_bool  my_init_done;
extern my_bool  mysql_ps_subsystem_initialized;
extern my_bool mysql_handle_local_infile(MYSQL *mysql, const char *filename);
extern const CHARSET_INFO * mysql_find_charset_nr(uint charsetnr);
extern const CHARSET_INFO * mysql_find_charset_name(const char * const name);
extern int run_plugin_auth(MYSQL *mysql, char *data, uint data_len,
                           const char *data_plugin, const char *db);
uint mysql_port=0;
my_string mysql_unix_port=0;

#ifdef _WIN32
#define CONNECT_TIMEOUT 20
#else
#define CONNECT_TIMEOUT 0
#endif

#if defined(MSDOS) || defined(_WIN32)
// socket_errno is defined in my_global.h for all platforms
#define perror(A)
#else
#include <errno.h>
#define SOCKET_ERROR -1
#endif /* _WIN32 */

#include <mysql/client_plugin.h>

#define native_password_plugin_name "mysql_native_password"
const char     *unknown_sqlstate= "HY000";

MYSQL_DATA *read_rows (MYSQL *mysql,MYSQL_FIELD *fields,
                       uint field_count);
static int read_one_row(MYSQL *mysql,uint fields,MYSQL_ROW row,
                        ulong *lengths);
static void end_server(MYSQL *mysql);
static void mysql_close_memory(MYSQL *mysql);
void read_user_name(char *name);
static void append_wild(char *to,char *end,const char *wild);
static my_bool mysql_reconnect(MYSQL *mysql);
static sig_handler pipe_sig_handler(int sig);
static int cli_report_progress(MYSQL *mysql, uchar *packet, uint length);

extern int mysql_client_plugin_init();
extern void mysql_client_plugin_deinit();

/*
  Let the user specify that we don't want SIGPIPE;  This doesn't however work
  with threaded applications as we can have multiple read in progress.
*/

#if !defined(_WIN32) && defined(SIGPIPE) && !defined(THREAD)
#define init_sigpipe_variables  sig_return old_signal_handler=(sig_return) 0;
#define set_sigpipe(mysql)     if ((mysql)->client_flag & CLIENT_IGNORE_SIGPIPE) old_signal_handler=signal(SIGPIPE,pipe_sig_handler)
#define reset_sigpipe(mysql) if ((mysql)->client_flag & CLIENT_IGNORE_SIGPIPE) signal(SIGPIPE,old_signal_handler);
#else
#define init_sigpipe_variables
#define set_sigpipe(mysql)
#define reset_sigpipe(mysql)
#endif


/****************************************************************************
* A modified version of connect().  connect2() allows you to specify
* a timeout value, in seconds, that we should wait until we
* derermine we can't connect to a particular host.  If timeout is 0,
* connect2() will behave exactly like connect().
*
* Base version coded by Steve Bernacki, Jr. <steve@navinet.net>
*****************************************************************************/

static int connect2(my_socket s, const struct sockaddr *name, uint namelen,
		    uint timeout)
{
#if defined(_WIN32) || defined(OS2)
  return connect(s, (struct sockaddr*) name, namelen);
#else
  int flags, res, s_err;
  socklen_t s_err_size = sizeof(uint);
  fd_set sfds;
  struct timeval tv;
  time_t start_time, now_time;

  /*
    If they passed us a timeout of zero, we should behave
    exactly like the normal connect() call does.
  */

  if (timeout == 0)
    return connect(s, (struct sockaddr*) name, namelen);

  flags = fcntl(s, F_GETFL, 0);		  /* Set socket to not block */
#ifdef O_NONBLOCK
  fcntl(s, F_SETFL, flags | O_NONBLOCK);  /* and save the flags..  */
#endif

  res = connect(s, (struct sockaddr*) name, namelen);
  s_err = errno;			/* Save the error... */
  fcntl(s, F_SETFL, flags);
  if ((res != 0) && (s_err != EINPROGRESS))
  {
    errno = s_err;			/* Restore it */
    return(-1);
  }
  if (res == 0)				/* Connected quickly! */
    return(0);

  /*
    Otherwise, our connection is "in progress."  We can use
    the select() call to wait up to a specified period of time
    for the connection to suceed.  If select() returns 0
    (after waiting howevermany seconds), our socket never became
    writable (host is probably unreachable.)  Otherwise, if
    select() returns 1, then one of two conditions exist:
   
    1. An error occured.  We use getsockopt() to check for this.
    2. The connection was set up sucessfully: getsockopt() will
    return 0 as an error.
   
    Thanks goes to Andrew Gierth <andrew@erlenstar.demon.co.uk>
    who posted this method of timing out a connect() in
    comp.unix.programmer on August 15th, 1997.
  */

  FD_ZERO(&sfds);
  FD_SET(s, &sfds);
  /*
    select could be interrupted by a signal, and if it is, 
    the timeout should be adjusted and the select restarted
    to work around OSes that don't restart select and 
    implementations of select that don't adjust tv upon
    failure to reflect the time remaining
   */
  start_time = time(NULL);
  for (;;)
  {
    tv.tv_sec = (long) timeout;
    tv.tv_usec = 0;
#if defined(HPUX) && defined(THREAD)
    if ((res = select(s+1, NULL, (int*) &sfds, NULL, &tv)) > 0)
      break;
#else
    if ((res = select(s+1, NULL, &sfds, NULL, &tv)) > 0)
      break;
#endif
    if (res == 0)					/* timeout */
      return -1;
    now_time=time(NULL);
    timeout-= (uint) (now_time - start_time);
    if (errno != EINTR || (int) timeout <= 0)
      return -1;
  }

  /*
    select() returned something more interesting than zero, let's
    see if we have any errors.  If the next two statements pass,
    we've got an open socket!
  */

  s_err=0;
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*) &s_err, &s_err_size) != 0)
    return(-1);

  if (s_err)
  {						/* getsockopt could succeed */
    errno = s_err;
    return(-1);					/* but return an error... */
  }
  return (0);					/* ok */

#endif
}

/*
** Create a named pipe connection
*/

#ifdef _WIN32

HANDLE create_named_pipe(NET *net, uint connect_timeout, char **arg_host,
			 char **arg_unix_socket)
{
  HANDLE hPipe=INVALID_HANDLE_VALUE;
  char szPipeName [ 257 ];
  DWORD dwMode;
  int i;
  my_bool testing_named_pipes=0;
  char *host= *arg_host, *unix_socket= *arg_unix_socket;

  if ( ! unix_socket || (unix_socket)[0] == 0x00)
    unix_socket = mysql_unix_port;
  if (!host || !strcmp(host,LOCAL_HOST))
    host=LOCAL_HOST_NAMEDPIPE;

  sprintf( szPipeName, "\\\\%s\\pipe\\%s", host, unix_socket);
  DBUG_PRINT("info",("Server name: '%s'.  Named Pipe: %s",
		     host, unix_socket));

  for (i=0 ; i < 100 ; i++)			/* Don't retry forever */
  {
    if ((hPipe = CreateFile(szPipeName,
			    GENERIC_READ | GENERIC_WRITE,
			    0,
			    NULL,
			    OPEN_EXISTING,
			    0,
			    NULL )) != INVALID_HANDLE_VALUE)
      break;
    if (GetLastError() != ERROR_PIPE_BUSY)
    {
      net->last_errno=CR_NAMEDPIPEOPEN_ERROR;
      sprintf(net->last_error,ER(net->last_errno),host, unix_socket,
	      (ulong) GetLastError());
      return INVALID_HANDLE_VALUE;
    }
    /* wait for for an other instance */
    if (! WaitNamedPipe(szPipeName, connect_timeout*1000) )
    {
      net->last_errno=CR_NAMEDPIPEWAIT_ERROR;
      sprintf(net->last_error,ER(net->last_errno),host, unix_socket,
	      (ulong) GetLastError());
      return INVALID_HANDLE_VALUE;
    }
  }
  if (hPipe == INVALID_HANDLE_VALUE)
  {
    net->last_errno=CR_NAMEDPIPEOPEN_ERROR;
    sprintf(net->last_error,ER(net->last_errno),host, unix_socket,
	    (ulong) GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  dwMode = PIPE_READMODE_BYTE | PIPE_WAIT;
  if ( !SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL) )
  {
    CloseHandle( hPipe );
    net->last_errno=CR_NAMEDPIPESETSTATE_ERROR;
    sprintf(net->last_error,ER(net->last_errno),host, unix_socket,
	    (ulong) GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  *arg_host=host ; *arg_unix_socket=unix_socket;	/* connect arg */
  return (hPipe);
}
#endif

/* net_get_error */
void net_get_error(char *buf, size_t buf_len,
       char *error, size_t error_len,
       unsigned int *error_no,
       char *sqlstate)
{
  char *p= buf;
  size_t error_msg_len= 0;

  if (buf_len > 2)
  {
    *error_no= uint2korr(p);
    p+= 2;

    /* since 4.1 sqlstate is following */
    if (*p == '#')
    {
      memcpy(sqlstate, ++p, SQLSTATE_LENGTH);
      p+= SQLSTATE_LENGTH;  
    }
    error_msg_len= buf_len - (p - buf);
    error_msg_len= MIN(error_msg_len, error_len - 1);
    memcpy(error, p, error_msg_len);
  }
  else
  {
    *error_no= CR_UNKNOWN_ERROR;
    memcpy(sqlstate, unknown_sqlstate, SQLSTATE_LENGTH);
  }
}


/*****************************************************************************
** read a packet from server. Give error message if socket was down
** or packet is an error message
*****************************************************************************/

ulong
net_safe_read(MYSQL *mysql)
{
  NET *net= &mysql->net;
  ulong len=0;
  init_sigpipe_variables

restart:
  /* Don't give sigpipe errors if the client doesn't want them */
  set_sigpipe(mysql);
  if (net->vio != 0)
    len=my_net_read(net);
  reset_sigpipe(mysql);

  if (len == packet_error || len == 0)
  {
    end_server(mysql);
    my_set_error(mysql, net->last_errno == ER_NET_PACKET_TOO_LARGE ?
		     CR_NET_PACKET_TOO_LARGE:
		     CR_SERVER_LOST,
                     SQLSTATE_UNKNOWN, 0);
    return(packet_error);
  }
  if (net->read_pos[0] == 255)
  {
    if (len > 3)
    {
      char *pos=(char*) net->read_pos+1;
      uint last_errno=uint2korr(pos);
      pos+=2;
      len-=2;

      if (last_errno== 65535 &&
          (mysql->server_capabilities & CLIENT_PROGRESS))
      {
        if (cli_report_progress(mysql, (uchar *)pos, (uint) (len-1)))
        {
          /* Wrong packet */
          my_set_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate, 0);
          return (packet_error);
        }
        goto restart;
      }
      net->last_errno= last_errno;
      if (pos[0]== '#')
      {
        strmake(net->sqlstate, pos+1, SQLSTATE_LENGTH);
        pos+= SQLSTATE_LENGTH + 1;
      }
      else
      {
        strmov(net->sqlstate, SQLSTATE_UNKNOWN);
      }
      (void) strmake(net->last_error,(char*) pos,
		     min(len,sizeof(net->last_error)-1));
    }
    else
    {
      my_set_error(mysql, CR_UNKNOWN_ERROR, SQLSTATE_UNKNOWN, 0);
    }

    mysql->server_status= ~SERVER_MORE_RESULTS_EXIST;

    DBUG_PRINT("error",("Got error: %d (%s)", net->last_errno,
			net->last_error));
    return(packet_error);
  }
  return len;
}

/*
  Report progress to the client

  RETURN VALUES
    0  ok
    1  error
*/
static int cli_report_progress(MYSQL *mysql, uchar *packet, uint length)
{
  uint stage, max_stage, proc_length;
  double progress;
  uchar *start= packet;

  if (length < 5)
    return 1;                         /* Wrong packet */

  if (!(mysql->options.extension && mysql->options.extension->report_progress))
    return 0;                         /* No callback, ignore packet */

  packet++;                           /* Ignore number of strings */
  stage= (uint) *packet++;
  max_stage= (uint) *packet++;
  progress= uint3korr(packet)/1000.0;
  packet+= 3;
  proc_length= net_field_length(&packet);
  if (packet + proc_length > start + length)
    return 1;                         /* Wrong packet */
  (*mysql->options.extension->report_progress)(mysql, stage, max_stage,
                                               progress, (char*) packet,
                                               proc_length);
  return 0;
}

/* Get the length of next field. Change parameter to point at fieldstart */
ulong
net_field_length(uchar **packet)
{
  reg1 uchar *pos= *packet;
  if (*pos < 251)
  {
    (*packet)++;
    return (ulong) *pos;
  }
  if (*pos == 251)
  {
    (*packet)++;
    return NULL_LENGTH;
  }
  if (*pos == 252)
  {
    (*packet)+=3;
    return (ulong) uint2korr(pos+1);
  }
  if (*pos == 253)
  {
    (*packet)+=4;
    return (ulong) uint3korr(pos+1);
  }
  (*packet)+=9;					/* Must be 254 when here */
  return (ulong) uint4korr(pos+1);
}

/* Same as above, but returns ulonglong values */

static my_ulonglong
net_field_length_ll(uchar **packet)
{
  reg1 uchar *pos= *packet;
  if (*pos < 251)
  {
    (*packet)++;
    return (my_ulonglong) *pos;
  }
  if (*pos == 251)
  {
    (*packet)++;
    return (my_ulonglong) NULL_LENGTH;
  }
  if (*pos == 252)
  {
    (*packet)+=3;
    return (my_ulonglong) uint2korr(pos+1);
  }
  if (*pos == 253)
  {
    (*packet)+=4;
    return (my_ulonglong) uint3korr(pos+1);
  }
  (*packet)+=9;					/* Must be 254 when here */
#ifdef NO_CLIENT_LONGLONG
  return (my_ulonglong) uint4korr(pos+1);
#else
  return (my_ulonglong) uint8korr(pos+1);
#endif
}


void free_rows(MYSQL_DATA *cur)
{
  if (cur)
  {
    free_root(&cur->alloc,MYF(0));
    my_free((gptr) cur,MYF(0));
  }
}


int
simple_command(MYSQL *mysql,enum enum_server_command command, const char *arg,
	       size_t length, my_bool skipp_check)
{
  NET *net= &mysql->net;
  int result= -1;
  init_sigpipe_variables

  DBUG_ENTER("simple_command");

  DBUG_PRINT("info", ("server_command: %d  packet_size: %u", command, length));

  /* Don't give sigpipe errors if the client doesn't want them */
  set_sigpipe(mysql);
  if (mysql->net.vio == 0)
  {						/* Do reconnect if possible */
    if (mysql_reconnect(mysql))
    {
      DBUG_PRINT("info", ("reconnect failed"));
      DBUG_RETURN(1);
    }
  }
  if (mysql->status != MYSQL_STATUS_READY ||
      mysql->server_status & SERVER_MORE_RESULTS_EXIST)
  {
    SET_CLIENT_ERROR(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate, 0);
    goto end;
  }

  mysql->net.last_error[0]=0;
  mysql->net.last_errno=0;
  strmov(mysql->net.sqlstate, "00000");

  mysql->info=0;
  mysql->affected_rows= ~(my_ulonglong) 0;
  net_clear(net);			/* Clear receive buffer */
  if (!arg)
    arg="";

  if (net_write_command(net,(uchar) command,arg,
			length ? length : (ulong) strlen(arg)))
  {
    DBUG_PRINT("error",("Can't send command to server. Error: %d",socket_errno));
    if (net->last_errno == ER_NET_PACKET_TOO_LARGE)
    {
      my_set_error(mysql, CR_NET_PACKET_TOO_LARGE, SQLSTATE_UNKNOWN, 0);
      goto end;
    }
    end_server(mysql);
    if (mysql_reconnect(mysql))
      goto end;
    if (net_write_command(net,(uchar) command,arg,
			  length ? length : (ulong) strlen(arg)))
    {
      my_set_error(mysql, CR_SERVER_GONE_ERROR, SQLSTATE_UNKNOWN, 0);
      goto end;
    }
  }
  result=0;
  if (!skipp_check) {
    result= ((mysql->packet_length=net_safe_read(mysql)) == packet_error ?
	     -1 : 0);
    DBUG_PRINT("info", ("packet_length=%llu", mysql->packet_length));
  }
 end:
  reset_sigpipe(mysql);
  DBUG_RETURN(result);
}


static void free_old_query(MYSQL *mysql)
{
  DBUG_ENTER("free_old_query");
  if (mysql->fields)
    free_root(&mysql->field_alloc,MYF(0));
  init_alloc_root(&mysql->field_alloc,8192,0);	/* Assume rowlength < 8192 */
  mysql->fields=0;
  mysql->field_count=0;				/* For API */
  DBUG_VOID_RETURN;
}

#if defined(HAVE_GETPWUID) && defined(NO_GETPWUID_DECL)
struct passwd *getpwuid(uid_t);
char* getlogin(void);
#endif

#if !defined(MSDOS) && ! defined(VMS) && !defined(_WIN32) && !defined(OS2)
void read_user_name(char *name)
{
  DBUG_ENTER("read_user_name");
  if (geteuid() == 0)
    (void) strmov(name,"root");		/* allow use of surun */
  else
  {
#ifdef HAVE_GETPWUID
    struct passwd *skr;
    const char *str;
    if ((str=getlogin()) == NULL)
    {
      if ((skr=getpwuid(geteuid())) != NULL)
	str=skr->pw_name;
      else if (!(str=getenv("USER")) && !(str=getenv("LOGNAME")) &&
	       !(str=getenv("LOGIN")))
	str="UNKNOWN_USER";
    }
    (void) strmake(name,str,USERNAME_LENGTH);
#elif HAVE_CUSERID
    (void) cuserid(name);
#else
    strmov(name,"UNKNOWN_USER");
#endif
  }
  DBUG_VOID_RETURN;
}

#else /* If MSDOS || VMS */

void read_user_name(char *name)
{
  char *str=getenv("USERNAME");		/* ODBC will send user variable */
  strmake(name,str ? str : "ODBC", USERNAME_LENGTH);
}

#endif

#ifdef _WIN32
static my_bool is_NT(void)
{
  char *os=getenv("OS");
  return (os && !strcmp(os, "Windows_NT")) ? 1 : 0;
}
#endif

/*
** Expand wildcard to a sql string
*/

static void
append_wild(char *to, char *end, const char *wild)
{
  end-=5;					/* Some extra */
  if (wild && wild[0])
  {
    to=strmov(to," like '");
    while (*wild && to < end)
    {
      if (*wild == '\\' || *wild == '\'')
	*to++='\\';
      *to++= *wild++;
    }
    if (*wild)					/* Too small buffer */
      *to++='%';				/* Nicer this way */
    to[0]='\'';
    to[1]=0;
  }
}



/**************************************************************************
** Init debugging if MYSQL_DEBUG environment variable is found
**************************************************************************/
void STDCALL mysql_debug_end()
{
  if (_db_on_)
  {
    DEBUGGER_OFF;
    DBUG_POP();
  }
}

void STDCALL
mysql_debug(const char *debug __attribute__((unused)))
{
#ifndef DBUG_OFF
  char	*env;
  if (_db_on_)
    return;					/* Already using debugging */
  if (debug)
  {
    DEBUGGER_ON;
    DBUG_PUSH(debug);
  }
  else if ((env = getenv("MYSQL_DEBUG")))
  {
    DEBUGGER_ON;
    DBUG_PUSH(env);
#if !defined(_WINVER) && !defined(WINVER)
    puts("\n-------------------------------------------------------");
    puts("MYSQL_DEBUG found. libmysql started with the following:");
    puts(env);
    puts("-------------------------------------------------------\n");
#else
    {
      char buff[80];
      strmov(strmov(buff,"libmysql: "),env);
      MessageBox((HWND) 0,"Debugging variable MYSQL_DEBUG used",buff,MB_OK);
    }
#endif
  }
#endif
}


/**************************************************************************
** Close the server connection if we get a SIGPIPE
   ARGSUSED
**************************************************************************/

static sig_handler
pipe_sig_handler(int sig __attribute__((unused)))
{
  DBUG_PRINT("info",("Hit by signal %d",sig));
#ifdef DONT_REMEMBER_SIGNAL
  (void) signal(SIGPIPE,pipe_sig_handler);
#endif
}


/**************************************************************************
** Shut down connection
**************************************************************************/

static void
end_server(MYSQL *mysql)
{
  DBUG_ENTER("end_server");
  if (mysql->net.vio != 0)
  {
    init_sigpipe_variables
    set_sigpipe(mysql);
    vio_delete(mysql->net.vio);
    reset_sigpipe(mysql);
    mysql->net.vio= 0;    /* Marker */
  }
  net_end(&mysql->net);
  free_old_query(mysql);
  DBUG_VOID_RETURN;
}


void STDCALL
mysql_free_result(MYSQL_RES *result)
{
  DBUG_ENTER("mysql_free_result");
  DBUG_PRINT("enter",("mysql_res: %lx",result));
  if (result)
  {
    if (result->handle && result->handle->status == MYSQL_STATUS_USE_RESULT)
    {
      ulong pkt_len;
      DBUG_PRINT("warning",("Not all rows in set were read; Ignoring rows"));

      do {
        pkt_len= net_safe_read(result->handle);
        if (pkt_len == packet_error)
          break;
      } while (pkt_len > 8 || result->handle->net.read_pos[0] != 254);
      result->handle->status=MYSQL_STATUS_READY;
    }
    free_rows(result->data);
    if (result->fields)
      free_root(&result->field_alloc,MYF(0));
    if (result->row)
      my_free((gptr) result->row,MYF(0));
    my_free((gptr) result,MYF(0));
  }
  DBUG_VOID_RETURN;
}


/****************************************************************************
** Get options from my.cnf
****************************************************************************/

static const char *default_options[]=
{
  "port","socket","compress","password","pipe", "timeout", "user",
  "init-command", "host", "database", "debug", "return-found-rows",
  "ssl-key" ,"ssl-cert" ,"ssl-ca" ,"ssl-capath",
  "character-sets-dir", "default-character-set", "interactive-timeout",
  "connect-timeout", "local-infile", "disable-local-infile",
  "ssl-cipher", "max-allowed-packet", "protocol", "shared-memory-base-name",
  "multi-results", "multi-statements", "multi-queries", "secure-auth",
  "report-data-truncation", "plugin-dir", "default-auth",
  NULL
};

enum option_val
{
  OPT_port=1, OPT_socket, OPT_compress, OPT_password, OPT_pipe,
  OPT_timeout, OPT_user, OPT_init_command, OPT_host, OPT_database,
  OPT_debug, OPT_return_found_rows, OPT_ssl_key, OPT_ssl_cert,
  OPT_ssl_ca, OPT_ssl_capath, OPT_charset_dir, 
  OPT_charset_name, OPT_interactive_timeout,
  OPT_connect_timeout, OPT_local_infile, OPT_disable_local_infile,
  OPT_ssl_cipher, OPT_max_allowed_packet, OPT_protocol, OPT_shared_memory_base_name,
  OPT_multi_results, OPT_multi_statements, OPT_multi_queries, OPT_secure_auth,
  OPT_report_data_truncation, OPT_plugin_dir, OPT_default_auth
};

static TYPELIB option_types={array_elements(default_options)-1,
			     "options",default_options};

#define extension_set(OPTS, X, VAL)                              \
    if (!(OPTS)->extension)                                      \
      (OPTS)->extension= (struct st_mysql_options_extention *)   \
        my_malloc(sizeof(struct st_mysql_options_extention),     \
                  MYF(MY_WME | MY_ZEROFILL));                    \
    (OPTS)->extension->X= VAL;

#define extension_set_string(OPTS, X, STR)                       \
    if ((OPTS)->extension)                                       \
      my_free((OPTS)->extension->X, MYF(MY_ALLOW_ZERO_PTR));     \
    extension_set(OPTS, X, my_strdup((STR), MYF(MY_WME)));

const char *protocol_names[]= {"TCP", "SOCKED", "PIPE", "MEMORY", NULL};
static TYPELIB protocol_types= {array_elements(protocol_names)-1,
                                "protocol names", 
                                 protocol_names};

static void mysql_read_default_options(struct st_mysql_options *options,
				       const char *filename,const char *group)
{
  int argc;
  char *argv_buff[1],**argv;
  const char *groups[3];
  DBUG_ENTER("mysql_read_default_options");
  DBUG_PRINT("enter",("file: %s  group: %s",filename,group ? group :"NULL"));

  argc=1; argv=argv_buff; argv_buff[0]= (char*) "client";
  groups[0]= (char*) "client"; groups[1]= (char*) group; groups[2]=0;

  load_defaults(filename, groups, &argc, &argv);
  if (argc != 1)				/* If some default option */
  {
    char **option=argv;
    while (*++option)
    {
      /* DBUG_PRINT("info",("option: %s",option[0])); */
      if (option[0][0] == '-' && option[0][1] == '-')
      {
	char *end=strcend(*option,'=');
	char *opt_arg=0;
	if (*end)
	{
	  opt_arg=end+1;
	  *end=0;				/* Remove '=' */
	}
	/* Change all '_' in variable name to '-' */
	for (end= *option ; *(end= strcend(end,'_')) ; )
	  *end= '-';
	switch (find_type(*option+2,&option_types,2)) {
	case OPT_port:
	  if (opt_arg)
	    options->port=atoi(opt_arg);
	  break;
	case OPT_socket:
	  if (opt_arg)
	  {
	    my_free(options->unix_socket,MYF(MY_ALLOW_ZERO_PTR));
	    options->unix_socket=my_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_compress:
	  options->compress=1;
	  break;
	case OPT_password:
	  if (opt_arg)
	  {
	    my_free(options->password,MYF(MY_ALLOW_ZERO_PTR));
	    options->password=my_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_pipe:
	  options->named_pipe=1;	/* Force named pipe */
	  break;
	case OPT_connect_timeout:
        case OPT_timeout:
	  if (opt_arg)
	    options->connect_timeout=atoi(opt_arg);
	  break;
	case OPT_user:
	  if (opt_arg)
	  {
	    my_free(options->user,MYF(MY_ALLOW_ZERO_PTR));
	    options->user=my_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_init_command:
	  if (opt_arg)
	  {
      /* todo
	    my_free(options->init_command,MYF(MY_ALLOW_ZERO_PTR));
	    options->init_command=my_strdup(opt_arg,MYF(MY_WME));
      */
	  }
	  break;
	case OPT_host:
	  if (opt_arg)
	  {
	    my_free(options->host,MYF(MY_ALLOW_ZERO_PTR));
	    options->host=my_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_database:
	  if (opt_arg)
	  {
	    my_free(options->db,MYF(MY_ALLOW_ZERO_PTR));
	    options->db=my_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_debug:
	  mysql_debug(opt_arg ? opt_arg : "d:t:o,/tmp/client.trace");
	  break;
	case OPT_return_found_rows:
	  options->client_flag|=CLIENT_FOUND_ROWS;
	  break;
#ifdef HAVE_OPENSSL
	case OPT_ssl_key:
	  my_free(options->ssl_key, MYF(MY_ALLOW_ZERO_PTR));
          options->ssl_key = my_strdup(opt_arg, MYF(MY_WME));
        break;
	case OPT_ssl_cert:
	  my_free(options->ssl_cert, MYF(MY_ALLOW_ZERO_PTR));
          options->ssl_cert = my_strdup(opt_arg, MYF(MY_WME));
          break;
	case OPT_ssl_ca:
	  my_free(options->ssl_ca, MYF(MY_ALLOW_ZERO_PTR));
          options->ssl_ca = my_strdup(opt_arg, MYF(MY_WME));
          break;
	case OPT_ssl_capath:
          my_free(options->ssl_capath, MYF(MY_ALLOW_ZERO_PTR));
          options->ssl_capath = my_strdup(opt_arg, MYF(MY_WME));
          break;
        case OPT_ssl_cipher:
          break;
#else
	case OPT_ssl_key:
	case OPT_ssl_cert:
	case OPT_ssl_ca:
	case OPT_ssl_capath:
        case OPT_ssl_cipher:
          break;
#endif /* HAVE_OPENSSL */
	case OPT_charset_dir:
	  my_free(options->charset_dir,MYF(MY_ALLOW_ZERO_PTR));
          options->charset_dir = my_strdup(opt_arg, MYF(MY_WME));
	  break;
	case OPT_charset_name:
	  my_free(options->charset_name,MYF(MY_ALLOW_ZERO_PTR));
          options->charset_name = my_strdup(opt_arg, MYF(MY_WME));
	  break;
	case OPT_interactive_timeout:
	  options->client_flag|= CLIENT_INTERACTIVE;
	  break;
	case OPT_local_infile:
	  if (!opt_arg || atoi(opt_arg) != 0)
	    options->client_flag|= CLIENT_LOCAL_FILES;
	  else
	    options->client_flag&= ~CLIENT_LOCAL_FILES;
	  break;
	case OPT_disable_local_infile:
	  options->client_flag&= CLIENT_LOCAL_FILES;
	  break;
        case OPT_max_allowed_packet:
          if(opt_arg)
            options->max_allowed_packet= atoi(opt_arg);
        case OPT_protocol:
          options->protocol= find_type(opt_arg, &protocol_types, 0);
#ifndef _WIN32
          if (options->protocol < 0 || options->protocol > 1) 
#else
          if (options->protocol < 0)
#endif
          {
            fprintf(stderr, 
                    "Unknown or unsupported protocol %s",
                    opt_arg);
          }
          break;
        case OPT_shared_memory_base_name:
          /* todo */
        break;
        case OPT_multi_results:
          options->client_flag|= CLIENT_MULTI_RESULTS;
          break;
        case OPT_multi_statements:
        case OPT_multi_queries:
          options->client_flag|= CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS;
          break;
        case OPT_report_data_truncation:
          if (opt_arg)
            options->report_data_truncation= atoi(opt_arg);
          else
            options->report_data_truncation= 1;
        break;
        case OPT_secure_auth:
        case OPT_plugin_dir:
        case OPT_default_auth:
          /* todo */
        break;
	default:
	  DBUG_PRINT("warning",("unknown option: %s",option[0]));
	}
      }
    }
  }
  free_defaults(argv);
  DBUG_VOID_RETURN;
}


/***************************************************************************
** Change field rows to field structs
***************************************************************************/

static size_t rset_field_offsets[]= {
  OFFSET(MYSQL_FIELD, catalog),
  OFFSET(MYSQL_FIELD, catalog_length),
  OFFSET(MYSQL_FIELD, db),
  OFFSET(MYSQL_FIELD, db_length),
  OFFSET(MYSQL_FIELD, table),
  OFFSET(MYSQL_FIELD, table_length),
  OFFSET(MYSQL_FIELD, org_table),
  OFFSET(MYSQL_FIELD, org_table_length),
  OFFSET(MYSQL_FIELD, name),
  OFFSET(MYSQL_FIELD, name_length),
  OFFSET(MYSQL_FIELD, org_name),
  OFFSET(MYSQL_FIELD, org_name_length)
};

MYSQL_FIELD *
unpack_fields(MYSQL_DATA *data,MEM_ROOT *alloc,uint fields,
	      my_bool default_value, my_bool long_flag_protocol)
{
  MYSQL_ROWS	*row;
  MYSQL_FIELD	*field,*result;
  char    *p;
  unsigned int i, field_count= sizeof(rset_field_offsets)/sizeof(size_t)/2;

  DBUG_ENTER("unpack_fields");
  field=result=(MYSQL_FIELD*) alloc_root(alloc,sizeof(MYSQL_FIELD)*fields);
  if (!result)
    DBUG_RETURN(0);

  for (row=data->data; row ; row = row->next,field++)
  {
    for (i=0; i < field_count; i++)
    {
      switch(row->data[i][0]) {
      case 0:
       *(char **)(((char *)field) + rset_field_offsets[i*2])= strdup_root(alloc, "");
       *(unsigned int *)(((char *)field) + rset_field_offsets[i*2+1])= 0;
       break;
     default:
       *(char **)(((char *)field) + rset_field_offsets[i*2])= 
         strdup_root(alloc, (char *)row->data[i]);
       *(unsigned int *)(((char *)field) + rset_field_offsets[i*2+1])=
         (uint)(row->data[i+1] - row->data[i] - 1);
       break;
      }
    }

    p= (char *)row->data[6];
    /* filler */
    field->charsetnr= uint2korr(p);
    p+= 2;
    field->length= (uint) uint4korr(p);
    p+= 4;
    field->type=   (enum enum_field_types)uint1korr(p);
    p++;
    field->flags= uint2korr(p);
    p+= 2;
    field->decimals= (uint) p[0];
    p++;

    /* filler */
    p+= 2;

    if (INTERNAL_NUM_FIELD(field))
      field->flags|= NUM_FLAG;

    if (default_value && row->data[7])
    {
      field->def=strdup_root(alloc,(char*) row->data[7]);
    }
    else
      field->def=0;
    field->max_length= 0;
  }
  free_rows(data);				/* Free old data */
  DBUG_RETURN(result);
}


/* Read all rows (fields or data) from server */

MYSQL_DATA *read_rows(MYSQL *mysql,MYSQL_FIELD *mysql_fields,
			     uint fields)
{
  uint	field;
  ulong pkt_len;
  ulong len;
  uchar *cp;
  char	*to, *end_to;
  MYSQL_DATA *result;
  MYSQL_ROWS **prev_ptr,*cur;
  NET *net = &mysql->net;
  DBUG_ENTER("read_rows");

  if ((pkt_len= net_safe_read(mysql)) == packet_error)
    DBUG_RETURN(0);
  if (!(result=(MYSQL_DATA*) my_malloc(sizeof(MYSQL_DATA),
				       MYF(MY_WME | MY_ZEROFILL))))
  {
    SET_CLIENT_ERROR(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0);
    DBUG_RETURN(0);
  }
  init_alloc_root(&result->alloc,8192,0);	/* Assume rowlength < 8192 */
  result->alloc.min_malloc=sizeof(MYSQL_ROWS);
  prev_ptr= &result->data;
  result->rows=0;
  result->fields=fields;

  while (*(cp=net->read_pos) != 254 || pkt_len >= 8)
  {
    result->rows++;
    if (!(cur= (MYSQL_ROWS*) alloc_root(&result->alloc,
					    sizeof(MYSQL_ROWS))) ||
	!(cur->data= ((MYSQL_ROW)
		      alloc_root(&result->alloc,
				     (fields+1)*sizeof(char *)+pkt_len))))
    {
      free_rows(result);
      SET_CLIENT_ERROR(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0);
      DBUG_RETURN(0);
    }
    *prev_ptr=cur;
    prev_ptr= &cur->next;
    to= (char*) (cur->data+fields+1);
    end_to=to+pkt_len-1;
    for (field=0 ; field < fields ; field++)
    {
      if ((len=(ulong) net_field_length(&cp)) == NULL_LENGTH)
      {						/* null field */
	cur->data[field] = 0;
      }
      else
      {
	cur->data[field] = to;
        if (len > (ulong) (end_to - to))
        {
          free_rows(result);
          SET_CLIENT_ERROR(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate, 0);
          DBUG_RETURN(0);
        }
        memcpy(to,(char*) cp,len); to[len]=0;
        to+=len+1;
        cp+=len;
        if (mysql_fields)
        {
          if (mysql_fields[field].max_length < len)
            mysql_fields[field].max_length=len;
         }
      }
    }
    cur->data[field]=to;			/* End of last field */
    if ((pkt_len=net_safe_read(mysql)) == packet_error)
    {
      free_rows(result);
      DBUG_RETURN(0);
    }
  }
  *prev_ptr=0;					/* last pointer is null */
  /* save status */
  if (pkt_len > 1)
  {
    cp++;
    mysql->warning_count= uint2korr(cp);
    cp+= 2;
    mysql->server_status= uint2korr(cp);
  }
  DBUG_PRINT("exit",("Got %d rows",result->rows));
  DBUG_RETURN(result);
}


/*
** Read one row. Uses packet buffer as storage for fields.
** When next packet is read, the previous field values are destroyed
*/


static int
read_one_row(MYSQL *mysql,uint fields,MYSQL_ROW row, ulong *lengths)
{
  uint field;
  ulong pkt_len,len;
  uchar *pos,*prev_pos, *end_pos;

  if ((pkt_len=(uint) net_safe_read(mysql)) == packet_error)
    return -1;
  
  if (pkt_len <= 8 && mysql->net.read_pos[0] == 254)
  {
    mysql->warning_count= uint2korr(mysql->net.read_pos + 1);
    mysql->server_status= uint2korr(mysql->net.read_pos + 3);
    return 1;				/* End of data */
  }
  prev_pos= 0;				/* allowed to write at packet[-1] */
  pos=mysql->net.read_pos;
  end_pos=pos+pkt_len;
  for (field=0 ; field < fields ; field++)
  {
    if ((len=(ulong) net_field_length(&pos)) == NULL_LENGTH)
    {						/* null field */
      row[field] = 0;
      *lengths++=0;
    }
    else
    {
      if (len > (ulong) (end_pos - pos))
      {
  mysql->net.last_errno=CR_UNKNOWN_ERROR;
  strmov(mysql->net.last_error,ER(mysql->net.last_errno));
  return -1;
      }
      row[field] = (char*) pos;
      pos+=len;
      *lengths++=len;
    }
    if (prev_pos)
      *prev_pos=0;				/* Terminate prev field */
    prev_pos=pos;
  }
  row[field]=(char*) prev_pos+1;		/* End of last field */
  *prev_pos=0;					/* Terminate last field */
  return 0;
}

/****************************************************************************
** Init MySQL structure or allocate one
****************************************************************************/

MYSQL * STDCALL
mysql_init(MYSQL *mysql)
{
  if (mysql_server_init(0, NULL, NULL))
    return NULL;
  if (!mysql)
  {
    if (!(mysql=(MYSQL*) my_malloc(sizeof(*mysql),MYF(MY_WME | MY_ZEROFILL))))
      return 0;
    mysql->free_me=1;
    mysql->net.vio= 0;
  }
  else
    bzero((char*) (mysql),sizeof(*(mysql)));
  mysql->options.connect_timeout=CONNECT_TIMEOUT;
  mysql->charset= default_charset_info;
  strmov(mysql->net.sqlstate, "00000");
  mysql->net.last_error[0]= mysql->net.last_errno= 0;
#if defined(SIGPIPE) && defined(THREAD) && !defined(_WIN32)
  if (!((mysql)->client_flag & CLIENT_IGNORE_SIGPIPE))
    (void) signal(SIGPIPE,pipe_sig_handler);
#endif

/*
  Only enable LOAD DATA INFILE by default if configured with
  --enable-local-infile
*/
#ifdef ENABLED_LOCAL_INFILE
  mysql->options.client_flag|= CLIENT_LOCAL_FILES;
#endif
  return mysql;
}



//#ifdef HAVE_OPENSSL
/**************************************************************************
** Fill in SSL part of MYSQL structure and set 'use_ssl' flag.
** NB! Errors are not reported until you do mysql_real_connect.
**************************************************************************/

int STDCALL
mysql_ssl_set(MYSQL *mysql, const char *key, const char *cert,
        const char *ca, const char *capath)
{
  mysql->options.ssl_key = key==0 ? 0 : my_strdup(key,MYF(0));
  mysql->options.ssl_cert = cert==0 ? 0 : my_strdup(cert,MYF(0));
  mysql->options.ssl_ca = ca==0 ? 0 : my_strdup(ca,MYF(0));
  mysql->options.ssl_capath = capath==0 ? 0 : my_strdup(capath,MYF(0));
  mysql->options.use_ssl = 1;
  //mysql->connector_fd = new_VioSSLConnectorFd(key, cert, ca, capath);
  return 0;
}

/**************************************************************************
**************************************************************************/

const char * STDCALL
mysql_get_ssl_cipher(MYSQL *mysql)
{
#ifdef HAVE_OPENSSL
  if (mysql->net.vio && mysql->net.vio->ssl)
  {
    return SSL_get_cipher_name(mysql->net.vio->ssl);
  }
#endif
  return(NULL);
}

/**************************************************************************
** Free strings in the SSL structure and clear 'use_ssl' flag.
** NB! Errors are not reported until you do mysql_real_connect.
**************************************************************************/

//#endif /* HAVE_OPENSSL */

/**************************************************************************
** Connect to sql server
** If host == 0 then use localhost
**************************************************************************/

MYSQL * STDCALL
mysql_connect(MYSQL *mysql,const char *host,
	      const char *user, const char *passwd)
{
  MYSQL *res;
  mysql=mysql_init(mysql);			/* Make it thread safe */
  {
    DBUG_ENTER("mysql_connect");
    if (!(res=mysql_real_connect(mysql,host,user,passwd,NullS,0,NullS,0)))
    {
      if (mysql->free_me)
	my_free((gptr) mysql,MYF(0));
    }
    DBUG_RETURN(res);
  }
}


/*
** Note that the mysql argument must be initialized with mysql_init()
** before calling mysql_real_connect !
*/

MYSQL * STDCALL
mysql_real_connect(MYSQL *mysql,const char *host, const char *user,
		   const char *passwd, const char *db,
		   uint port, const char *unix_socket,unsigned long client_flag)
{
  char		buff[NAME_LEN+USERNAME_LENGTH+100];
  char		*end, *end_pkt, *host_info,
                *charset_name= NULL;
  my_socket	sock;
  char          *scramble_data;
  const char *  scramble_plugin;
  uint32	ip_addr;
  struct	sockaddr_in sock_addr;
  uint		pkt_length, scramble_len, pkt_scramble_len= 0;
  NET		*net= &mysql->net;
#ifdef _WIN32
  HANDLE	hPipe=INVALID_HANDLE_VALUE;
#endif
#ifdef HAVE_SYS_UN_H
  struct	sockaddr_un UNIXaddr;
#endif
  init_sigpipe_variables
  DBUG_ENTER("mysql_real_connect");

  DBUG_PRINT("enter",("host: %s  db: %s  user: %s",
		      host ? host : "(Null)",
		      db ? db : "(Null)",
		      user ? user : "(Null)"));

  if (net->vio)  /* check if we are already connected */
  {
    SET_CLIENT_ERROR(mysql, CR_ALREADY_CONNECTED, SQLSTATE_UNKNOWN, 0);
    DBUG_RETURN(NULL);
  }
  
  /* Don't give sigpipe errors if the client doesn't want them */
  set_sigpipe(mysql);

  /* use default options */
  if (mysql->options.my_cnf_file || mysql->options.my_cnf_group)
  {
    mysql_read_default_options(&mysql->options,
			       (mysql->options.my_cnf_file ?
				mysql->options.my_cnf_file : "my"),
			       mysql->options.my_cnf_group);
    my_free(mysql->options.my_cnf_file,MYF(MY_ALLOW_ZERO_PTR));
    my_free(mysql->options.my_cnf_group,MYF(MY_ALLOW_ZERO_PTR));
    mysql->options.my_cnf_file=mysql->options.my_cnf_group=0;
  }

  /* Some empty-string-tests are done because of ODBC */
  if (!host || !host[0])
    host=mysql->options.host;
  if (!user || !user[0])
    user=mysql->options.user;
  if (!passwd)
  {
    passwd=mysql->options.password;
#ifndef DONT_USE_MYSQL_PWD
    if (!passwd)
      passwd=getenv("MYSQL_PWD");  /* get it from environment (haneke) */
#endif
  }
  if (!db || !db[0])
    db=mysql->options.db;
  if (!port)
    port=mysql->options.port;
  if (!unix_socket)
    unix_socket=mysql->options.unix_socket;


  /*  Since 5.0.3 reconnect is not enabled by default!!
  mysql->reconnect=1; */
  mysql->server_status=SERVER_STATUS_AUTOCOMMIT;

  /*
  ** Grab a socket and connect it to the server
  */

#if defined(HAVE_SYS_UN_H)
  if ((!host || !strcmp(host,LOCAL_HOST)) && (unix_socket || mysql_unix_port))
  {
    host=LOCAL_HOST;
    if (!unix_socket)
      unix_socket=mysql_unix_port;
    host_info=(char*) ER(CR_LOCALHOST_CONNECTION);
    DBUG_PRINT("info",("Using UNIX sock '%s'",unix_socket));
    if ((sock = socket(AF_UNIX,SOCK_STREAM,0)) == SOCKET_ERROR)
    {
      net->last_errno=CR_SOCKET_CREATE_ERROR;
      sprintf(net->last_error,ER(net->last_errno),socket_errno);
      goto error;
    }
    net->vio = vio_new(sock, VIO_TYPE_SOCKET, TRUE);
    bzero((char*) &UNIXaddr,sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmov(UNIXaddr.sun_path, unix_socket);
    if (connect2(sock,(struct sockaddr *) &UNIXaddr, sizeof(UNIXaddr),
		 mysql->options.connect_timeout) <0)
    {
      DBUG_PRINT("error",("Got error %d on connect to local server",socket_errno));
      my_set_error(mysql, CR_CONNECTION_ERROR, SQLSTATE_UNKNOWN, ER(CR_CONNECTION_ERROR),
                          unix_socket, socket_errno);
      goto error;
    }
  }
  else
#elif defined(_WIN32)
  {
    if ((unix_socket ||
	 !host && is_NT() ||
	 host && !strcmp(host,LOCAL_HOST_NAMEDPIPE) ||
	 mysql->options.named_pipe || !have_tcpip))
    {
      sock=0;
      if ((hPipe=create_named_pipe(net, mysql->options.connect_timeout,
				   (char**) &host, (char**) &unix_socket)) ==
	  INVALID_HANDLE_VALUE)
      {
	DBUG_PRINT("error",
		   ("host: '%s'  socket: '%s'  named_pipe: %d  have_tcpip: %d",
		    host ? host : "<null>",
		    unix_socket ? unix_socket : "<null>",
		    (int) mysql->options.named_pipe,
		    (int) have_tcpip));
	if (mysql->options.named_pipe ||
	    (host && !strcmp(host,LOCAL_HOST_NAMEDPIPE)) ||
	    (unix_socket && !strcmp(unix_socket,MYSQL_NAMEDPIPE)))
	  goto error;		/* User only requested named pipes */
	/* Try also with TCP/IP */
      }
      else
      {
	net->vio=vio_new_win32pipe(hPipe);
	sprintf(host_info=buff, ER(CR_NAMEDPIPE_CONNECTION), host,
		unix_socket);
      }
    }
  }
  if (hPipe == INVALID_HANDLE_VALUE)
#endif
  {
    unix_socket=0;				/* This is not used */
    if (!port)
      port=mysql_port;
    if (!host)
      host=LOCAL_HOST;
    sprintf(host_info=buff,ER(CR_TCP_CONNECTION),host);
    DBUG_PRINT("info",("Server name: '%s'.  TCP sock: %d", host,port));
    /* _WIN64 ;  Assume that the (int) range is enough for socket() */
    if ((sock = (my_socket) socket(AF_INET,SOCK_STREAM,0)) == SOCKET_ERROR)
    {
      my_set_error(mysql, CR_IPSOCK_ERROR, SQLSTATE_UNKNOWN, ER(CR_IPSOCK_ERROR),
                   socket_errno);
      goto error;
    }
    net->vio = vio_new(sock,VIO_TYPE_TCPIP,FALSE);
    bzero((char*) &sock_addr,sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;

    /*
    ** The server name may be a host name or IP address
    */

    if ((int) (ip_addr = inet_addr(host)) != (int) INADDR_NONE)
    {
      memcpy_fixed(&sock_addr.sin_addr,&ip_addr,sizeof(ip_addr));
    }
    else
    {
      int tmp_errno;
      struct hostent tmp_hostent,*hp;
      char buff2[GETHOSTBYNAME_BUFF_SIZE];
      hp = my_gethostbyname_r(host,&tmp_hostent,buff2,sizeof(buff2),
			      &tmp_errno);
      if (!hp)
      {
        my_set_error(mysql, CR_UNKNOWN_HOST, SQLSTATE_UNKNOWN, ER(CR_UNKNOWN_HOST),
                     host, tmp_errno);
	my_gethostbyname_r_free();
	goto error;
      }
      memcpy(&sock_addr.sin_addr,hp->h_addr, (size_t) hp->h_length);
      my_gethostbyname_r_free();
    }
    sock_addr.sin_port = (ushort) htons((ushort) port);
    if (connect2(sock,(struct sockaddr *) &sock_addr, sizeof(sock_addr),
		 mysql->options.connect_timeout) <0)
    {
      DBUG_PRINT("error",("Got error %d on connect to '%s'",socket_errno,host));
      my_set_error(mysql, CR_CONN_HOST_ERROR, SQLSTATE_UNKNOWN,
                   ER(CR_CONN_HOST_ERROR), host, socket_errno);
      goto error;
    }
  }

  if (!net->vio || my_net_init(net, net->vio))
  {
    vio_delete(net->vio);
    net->vio = 0;
    SET_CLIENT_ERROR(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0);
    goto error;
  }
  vio_keepalive(net->vio,TRUE);
  strmov(mysql->net.sqlstate, "00000"); 


  /* set read timeout */
  if (mysql->options.read_timeout)
    vio_read_timeout(net->vio, mysql->options.read_timeout);

  /* set write timeout */
  if (mysql->options.write_timeout)
    vio_write_timeout(net->vio, mysql->options.read_timeout);


  /* Get version info */
  mysql->protocol_version= PROTOCOL_VERSION;	/* Assume this */
  if (mysql->options.connect_timeout &&
      vio_poll_read(net->vio, mysql->options.connect_timeout))
  {
    my_set_error(mysql, CR_SERVER_LOST, SQLSTATE_UNKNOWN, 0);
    goto error;
  }
  if ((pkt_length=net_safe_read(mysql)) == packet_error)
    goto error;

  end= (char *)net->read_pos;
  end_pkt= (char *)net->read_pos + pkt_length;

  /* Check if version of protocoll matches current one */

  mysql->protocol_version= end[0];
  end++;

  /* Check if server sends an error */
  if (mysql->protocol_version == 0XFF)
  {
    net_get_error(end, pkt_length - 1, net->last_error, sizeof(net->last_error),
      &net->last_errno, net->sqlstate);
    /* fix for bug #26426 */
    if (net->last_errno == 1040)
      memcpy(net->sqlstate, "08004", SQLSTATE_LENGTH);
    goto error;
  }

  DBUG_DUMP("packet",(char*) net->read_pos,10);
  DBUG_PRINT("info",("mysql protocol version %d, server=%d",
		     PROTOCOL_VERSION, mysql->protocol_version));
  if (mysql->protocol_version <  PROTOCOL_VERSION)
  {
    net->last_errno= CR_VERSION_ERROR;
    sprintf(net->last_error, ER(CR_VERSION_ERROR), mysql->protocol_version,
	    PROTOCOL_VERSION);
    goto error;
  }
  /* Save connection information */
  if (!user) user="";
  if (!passwd) passwd="";

  if (!my_multi_malloc(MYF(0),
		       &mysql->host_info, (uint) strlen(host_info)+1,
		       &mysql->host,      (uint) strlen(host)+1,
		       &mysql->unix_socket,unix_socket ?
		       (uint) strlen(unix_socket)+1 : (uint) 1,
		       &mysql->server_version, (uint) (end - (char*) net->read_pos),
		       NullS) ||
      !(mysql->user=my_strdup(user,MYF(0))) ||
      !(mysql->passwd=my_strdup(passwd,MYF(0))))
  {
    SET_CLIENT_ERROR(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0);
    goto error;
  }
  strmov(mysql->host_info,host_info);
  strmov(mysql->host,host);
  if (unix_socket)
    strmov(mysql->unix_socket,unix_socket);
  else
    mysql->unix_socket=0;
  mysql->port=port;
  client_flag|=mysql->options.client_flag;

  mysql->server_version= my_strdup(end, MYF(0));
  end+= strlen(mysql->server_version) + 1;

  mysql->thread_id=uint4korr(end);
  end+=4;

  /* This is the first part of scramble packet. In 4.1 and later
     a second package will follow later */
  scramble_data= end;
  scramble_len= SCRAMBLE_LENGTH_323 + 1;
  scramble_plugin= old_password_plugin_name;
  end+= SCRAMBLE_LENGTH_323;

  /* 1st pad */
  end++;

  if (end + 1<= end_pkt)
  {
    mysql->server_capabilities=uint2korr(end);
  }

  /* mysql 5.5 protocol */
  if (end + 18 <= end_pkt)
  {
    mysql->server_language= uint1korr(end + 2);
    mysql->server_status= uint2korr(end + 3);
    mysql->server_capabilities|= uint2korr(end + 5) << 16;
    pkt_scramble_len= uint1korr(end + 7);
  }
  /* pad 2 */
  end+= 18;

  /* second scramble package */
  if (end + SCRAMBLE_LENGTH - SCRAMBLE_LENGTH_323 + 1 <= end_pkt)
  {
    memcpy(end - SCRAMBLE_LENGTH_323, scramble_data, SCRAMBLE_LENGTH_323);
    scramble_data= end - SCRAMBLE_LENGTH_323;
    if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    {
      scramble_len= pkt_scramble_len;
      scramble_plugin= scramble_data + scramble_len;
      if (scramble_data + scramble_len > end_pkt)
        scramble_len= end_pkt - scramble_data;
    } else
    {
      scramble_len= end_pkt - scramble_data;
      scramble_plugin= native_password_plugin_name;
    }
  } else
    mysql->server_capabilities&= ~CLIENT_SECURE_CONNECTION;

  /* Set character set */
  if (mysql->options.charset_name)
    mysql->charset= mysql_find_charset_name(mysql->options.charset_name);
  else if (mysql->server_language)
    mysql->charset= mysql_find_charset_nr(mysql->server_language);
  else
    mysql->charset=default_charset_info;

  if (!mysql->charset)
  {
    net->last_errno=CR_CANT_READ_CHARSET;
    sprintf(net->last_error,ER(net->last_errno),
      charset_name ? charset_name : "unknown",
      "compiled_in");
    goto error;
  }

  mysql->client_flag= client_flag;

  if (run_plugin_auth(mysql, scramble_data, scramble_len,
                             scramble_plugin, db))
    goto error;

  if (mysql->client_flag & CLIENT_COMPRESS)
    net->compress= 1;

  /* last part: select default db */
  if (db && !mysql->db)
  {
    if (mysql_select_db(mysql, db))
    {
      my_set_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                          ER(CR_SERVER_LOST_EXTENDED),
                          "Setting intital database",
                          errno);
      goto error;
    }
  }  

  DBUG_PRINT("info",("Server version = '%s'  capabilites: %ld  status: %d  client_flag: %d",
		     mysql->server_version,mysql->server_capabilities,
		     mysql->server_status, client_flag));

  if (mysql->options.init_command)
  {
    MYSQL_RES *res;
    int status;
    /* Avoid reconnect in mysql_real_connect */
    my_bool save_reconnect= mysql->reconnect;
    
    mysql->reconnect= 0;

    if (mysql_query(mysql, mysql->options.init_command))
      goto error;

    /* handle possible multi results */
    do {
      if ((res= mysql_use_result(mysql)))
        mysql_free_result(res);
      status= mysql_next_result(mysql);
    } while (status == 0);
    mysql->reconnect= save_reconnect;
  }

  strmov(mysql->net.sqlstate, "00000");

  DBUG_PRINT("exit",("Mysql handler: %lx",mysql));
  reset_sigpipe(mysql);
  DBUG_RETURN(mysql);

error:
  reset_sigpipe(mysql);
  DBUG_PRINT("error",("message: %u (%s)",net->last_errno,net->last_error));
  {
    /* Free alloced memory */
    end_server(mysql);
    /* only free the allocated memory, user needs to call mysql_close */
    mysql_close_memory(mysql);
  }
  DBUG_RETURN(0);
}


static my_bool mysql_reconnect(MYSQL *mysql)
{
  MYSQL tmp_mysql;
  LIST *li_stmt= mysql->stmts;
  DBUG_ENTER("mysql_reconnect");

  if (!mysql->reconnect ||
      (mysql->server_status & SERVER_STATUS_IN_TRANS) || !mysql->host_info)
  {
   /* Allov reconnect next time */
    mysql->server_status&= ~SERVER_STATUS_IN_TRANS;
    my_set_error(mysql, CR_SERVER_GONE_ERROR, SQLSTATE_UNKNOWN, 0);
    DBUG_RETURN(1);
  }
  mysql_init(&tmp_mysql);
  tmp_mysql.options=mysql->options;
  bzero((char*) &mysql->options,sizeof(mysql->options));
  if (!mysql_real_connect(&tmp_mysql,mysql->host,mysql->user,mysql->passwd,
			  mysql->db, mysql->port, mysql->unix_socket,
			  mysql->client_flag))
  {
    my_set_error(mysql, tmp_mysql.net.last_errno, 
                        tmp_mysql.net.sqlstate, 
                        tmp_mysql.net.last_error);
    DBUG_RETURN(1);
  }
  tmp_mysql.free_me=mysql->free_me;
  mysql->free_me=0;
  mysql_close(mysql);
  *mysql=tmp_mysql;
  net_clear(&mysql->net);
  mysql->affected_rows= ~(my_ulonglong) 0;

  /* reset the connection in all active statements 
     todo: check stmt->mysql in mysql_stmt* functions ! */
  for (;li_stmt;li_stmt= li_stmt->next)
  {
    MYSQL_STMT *stmt= (MYSQL_STMT *)li_stmt->data;
    stmt->mysql= NULL;
    stmt->state= MYSQL_STMT_INITTED;
    SET_CLIENT_STMT_ERROR(stmt, CR_SERVER_LOST, SQLSTATE_UNKNOWN, 0);
  }
  DBUG_RETURN(0);
}


/**************************************************************************
** Change user and database 
**************************************************************************/

my_bool	STDCALL mysql_change_user(MYSQL *mysql, const char *user, 
				  const char *passwd, const char *db)
{
  const CHARSET_INFO *s_cs= mysql->charset;
  char *s_user= mysql->user, 
       *s_passwd= mysql->passwd, 
       *s_db= mysql->db;
  int rc;
  
  DBUG_ENTER("mysql_change_user");

  if (!user)
    user="";
  if (!passwd)
    passwd="";
  if (!db)
    db="";

  if (mysql->options.charset_name)
    mysql->charset =mysql_find_charset_name(mysql->options.charset_name);
  else if (mysql->server_language)
    mysql->charset=mysql_find_charset_nr(mysql->server_language);
  else
    mysql->charset=default_charset_info;

  mysql->user= (char *)user;
  mysql->passwd= (char *)passwd;
  mysql->db= (char *)db;
  rc= run_plugin_auth(mysql, 0, 0, 0, db);

  if (rc==0)
  {
    LIST *li_stmt= mysql->stmts;
    my_free(s_user,MYF(MY_ALLOW_ZERO_PTR));
    my_free(s_passwd,MYF(MY_ALLOW_ZERO_PTR));
    my_free(s_db,MYF(MY_ALLOW_ZERO_PTR));

    mysql->user=  my_strdup(user,MYF(MY_WME));
    mysql->passwd=my_strdup(passwd,MYF(MY_WME));
    mysql->db=    db ? my_strdup(db,MYF(MY_WME)) : 0;

    for (;li_stmt;li_stmt= li_stmt->next)
    {
      MYSQL_STMT *stmt= (MYSQL_STMT *)li_stmt->data;
      stmt->mysql= NULL;
      SET_CLIENT_STMT_ERROR(stmt, CR_SERVER_LOST, SQLSTATE_UNKNOWN, 0);
    }/* detach stmts */
    mysql->stmts= NULL;
    
  } else
  {
    mysql->user= s_user;
    mysql->passwd= s_passwd;
    mysql->db= s_db;
    mysql->charset= s_cs;
  }
  DBUG_RETURN(rc);
}


/**************************************************************************
** Set current database
**************************************************************************/

int STDCALL
mysql_select_db(MYSQL *mysql, const char *db)
{
  int error;
  DBUG_ENTER("mysql_select_db");
  DBUG_PRINT("enter",("db: '%s'",db));

  if ((error=simple_command(mysql,MYSQL_COM_INIT_DB,db,(uint) strlen(db),0)))
    DBUG_RETURN(error);
  my_free(mysql->db,MYF(MY_ALLOW_ZERO_PTR));
  mysql->db=my_strdup(db,MYF(MY_WME));
  DBUG_RETURN(0);
}


/*************************************************************************
** Send a QUIT to the server and close the connection
** If handle is alloced by mysql connect free it.
*************************************************************************/

static void mysql_close_options(MYSQL *mysql)
{
/* todo
  my_free(mysql->options.init_command,MYF(MY_ALLOW_ZERO_PTR));
*/
  my_free(mysql->options.user,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.host,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.password,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.unix_socket,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.db,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.my_cnf_file,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.my_cnf_group,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.charset_dir,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.charset_name,MYF(MY_ALLOW_ZERO_PTR));
#ifdef HAVE_OPENSSL
  my_free(mysql->options.ssl_key, MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.ssl_cert, MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.ssl_ca, MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->options.ssl_capath, MYF(MY_ALLOW_ZERO_PTR));
#endif /* HAVE_OPENSSL */
}

static void mysql_close_memory(MYSQL *mysql)
{
  my_free(mysql->host_info, MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->user,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->passwd,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->db,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->server_version,MYF(MY_ALLOW_ZERO_PTR));
  mysql->host_info= mysql->server_version=mysql->user=mysql->passwd=mysql->db=0;
  mysql_close_options(mysql);
}



void my_set_error(MYSQL *mysql,
                  unsigned int error_nr,
                  const char *sqlstate,
                  const char *format,
                  ...)
{
  va_list ap;

  DBUG_ENTER("my_set_error");

  mysql->net.last_errno= error_nr;
  strncpy(mysql->net.sqlstate, sqlstate, SQLSTATE_LENGTH);
  va_start(ap, format);
  my_vsnprintf(mysql->net.last_error, MYSQL_ERRMSG_SIZE, 
               format ? format : ER(error_nr), ap);
  DBUG_PRINT("info", ("error(%d) %s", error_nr, mysql->net.last_error));
  va_end(ap);
  DBUG_VOID_RETURN;
}

void STDCALL
mysql_close(MYSQL *mysql)
{
  DBUG_ENTER("mysql_close");
  if (mysql)					/* Some simple safety */
  {
    LIST *li_stmt= mysql->stmts;
    if (mysql->net.vio)
    {
      free_old_query(mysql);
      mysql->status=MYSQL_STATUS_READY; /* Force command */
      mysql->reconnect=0;
      simple_command(mysql,MYSQL_COM_QUIT,NullS,0,1);
      end_server(mysql);
    }

    /* reset the connection in all active statements 
       todo: check stmt->mysql in mysql_stmt* functions ! */
    for (;li_stmt;li_stmt= li_stmt->next)
    {
      MYSQL_STMT *stmt= (MYSQL_STMT *)li_stmt->data;
      stmt->mysql= NULL;
      SET_CLIENT_STMT_ERROR(stmt, CR_SERVER_LOST, SQLSTATE_UNKNOWN, 0);
    }
    mysql_close_memory(mysql);
    mysql->host_info=mysql->user=mysql->passwd=mysql->db=0;
   
    /* Clear pointers for better safety */
    bzero((char*) &mysql->options,sizeof(mysql->options));
    mysql->net.vio= 0;
    if (mysql->free_me)
      my_free((gptr) mysql,MYF(0));
  }
  DBUG_VOID_RETURN;
}


/**************************************************************************
** Do a query. If query returned rows, free old rows.
** Read data by mysql_store_result or by repeat call of mysql_fetch_row
**************************************************************************/

int STDCALL
mysql_query(MYSQL *mysql, const char *query)
{
  return mysql_real_query(mysql,query, (uint) strlen(query));
}

/*
  Send the query and return so we can do something else.
  Needs to be followed by mysql_read_query_result() when we want to
  finish processing it.
*/  

int STDCALL
mysql_send_query(MYSQL* mysql, const char* query, uint length)
{
  return simple_command(mysql, MYSQL_COM_QUERY, query, length, 1);
}

int STDCALL mysql_read_query_result(MYSQL *mysql)
{
  uchar *pos;
  ulong field_count;
  MYSQL_DATA *fields;
  ulong length;
  DBUG_ENTER("mysql_read_query_result");

  if (!mysql || (length = net_safe_read(mysql)) == packet_error)
    DBUG_RETURN(1);
  free_old_query(mysql);			/* Free old result */
get_info:
  pos=(uchar*) mysql->net.read_pos;
  if ((field_count= net_field_length(&pos)) == 0)
  {
    mysql->affected_rows= net_field_length_ll(&pos);
    mysql->insert_id=	  net_field_length_ll(&pos);
    mysql->server_status=uint2korr(pos); 
    pos+=2;
    mysql->warning_count=uint2korr(pos); 
    pos+=2;
    if (pos < mysql->net.read_pos+length && net_field_length(&pos))
      mysql->info=(char*) pos;
    DBUG_RETURN(0);
  }
  if (field_count == NULL_LENGTH)		/* LOAD DATA LOCAL INFILE */
  {
    int error=mysql_handle_local_infile(mysql, (char *)pos);

    if ((length=net_safe_read(mysql)) == packet_error || error)
      DBUG_RETURN(-1);
    goto get_info;				/* Get info packet */
  }
  if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
    mysql->server_status|= SERVER_STATUS_IN_TRANS;

  mysql->extra_info= net_field_length_ll(&pos); /* Maybe number of rec */
  if (!(fields=read_rows(mysql,(MYSQL_FIELD*) 0,8)))
    DBUG_RETURN(-1);
  if (!(mysql->fields=unpack_fields(fields,&mysql->field_alloc,
				    (uint) field_count,1,
				    (my_bool) test(mysql->server_capabilities &
						   CLIENT_LONG_FLAG))))
    DBUG_RETURN(-1);
  mysql->status=MYSQL_STATUS_GET_RESULT;
  mysql->field_count=field_count;
  DBUG_RETURN(0);
}

int STDCALL
mysql_real_query(MYSQL *mysql, const char *query, uint length)
{
  DBUG_ENTER("mysql_real_query");
  DBUG_PRINT("enter",("handle: %lx",mysql));
  DBUG_PRINT("query",("Query = \"%.255s\" length=%u",query, length));

  if (simple_command(mysql,MYSQL_COM_QUERY,query,length,1))
    DBUG_RETURN(-1);
  DBUG_RETURN(mysql_read_query_result(mysql));
}

/**************************************************************************
** Alloc result struct for buffered results. All rows are read to buffer.
** mysql_data_seek may be used.
**************************************************************************/

MYSQL_RES * STDCALL
mysql_store_result(MYSQL *mysql)
{
  MYSQL_RES *result;
  DBUG_ENTER("mysql_store_result");

  if (!mysql->fields)
    DBUG_RETURN(0);
  if (mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    SET_CLIENT_ERROR(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate, 0);
    DBUG_RETURN(0);
  }
  mysql->status=MYSQL_STATUS_READY;		/* server is ready */
  if (!(result=(MYSQL_RES*) my_malloc(sizeof(MYSQL_RES)+
				      sizeof(ulong)*mysql->field_count,
				      MYF(MY_WME | MY_ZEROFILL))))
  {
    SET_CLIENT_ERROR(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate, 0);
    DBUG_RETURN(0);
  }
  result->eof=1;				/* Marker for buffered */
  result->lengths=(ulong*) (result+1);
  if (!(result->data=read_rows(mysql,mysql->fields,mysql->field_count)))
  {
    my_free((gptr) result,MYF(0));
    DBUG_RETURN(0);
  }
  mysql->affected_rows= result->row_count= result->data->rows;
  result->data_cursor=	result->data->data;
  result->fields=	mysql->fields;
  result->field_alloc=	mysql->field_alloc;
  result->field_count=	mysql->field_count;
  result->current_field=0;
  result->current_row=0;			/* Must do a fetch first */
  mysql->fields=0;				/* fields is now in result */
  DBUG_RETURN(result);				/* Data fetched */
}


/**************************************************************************
** Alloc struct for use with unbuffered reads. Data is fetched by domand
** when calling to mysql_fetch_row.
** mysql_data_seek is a noop.
**
** No other queries may be specified with the same MYSQL handle.
** There shouldn't be much processing per row because mysql server shouldn't
** have to wait for the client (and will not wait more than 30 sec/packet).
**************************************************************************/

MYSQL_RES * STDCALL
mysql_use_result(MYSQL *mysql)
{
  MYSQL_RES *result;
  DBUG_ENTER("mysql_use_result");

  if (!mysql->fields)
    DBUG_RETURN(0);
  if (mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    SET_CLIENT_ERROR(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate, 0);
    DBUG_RETURN(0);
  }
  if (!(result=(MYSQL_RES*) my_malloc(sizeof(*result)+
				      sizeof(ulong)*mysql->field_count,
				      MYF(MY_WME | MY_ZEROFILL))))
    DBUG_RETURN(0);
  result->lengths=(ulong*) (result+1);
  if (!(result->row=(MYSQL_ROW)
	my_malloc(sizeof(result->row[0])*(mysql->field_count+1), MYF(MY_WME))))
  {					/* Ptrs: to one row */
    my_free((gptr) result,MYF(0));
    DBUG_RETURN(0);
  }
  result->fields=	mysql->fields;
  result->field_alloc=	mysql->field_alloc;
  result->field_count=	mysql->field_count;
  result->current_field=0;
  result->handle=	mysql;
  result->current_row=	0;
  mysql->fields=0;			/* fields is now in result */
  mysql->status=MYSQL_STATUS_USE_RESULT;
  DBUG_RETURN(result);			/* Data is read to be fetched */
}

/**************************************************************************
** Return next field of the query results
**************************************************************************/
MYSQL_FIELD * STDCALL
mysql_fetch_field(MYSQL_RES *result)
{
  if (result->current_field >= result->field_count)
    return(NULL);
  return &result->fields[result->current_field++];
}

/**************************************************************************
**  Return next row of the query results
**************************************************************************/
MYSQL_ROW STDCALL
mysql_fetch_row(MYSQL_RES *res)
{
  DBUG_ENTER("mysql_fetch_row");
  if (!res->data)
  {						/* Unbufferred fetch */
    if (!res->eof)
    {
      if (!(read_one_row(res->handle,res->field_count,res->row, res->lengths)))
      {
	res->row_count++;
	DBUG_RETURN(res->current_row=res->row);
      }
      else
      {
	DBUG_PRINT("info",("end of data"));
	res->eof=1;
	res->handle->status=MYSQL_STATUS_READY;
	/* Don't clear handle in mysql_free_results */
	res->handle=0;
      }
    }
    DBUG_RETURN((MYSQL_ROW) NULL);
  }
  {
    MYSQL_ROW tmp;
    if (!res->data_cursor)
    {
      DBUG_PRINT("info",("end of data"));
      DBUG_RETURN(res->current_row=(MYSQL_ROW) NULL);
    }
    tmp = res->data_cursor->data;
    res->data_cursor = res->data_cursor->next;
    DBUG_RETURN(res->current_row=tmp);
  }
}

/**************************************************************************
** Get column lengths of the current row
** If one uses mysql_use_result, res->lengths contains the length information,
** else the lengths are calculated from the offset between pointers.
**************************************************************************/

ulong * STDCALL
mysql_fetch_lengths(MYSQL_RES *res)
{
  ulong *lengths,*prev_length;
  char *start;
  MYSQL_ROW column,end;

  if (!(column=res->current_row))
    return 0;					/* Something is wrong */
  if (res->data)
  {
    start=0;
    prev_length=0;				/* Keep gcc happy */
    lengths=res->lengths;
    for (end=column+res->field_count+1 ; column != end ; column++,lengths++)
    {
      if (!*column)
      {
	*lengths=0;				/* Null */
	continue;
      }
      if (start)				/* Found end of prev string */
	*prev_length= (uint) (*column-start-1);
      start= *column;
      prev_length=lengths;
    }
  }
  return res->lengths;
}

/**************************************************************************
** Move to a specific row and column
**************************************************************************/

void STDCALL
mysql_data_seek(MYSQL_RES *result, my_ulonglong row)
{
  MYSQL_ROWS	*tmp=0;
  DBUG_PRINT("info",("mysql_data_seek(%ld)",(long) row));
  if (result->data)
    for (tmp=result->data->data; row-- && tmp ; tmp = tmp->next) ;
  result->current_row=0;
  result->data_cursor = tmp;
}

/*************************************************************************
** put the row or field cursor one a position one got from mysql_row_tell()
** This doesn't restore any data. The next mysql_fetch_row or
** mysql_fetch_field will return the next row or field after the last used
*************************************************************************/

MYSQL_ROW_OFFSET STDCALL
mysql_row_seek(MYSQL_RES *result, MYSQL_ROW_OFFSET row)
{
  MYSQL_ROW_OFFSET return_value=result->data_cursor;
  result->current_row= 0;
  result->data_cursor= row;
  return return_value;
}


MYSQL_FIELD_OFFSET STDCALL
mysql_field_seek(MYSQL_RES *result, MYSQL_FIELD_OFFSET field_offset)
{
  MYSQL_FIELD_OFFSET return_value=result->current_field;
  result->current_field=field_offset;
  return return_value;
}

/*****************************************************************************
** List all databases
*****************************************************************************/

MYSQL_RES * STDCALL
mysql_list_dbs(MYSQL *mysql, const char *wild)
{
  char buff[255];
  DBUG_ENTER("mysql_list_dbs");

  append_wild(strmov(buff,"show databases"),buff+sizeof(buff),wild);
  if (mysql_query(mysql,buff))
    DBUG_RETURN(0);
  DBUG_RETURN (mysql_store_result(mysql));
}


/*****************************************************************************
** List all tables in a database
** If wild is given then only the tables matching wild is returned
*****************************************************************************/

MYSQL_RES * STDCALL
mysql_list_tables(MYSQL *mysql, const char *wild)
{
  char buff[255];
  DBUG_ENTER("mysql_list_tables");

  append_wild(strmov(buff,"show tables"),buff+sizeof(buff),wild);
  if (mysql_query(mysql,buff))
    DBUG_RETURN(0);
  DBUG_RETURN (mysql_store_result(mysql));
}


/**************************************************************************
** List all fields in a table
** If wild is given then only the fields matching wild is returned
** Instead of this use query:
** show fields in 'table' like "wild"
**************************************************************************/

MYSQL_RES * STDCALL
mysql_list_fields(MYSQL *mysql, const char *table, const char *wild)
{
  MYSQL_RES *result;
  MYSQL_DATA *query;
  char	     buff[257],*end;
  DBUG_ENTER("mysql_list_fields");
  DBUG_PRINT("enter",("table: '%s'  wild: '%s'",table,wild ? wild : ""));

  LINT_INIT(query);

  end=strmake(strmake(buff, table,128)+1,wild ? wild : "",128);
  if (simple_command(mysql,MYSQL_COM_FIELD_LIST,buff,(uint) (end-buff),1) ||
      !(query = read_rows(mysql,(MYSQL_FIELD*) 0,8)))
    DBUG_RETURN(NULL);

  free_old_query(mysql);
  if (!(result = (MYSQL_RES *) my_malloc(sizeof(MYSQL_RES),
					 MYF(MY_WME | MY_ZEROFILL))))
  {
    free_rows(query);
    DBUG_RETURN(NULL);
  }
  result->field_alloc=mysql->field_alloc;
  mysql->fields=0;
  result->field_count = (uint) query->rows;
  result->fields= unpack_fields(query,&result->field_alloc,
				result->field_count,1,
				(my_bool) test(mysql->server_capabilities &
					       CLIENT_LONG_FLAG));
  result->eof=1;
  DBUG_RETURN(result);
}

/* List all running processes (threads) in server */

MYSQL_RES * STDCALL
mysql_list_processes(MYSQL *mysql)
{
  MYSQL_DATA *fields;
  uint field_count;
  uchar *pos;
  DBUG_ENTER("mysql_list_processes");

  LINT_INIT(fields);
  if (simple_command(mysql,MYSQL_COM_PROCESS_INFO,0,0,0))
    DBUG_RETURN(0);
  free_old_query(mysql);
  pos=(uchar*) mysql->net.read_pos;
  field_count=(uint) net_field_length(&pos);
  if (!(fields = read_rows(mysql,(MYSQL_FIELD*) 0,5)))
    DBUG_RETURN(NULL);
  if (!(mysql->fields=unpack_fields(fields,&mysql->field_alloc,field_count,0,
				    (my_bool) test(mysql->server_capabilities &
						   CLIENT_LONG_FLAG))))
    DBUG_RETURN(0);
  mysql->status=MYSQL_STATUS_GET_RESULT;
  mysql->field_count=field_count;
  DBUG_RETURN(mysql_store_result(mysql));
}


int  STDCALL
mysql_create_db(MYSQL *mysql, const char *db)
{
  DBUG_ENTER("mysql_createdb");
  DBUG_PRINT("enter",("db: %s",db));
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_CREATE_DB,db, (uint) strlen(db),0));
}


int  STDCALL
mysql_drop_db(MYSQL *mysql, const char *db)
{
  DBUG_ENTER("mysql_drop_db");
  DBUG_PRINT("enter",("db: %s",db));
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_DROP_DB,db,(uint) strlen(db),0));
}


int STDCALL
mysql_shutdown(MYSQL *mysql)
{
  DBUG_ENTER("mysql_shutdown");
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_SHUTDOWN,0,0,0));
}


int STDCALL
mysql_refresh(MYSQL *mysql,uint options)
{
  uchar bits[1];
  DBUG_ENTER("mysql_refresh");
  bits[0]= (uchar) options;
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_REFRESH,(char*) bits,1,0));
}

int STDCALL
mysql_kill(MYSQL *mysql,ulong pid)
{
  char buff[12];
  DBUG_ENTER("mysql_kill");
  int4store(buff,pid);
  /* if we kill our own thread, reading the response packet will fail */ 
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_PROCESS_KILL,buff,4,0));
}


int STDCALL
mysql_dump_debug_info(MYSQL *mysql)
{
  DBUG_ENTER("mysql_dump_debug_info");
  DBUG_RETURN(simple_command(mysql,MYSQL_COM_DEBUG,0,0,0));
}

char * STDCALL
mysql_stat(MYSQL *mysql)
{
  DBUG_ENTER("mysql_stat");
  if (simple_command(mysql,MYSQL_COM_STATISTICS,0,0,0))
    return mysql->net.last_error;
  mysql->net.read_pos[mysql->packet_length]=0;	/* End of stat string */
  if (!mysql->net.read_pos[0])
  {
    SET_CLIENT_ERROR(mysql, CR_WRONG_HOST_INFO , unknown_sqlstate, 0);
    return mysql->net.last_error;
  }
  DBUG_RETURN((char*) mysql->net.read_pos);
}


int STDCALL
mysql_ping(MYSQL *mysql)
{
  int rc;
  DBUG_ENTER("mysql_ping");
  rc= simple_command(mysql,MYSQL_COM_PING,0,0,0);

  /* if connection was terminated and reconnect is true, try again */
  if (rc!=0  && mysql->reconnect)
    rc= simple_command(mysql,MYSQL_COM_PING,0,0,0);
  return rc;
}


char * STDCALL
mysql_get_server_info(MYSQL *mysql)
{
  return((char*) mysql->server_version);
}

unsigned long STDCALL mysql_get_server_version(MYSQL *mysql)
{
  long major, minor, patch;
  char *p;

  if (!(p = mysql->server_version)) {
    return 0;
  }

  major = strtol(p, &p, 10);
  p += 1; /* consume the dot */
  minor = strtol(p, &p, 10);
  p += 1; /* consume the dot */
  patch = strtol(p, &p, 10);

  return (unsigned long)(major * 10000L + (unsigned long)(minor * 100L + patch));
}



char * STDCALL
mysql_get_host_info(MYSQL *mysql)
{
  return(mysql->host_info);
}


uint STDCALL
mysql_get_proto_info(MYSQL *mysql)
{
  return (mysql->protocol_version);
}

const char * STDCALL
mysql_get_client_info(void)
{
  return (char*) MYSQL_CLIENT_VERSION;
}


int STDCALL
mysql_options(MYSQL *mysql,enum mysql_option option, const char *arg)
{
  DBUG_ENTER("mysql_option");
  DBUG_PRINT("enter",("option: %d",(int) option));
  switch (option) {
  case MYSQL_OPT_CONNECT_TIMEOUT:
    mysql->options.connect_timeout= *(uint*) arg;
    break;
  case MYSQL_OPT_COMPRESS:
    mysql->options.compress= 1;			/* Remember for connect */
    mysql->options.client_flag|= CLIENT_COMPRESS;
    break;
  case MYSQL_OPT_NAMED_PIPE:
    mysql->options.named_pipe=1;		/* Force named pipe */
    break;
  case MYSQL_OPT_LOCAL_INFILE:			/* Allow LOAD DATA LOCAL ?*/
    if (!arg || test(*(uint*) arg))
      mysql->options.client_flag|= CLIENT_LOCAL_FILES;
    else
      mysql->options.client_flag&= ~CLIENT_LOCAL_FILES;
    break;
  case MYSQL_INIT_COMMAND:
    my_free(mysql->options.init_command,MYF(MY_ALLOW_ZERO_PTR));
    mysql->options.init_command=my_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_READ_DEFAULT_FILE:
    my_free(mysql->options.my_cnf_file,MYF(MY_ALLOW_ZERO_PTR));
    mysql->options.my_cnf_file=my_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_READ_DEFAULT_GROUP:
    my_free(mysql->options.my_cnf_group,MYF(MY_ALLOW_ZERO_PTR));
    mysql->options.my_cnf_group=my_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_SET_CHARSET_DIR:
    /* not supported in this version. Since all character sets 
       are internally available, we don't throw an error */
    break;
  case MYSQL_SET_CHARSET_NAME:
    my_free(mysql->options.charset_name,MYF(MY_ALLOW_ZERO_PTR));
    mysql->options.charset_name=my_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_OPT_RECONNECT:
    mysql->reconnect= *(uint *)arg;
    break;
  case MYSQL_OPT_PROTOCOL:
#ifdef _WIN32
    if (*(uint *)arg > MYSQL_PROTOCOL_PIPE)
#else
    if (*(uint *)arg > MYSQL_PROTOCOL_SOCKET)
#endif
      DBUG_RETURN(-1);
    mysql->options.protocol= *(uint *)arg;
    break;
  case MYSQL_OPT_READ_TIMEOUT:
    mysql->options.read_timeout= *(uint *)arg;
    break;
  case MYSQL_OPT_WRITE_TIMEOUT:
    mysql->options.write_timeout= *(uint *)arg;
    break;
  case MYSQL_REPORT_DATA_TRUNCATION:
    mysql->options.report_data_truncation= *(uint *)arg;
    break;
  case MYSQL_OPT_PROGRESS_CALLBACK:
    if (!mysql->options.extension)
      mysql->options.extension= (struct st_mysql_options_extention *)
        my_malloc(sizeof(struct st_mysql_options_extention),
                  MYF(MY_WME | MY_ZEROFILL));
    if (mysql->options.extension)
      mysql->options.extension->report_progress=
        (void (*)(const MYSQL *, uint, uint, double, const char *, uint)) arg;
    break;
  default:
    DBUG_RETURN(-1);
  }
  DBUG_RETURN(0);
}

/****************************************************************************
** Functions to get information from the MySQL structure
** These are functions to make shared libraries more usable.
****************************************************************************/

/* MYSQL_RES */
my_ulonglong STDCALL mysql_num_rows(MYSQL_RES *res)
{
  return res->row_count;
}

unsigned int STDCALL mysql_num_fields(MYSQL_RES *res)
{
  return res->field_count;
}

/* deprecated */
my_bool STDCALL mysql_eof(MYSQL_RES *res)
{
  return res->eof;
}

MYSQL_FIELD * STDCALL mysql_fetch_field_direct(MYSQL_RES *res,uint fieldnr)
{
  return &(res)->fields[fieldnr];
}

MYSQL_FIELD * STDCALL mysql_fetch_fields(MYSQL_RES *res)
{
  return (res)->fields;
}

MYSQL_ROWS * STDCALL mysql_row_tell(MYSQL_RES *res)
{
  return res->data_cursor;
}

uint STDCALL mysql_field_tell(MYSQL_RES *res)
{
  return (res)->current_field;
}

/* MYSQL */

unsigned int STDCALL mysql_field_count(MYSQL *mysql)
{
  return mysql->field_count;
}

my_ulonglong STDCALL mysql_affected_rows(MYSQL *mysql)
{
  return (mysql)->affected_rows;
}

my_bool STDCALL mysql_autocommit(MYSQL *mysql, my_bool mode)
{
  DBUG_ENTER("mysql_autocommit");
  DBUG_RETURN((my_bool) mysql_real_query(mysql, (mode) ? "SET autocommit=1" : 
                                         "SET autocommit=0", 16)); 
}

my_bool STDCALL mysql_commit(MYSQL *mysql)
{
  DBUG_ENTER("mysql_commit");
  DBUG_RETURN((my_bool)mysql_real_query(mysql, "COMMIT", sizeof("COMMIT")));
}

my_bool STDCALL mysql_rollback(MYSQL *mysql)
{
  DBUG_ENTER("mysql_rollback");
  DBUG_RETURN((my_bool)mysql_real_query(mysql, "ROLLBACK", sizeof("ROLLBACK")));
}

my_ulonglong STDCALL mysql_insert_id(MYSQL *mysql)
{
  return (mysql)->insert_id;
}

uint STDCALL mysql_errno(MYSQL *mysql)
{
  return (mysql)->net.last_errno;
}

char * STDCALL mysql_error(MYSQL *mysql)
{
  return (mysql)->net.last_error;
}

char *STDCALL mysql_info(MYSQL *mysql)
{
  return (mysql)->info;
}

my_bool STDCALL mysql_more_results(MYSQL *mysql)
{
  DBUG_ENTER("mysql_more_results");
  DBUG_RETURN(test(mysql->server_status & SERVER_MORE_RESULTS_EXIST)); 
}

int STDCALL mysql_next_result(MYSQL *mysql)
{
  DBUG_ENTER("mysql_next_result");

  /* make sure communication is not blocking */
  if (mysql->status != MYSQL_STATUS_READY)
  {
    SET_CLIENT_ERROR(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate, 0);
    DBUG_RETURN(1);
  }

  /* clear error, and mysql status variables */
  CLEAR_CLIENT_ERROR(mysql);
  mysql->affected_rows = (ulonglong) ~0;

  if (mysql->server_status & SERVER_MORE_RESULTS_EXIST)
  {
     DBUG_RETURN(mysql_read_query_result(mysql));
  }

  DBUG_RETURN(-1);
}

ulong STDCALL mysql_thread_id(MYSQL *mysql)
{
  return (mysql)->thread_id;
}

const char * STDCALL mysql_character_set_name(MYSQL *mysql)
{
  return mysql->charset->csname;
}


uint STDCALL mysql_thread_safe(void)
{
#ifdef THREAD
  return 1;
#else
  return 0;
#endif
}

/****************************************************************************
** Some support functions
****************************************************************************/

/*
** Add escape characters to a string (blob?) to make it suitable for a insert
** to should at least have place for length*2+1 chars
** Returns the length of the to string
*/

ulong STDCALL
mysql_escape_string(char *to,const char *from, ulong length)
{
    return (ulong)mysql_cset_escape_slashes(default_charset_info, to, from, length);
}

ulong STDCALL
mysql_real_escape_string(MYSQL *mysql, char *to,const char *from,
			 ulong length)
{
  if (mysql->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
    return (ulong)mysql_cset_escape_quotes(mysql->charset, to, from, length);
  else
    return (ulong)mysql_cset_escape_slashes(mysql->charset, to, from, length);
}

void STDCALL
myodbc_remove_escape(MYSQL *mysql,char *name)
{
  char *to;
  my_bool use_mb_flag= (mysql->charset->char_maxlen > 1);
  char *end= 0;
  if (use_mb_flag)
    for (end=name; *end ; end++) ;

  for (to=name ; *name ; name++)
  {
    int l;
    if (use_mb_flag && (l = mysql->charset->mb_valid(name , end)))
    {
      while (l--)
	*to++ = *name++;
      name--;
      continue;
    }
    if (*name == '\\' && name[1])
      name++;
    *to++= *name;
  }
  *to=0;
}

void STDCALL mysql_get_character_set_info(MYSQL *mysql, MY_CHARSET_INFO *cs)
{
  DBUG_ENTER("mysql_get_character_set_info");

  if (!cs)
    DBUG_VOID_RETURN;

  cs->number= mysql->charset->nr;
  cs->csname=  mysql->charset->csname;
  cs->name= mysql->charset->name;
  cs->state= 0;
  cs->comment= NULL;
  cs->dir= NULL;
  cs->mbminlen= mysql->charset->char_minlen;
  cs->mbmaxlen= mysql->charset->char_maxlen;

  DBUG_VOID_RETURN;
}

int STDCALL mysql_set_character_set(MYSQL *mysql, const char *csname)
{
  const CHARSET_INFO *cs;
  DBUG_ENTER("mysql_set_character_set");

  if (!csname)
    goto error;

  if ((cs= mysql_find_charset_name(csname)))
  {
    char buff[64];

    my_snprintf(buff, 63, "SET NAMES %s", cs->csname);
    if (!mysql_real_query(mysql, buff, (uint)strlen(buff)))
    {
      mysql->charset= cs;
      DBUG_RETURN(0);
    }
  }

error:
  my_set_error(mysql, CR_CANT_READ_CHARSET, SQLSTATE_UNKNOWN, 
               0, csname, "compiled_in");
  DBUG_RETURN(mysql->net.last_errno);
}

unsigned int STDCALL mysql_warning_count(MYSQL *mysql)
{
  return mysql->warning_count;
}

const char * STDCALL mysql_sqlstate(MYSQL *mysql)
{
  return mysql->net.sqlstate;
}

int STDCALL mysql_server_init(int argc __attribute__((unused)),
			      char **argv __attribute__((unused)),
			      char **groups __attribute__((unused)))
{
  int rc= 0;

  if (!mysql_client_init)
  {
    mysql_client_init=1;
    my_init();					/* Will init threads */
    init_client_errs();
    if (mysql_client_plugin_init())
      return 1;
    if (!mysql_port)
    {
      struct servent *serv_ptr;
      char *env;

	  mysql_port = MYSQL_PORT;
      if ((serv_ptr = getservbyname("mysql", "tcp")))
        mysql_port = (uint) ntohs((ushort) serv_ptr->s_port);
      if ((env = getenv("MYSQL_TCP_PORT")))
        mysql_port =(uint) atoi(env);
    }
    if (!mysql_unix_port)
    {
      char *env;
#ifdef _WIN32
      mysql_unix_port = (char*) MYSQL_NAMEDPIPE;
#else
      mysql_unix_port = (char*) MYSQL_UNIX_ADDR;
#endif
      if ((env = getenv("MYSQL_UNIX_PORT")))
	mysql_unix_port = env;
    }
    mysql_debug(NullS);
#if defined(SIGPIPE) && !defined(THREAD) && !defined(_WIN32)
    (void) signal(SIGPIPE,SIG_IGN);
#endif
  }
#ifdef THREAD
  else
    rc= mysql_thread_init();
#endif
  if (!mysql_ps_subsystem_initialized)
    mysql_init_ps_subsystem(); 
  return(rc);
}

void STDCALL mysql_server_end()
{
  if (!mysql_client_init)
    return;
#ifdef HAVE_OPENSSL
  my_ssl_end();
#endif  

  mysql_client_plugin_deinit();

  if (my_init_done)
    my_end(0);
#ifdef THREAD
  else
    mysql_thread_end();
#endif
  mysql_client_init= 0;
  my_init_done= 0;
}

my_bool STDCALL mysql_thread_init()
{
#ifdef THREAD
  return my_thread_init();
#endif
  return 0;
}

void STDCALL mysql_thread_end()
{
  #ifdef THREAD
  my_thread_end();
  #endif
}

int STDCALL mysql_set_server_option(MYSQL *mysql, 
                                    enum enum_mysql_set_option option)
{
  char buffer[2];
  DBUG_ENTER("mysql_set_server_option");
  int2store(buffer, (uint)option);
  DBUG_RETURN(simple_command(mysql, MYSQL_COM_SET_OPTION, buffer, sizeof(buffer), 0));
}

ulong STDCALL mysql_get_client_version(void)
{
  return MYSQL_VERSION_ID;
}

ulong STDCALL mysql_hex_string(char *to, const char *from,
                               unsigned long len)
{
  char *start= to;
  char hexdigits[]= "012345679ABCDEF";

  while (len--)
  {
    *to++= hexdigits[*from >> 4];
    *to++= hexdigits[*from & 0x0F];
    from++;
  }
  *to= 0;
  return (to - start);
}
