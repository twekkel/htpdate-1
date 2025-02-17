/*
	htpdate v1.2.6

	Author: Angelo Compagnucci <angelo.compagnucci@gmail.com>
	https://github.com/angeloc/htpdate

	Original author:
	Eddy Vervest <eddy@vervest.org>
	http://www.vervest.org/htp

	Synchronize local workstation with time offered by remote web servers

	This program works with the timestamps return by web servers,
	formatted as specified by HTTP/1.1 (RFC 2616, RFC 1123).

	Example usage:

	Debug mode (shows raw timestamps, round trip time (RTT) and
	time difference):

	~# htpdate -d www.example.com

	Adjust time smoothly:

	~# htpdate -a www.example.com

	...see man page for more details


	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
	http://www.gnu.org/copyleft/gpl.html
*/

/* Needed to avoid implicit warnings from strptime */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <syslog.h>
#include <stdarg.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>

#ifdef ENABLE_HTTPS
#include <openssl/ssl.h>
#endif

#if defined BSD || defined __FreeBSD__
#define adjtimex ntp_adjtime
#endif

#define VERSION 			"1.2.6"
#define	MAX_HTTP_HOSTS			15			/* 16 web servers */
#define	DEFAULT_HTTP_PORT		"80"
#define	DEFAULT_PROXY_PORT		"8080"
#define	DEFAULT_IP_VERSION		PF_UNSPEC		/* IPv6 and IPv4 */
#define	DEFAULT_HTTP_VERSION		"1"			/* HTTP/1.1 */
#define	DEFAULT_TIME_LIMIT		31536000		/* 1 year */
#define	NO_TIME_LIMIT			-1
#define	DEFAULT_MIN_SLEEP		1800			/* 30 minutes */
#define	DEFAULT_MAX_SLEEP		115200			/* 32 hours */
#define	MAX_DRIFT			32768000		/* 500 PPM */
#define	MAX_ATTEMPT			2			/* Poll attempts */
#define	DEFAULT_PID_FILE		"/var/run/htpdate.pid"
#define	URLSIZE				128
#define	BUFFERSIZE			1024

#define sign(x) (x < 0 ? (-1) : 1)


/* By default we turn off "debug" and "log" mode  */
static int		debug = 0;
static int		logmode = 0;


/* Make mktime timezone agnostic, see manpage timegm */
time_t gmtmktime (struct tm *tm)
{
	char *tz;
	time_t result;

	/* Temporarily set timezone to UTC for conversion */
	tz = getenv("TZ");
	if (tz) {
		tz = strdup (tz);
	}
	setenv("TZ", "", 1);
	tzset();

	result = mktime (tm);

	/* Restore timezone */
	if (tz) {
		setenv("TZ", tz, 1);
		free (tz);
	} else {
		unsetenv("TZ");
	}
	tzset();

	return result;
}


/* Insertion sort is more efficient (and smaller) than qsort for small lists */
static void insertsort( int a[], int length )
{
	int i, j, value;

	for ( i = 1; i < length; i++ ) {
		value = a[i];
		for ( j = i - 1; j >= 0 && a[j] > value; j-- )
			a[j+1] = a[j];
		a[j+1] = value;
	}
}


/* Split argument in hostname/IP-address and TCP port
   Supports IPv6 literal addresses, RFC 2732.
*/
static void splithostport( char **host, char **port )
{
	char    *rb, *rc, *lb, *lc;

	lb = strchr( *host, '[' );
	rb = strrchr( *host, ']' );
	lc = strchr( *host, ':' );
	rc = strrchr( *host, ':' );

	/* A (litteral) address with "https://" prefix */
	if (strstr(*host, "https://") == *host) {
		rc[0] = '\0';
		*host = *host + strlen("https://");
		*port = "443\0";
		return;
	}

	/* A (litteral) address with "http://" prefix */
	if (strstr(*host, "http://") == *host) {
		rc[0] = '\0';
		*host = *host + strlen("http://");
		*port = "80\0";
		return;
	}

	/* A (litteral) IPv6 address with portnumber */
	if ( rb < rc && lb != NULL && rb != NULL ) {
		rb[0] = '\0';
		*port = rc + 1;
		*host = lb + 1;
		return;
	}

	/* A (litteral) IPv6 address without portnumber */
	if ( rb != NULL && lb != NULL ) {
		rb[0] = '\0';
		*host = lb + 1;
		return;
	}

	/* A IPv4 address or hostname with portnumber */
	if ( rc != NULL && lc == rc ) {
		rc[0] = '\0';
		*port = rc + 1;
		return;
	}
}


/* Printlog is a slighty modified version from the one used in rdate */
static void printlog( int is_error, char *format, ... )
{
	va_list args;
	char buf[128];

	va_start(args, format);
	(void) vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	if ( logmode )
		syslog(is_error?LOG_WARNING:LOG_INFO, "%s", buf);
	else
		fprintf(is_error?stderr:stdout, "%s\n", buf);
}


/* Drop or elevate privileges */
static void swuid( int id )
{
	if ( seteuid( id ) ) {
		printlog( 1, "seteuid() %i", id );
		exit(1);
	}
}
static void swgid( int id )
{
	if ( setegid( id ) ) {
		printlog( 1, "setegid() %i", id );
		exit(1);
	}
}

static int getHTTP (int server_s, char *buffer)
{
	int ret;

	/* Send HEAD request */
	ret = send(server_s, buffer, strlen(buffer), 0);

	if (ret < 0) {
		printlog( 1, "Error sending" );
		return 0;
	}

	/* Receive data from the web server
	   The return code from recv() is the number of bytes received
	*/
	ret = recv(server_s, buffer, BUFFERSIZE - 1, 0) != -1;

	close( server_s );

	return ret;
}

#ifdef ENABLE_HTTPS
static int getHTTPS (int server_s, char *buffer)
{
	int ret;
	SSL_CTX *ssl_ctx = SSL_CTX_new (TLS_method());

	SSL *conn = SSL_new(ssl_ctx);
	SSL_set_fd(conn, server_s);

	int err = SSL_connect(conn);
	if (err != 1)
		return 0;

	ret = SSL_write(conn, buffer, BUFFERSIZE -1);

	if (ret <= 0) {
		printlog( 1, "Error sending" );
		return 0;
	}

	ret = SSL_read(conn, buffer, BUFFERSIZE - 1) > 0;

	SSL_shutdown(conn);

	return ret;
}
#endif

static long getHTTPdate( char *host, char *port, char *proxy, char *proxyport, char *httpversion, int ipversion, int when )
{
	int			server_s;
	int			rc;
	struct addrinfo		hints, *res, *res0;
	struct tm		tm;
	struct timeval		timevalue = {LONG_MAX, 0};
	struct timeval		timeofday;
	struct timespec		sleepspec, remainder;
	long			rtt;
	char			buffer[BUFFERSIZE] = { '\0' };
	char			remote_time[25] = { '\0' };
	char			url[URLSIZE] = { '\0' };
	char			*pdate = NULL;

#ifdef ENABLE_HTTPS
	int			port_is_https = 0;

	if (strncmp (port, "443", 3) == 0)
		port_is_https = 1;
#endif

	/* Connect to web server via proxy server or directly */
	memset( &hints, 0, sizeof(hints) );
	switch( ipversion ) {
	case 4:					/* IPv4 only */
		hints.ai_family = AF_INET;
		break;
	case 6:					/* IPv6 only */
		hints.ai_family = AF_INET6;
		break;
	default:				/* Support IPv6 and IPv4 name resolution */
		hints.ai_family = PF_UNSPEC;
	}
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	if ( proxy == NULL ) {
		rc = getaddrinfo( host, port, &hints, &res0 );
	} else {
		snprintf( url, URLSIZE, "http://%s:%s", host, port);
		rc = getaddrinfo( proxy, proxyport, &hints, &res0 );
	}

	/* Was the hostname and service resolvable? */
	if ( rc ) {
		printlog( 1, "%s host or service unavailable", host );
		return(0);				/* Assume correct time */
	}

	/* Build a combined HTTP/1.0 and 1.1 HEAD request
	   Pragma: no-cache, "forces" an HTTP/1.0 and 1.1 compliant
	   web server to return a fresh timestamp
	   Connection: close, allows the server the immediately close the
	   connection after sending the response.
	*/
	snprintf(buffer, BUFFERSIZE, "HEAD %s/ HTTP/1.%s\r\nHost: %s\r\nUser-Agent: htpdate/"VERSION"\r\nPragma: no-cache\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", url, httpversion, host);

	/* Loop through the available canonical names */
	res = res0;
	do {
		server_s = socket( res->ai_family, res->ai_socktype, res->ai_protocol );
		if ( server_s < 0 ) {
			continue;
		}

		rc = connect( server_s, res->ai_addr, res->ai_addrlen );
		if ( rc ) {
			close( server_s);
			server_s = -1;
			continue;
		}

		break;
	} while ( ( res = res->ai_next ) );

	freeaddrinfo(res0);

	if ( rc ) {
		printlog( 1, "%s connection failed", host );
		return(0);				/* Assume correct time */
	}

	/* Initialize timer */
	gettimeofday(&timeofday, NULL);

	/* Initialize RTT (start of measurement) */
	rtt = timeofday.tv_sec;

	/* Wait till we reach the desired time, "when" */
	sleepspec.tv_sec = 0;
	if ( when >= timeofday.tv_usec ) {
		sleepspec.tv_nsec = ( when - timeofday.tv_usec ) * 1000;
	} else {
		sleepspec.tv_nsec = ( 1000000 + when - timeofday.tv_usec ) * 1000;
		rtt++;
	}
	nanosleep( &sleepspec, &remainder );

#ifdef ENABLE_HTTPS
	if (port_is_https)
		rc = getHTTPS(server_s, buffer);
	else
#endif
		rc = getHTTP(server_s, buffer);

	if ( !rc )
		printlog( 1, "error getting data from %s:%s", host, port );

	if ( rc ) {
		/* Assuming that network delay (server->htpdate) is neglectable,
		   the received web server time "should" match the local time.

		   From RFC 2616 paragraph 14.18
		   ...
		   It SHOULD represent the best available approximation
		   of the date and time of message generation, unless the
		   implementation has no means of generating a reasonably
		   accurate date and time.
		   ...
		*/

		gettimeofday(&timeofday, NULL);

		/* rtt contains round trip time in micro seconds, now! */
		rtt = ( timeofday.tv_sec - rtt ) * 1000000 + \
		      timeofday.tv_usec - when;

		/* Look for the line that contains Date: */
		if ( ((pdate = strstr(buffer, "Date: ")) != NULL ||
		      (pdate = strstr(buffer, "date: ")) != NULL) &&
		     strlen( pdate ) >= 35 ) {
			strncpy(remote_time, pdate + 11, 24);

			memset(&tm, 0, sizeof(struct tm));
			if ( strptime( remote_time, "%d %b %Y %T", &tm) != NULL) {
				timevalue.tv_sec = gmtmktime(&tm);
			} else {
				printlog( 1, "%s unknown time format", host );
			}

			/* Print host, raw timestamp, round trip time */
			if ( debug )
				printlog( 0, "%-25s %s %s (%.3f) => %li", host, port, remote_time, \
				          rtt * 1e-6, timevalue.tv_sec - timeofday.tv_sec );

		} else {
			printlog( 1, "%s no timestamp", host );
		}

	}						/* bytes received */

	/* Return the time delta between web server time (timevalue)
	   and system time (timeofday)
	*/
	return( timevalue.tv_sec - timeofday.tv_sec );

}


static int setclock( double timedelta, int setmode )
{
	struct timeval		timeofday;

	if ( timedelta == 0 ) {
		printlog( 0, "No time correction needed" );
		return(0);
	}

	switch ( setmode ) {

	case 0:						/* No time adjustment, just print time */
		printlog( 0, "Offset %.3f seconds", timedelta );
		return(0);

	case 1:						/* Adjust time smoothly */
		timeofday.tv_sec  = (long)timedelta;
		timeofday.tv_usec = (long)((timedelta - timeofday.tv_sec) * 1000000);

		printlog( 0, "Adjusting %.3f seconds", timedelta );

		/* Become root */
		swuid(0);
		return( adjtime(&timeofday, NULL) );

	case 2:					/* Set time */
		printlog( 0, "Setting %.3f seconds", timedelta );

		gettimeofday( &timeofday, NULL );
		timedelta += ( timeofday.tv_sec + timeofday.tv_usec*1e-6 );

		timeofday.tv_sec  = (long)timedelta;
		timeofday.tv_usec = (long)(timedelta - timeofday.tv_sec) * 1000000;

		printlog( 0, "Set: %s", asctime(localtime(&timeofday.tv_sec)) );

		/* Become root */
		swuid(0);
		return( settimeofday(&timeofday, NULL) );

	case 3:					/* Set frequency, but first an adjust */
		return( setclock( timedelta, 1 ) );


	default:
		return(-1);

	}

}


static int htpdate_adjtimex( double drift )
{
	struct timex		tmx;
	long			freq;

	/* Read current kernel frequency */
	tmx.modes = 0;
	adjtimex(&tmx);

	/* Calculate new frequency */
	freq = (long)(65536e6 * drift);

	/* Take the average of current and new drift values */
	tmx.freq = tmx.freq + (freq >> 1);
	if ( (tmx.freq < -MAX_DRIFT) || (tmx.freq > MAX_DRIFT) )
		tmx.freq = sign(tmx.freq) * MAX_DRIFT;

	printlog( 0, "Adjusting frequency %li", tmx.freq );
	tmx.modes = MOD_FREQUENCY;

	/* Become root */
	swuid(0);
	return( adjtimex(&tmx) );

}


static void showhelp()
{
	puts("htpdate version "VERSION"\n\
Usage: htpdate [-046abdhlqstxD] [-i pid file] [-m minpoll] [-M maxpoll]\n\
               [-p precision] [-P <proxyserver>[:port]] [-u user[:group]]\n\
               <host[:port]> ...\n\n\
  -0    HTTP/1.0 request\n\
  -4    Force IPv4 name resolution only\n\
  -6    Force IPv6 name resolution only\n\
  -a    adjust time smoothly\n\
  -b    burst mode\n\
  -d    debug mode\n\
  -D    daemon mode\n\
  -F    foreground mode\n\
  -h    help\n\
  -i    pid file\n\
  -l    use syslog for output\n\
  -m    minimum poll interval\n\
  -M    maximum poll interval\n\
  -p    precision (ms)\n\
  -P    proxy server\n\
  -q    query only, don't make time changes (default)\n\
  -s    set time\n\
  -t    turn off sanity time check\n\
  -u    run daemon as user\n\
  -x    adjust kernel clock\n\
  host  web server hostname or ip address (maximum of 16)\n\
  port  port number (default 80 and 8080 for proxy server)\n");

	return;
}


/* Run htpdate in daemon mode */
static void runasdaemon( char *pidfile )
{
	FILE		*pid_file;
	pid_t		pid;

	/* Check if htpdate is already running (pid exists)*/
	pid_file = fopen(pidfile, "r");
	if ( pid_file ) {
		fputs( "htpdate already running\n", stderr );
		exit(1);
	}

	pid = fork();
	if ( pid < 0 ) {
		fputs( "fork()\n", stderr );
		exit(1);
	}

	if ( pid > 0 ) {
		exit(0);
	}

	/* Create a new SID for the child process */
	if ( setsid () < 0 )
		exit(1);

	/* Close out the standard file descriptors */
	close( STDIN_FILENO );
	close( STDOUT_FILENO );
	close( STDERR_FILENO );

	signal(SIGHUP, SIG_IGN);

	/* Change the file mode mask */
	umask(0);

	/* Change the current working directory */
	if ( chdir("/") < 0 ) {
		printlog( 1, "chdir()" );
		exit(1);
	}

	/* Second fork, to become the grandchild */
	pid = fork();
	if ( pid < 0 ) {
		printlog( 1, "fork()" );
		exit(1);
	}

	if ( pid > 0 ) {
		/* Write a pid file */
		pid_file = fopen( pidfile, "w" );
		if ( !pid_file ) {
			printlog( 1, "Error writing pid file" );
			exit(1);
		} else {
			fprintf( pid_file, "%u\n", (unsigned int)pid );
			fclose( pid_file );
		}
		printlog( 0, "htpdate version "VERSION" started" );
		exit(0);
	}

}


int main( int argc, char *argv[] )
{
	char			*host = NULL, *proxy = NULL, *proxyport = NULL;
	char			*port = NULL;
	char			*hostport = NULL;
	char			*httpversion = DEFAULT_HTTP_VERSION;
	char			*pidfile = DEFAULT_PID_FILE;
	char			*user = NULL, *userstr = NULL, *group = NULL;
	long long		sumtimes;
	double			timeavg, drift = 0;
	int			timedelta[(MAX_HTTP_HOSTS+1)*(MAX_HTTP_HOSTS+1)-1], timestamp;
	int			numservers, validtimes, goodtimes, mean;
	int			nap = 0, when = 500000, precision = 0;
	int			setmode = 0, burstmode = 0, try, offsetdetect;
	int			i, burst, param;
	int			daemonize = 0;
	int			foreground = 0;
	int			ipversion = DEFAULT_IP_VERSION;
	int			timelimit = DEFAULT_TIME_LIMIT;
	int			minsleep = DEFAULT_MIN_SLEEP;
	int			maxsleep = DEFAULT_MAX_SLEEP;
	int			sleeptime = minsleep;
	int			sw_uid = 0, sw_gid = 0;
	time_t			starttime = 0;

	struct passwd		*pw;
	struct group		*gr;

	extern char		*optarg;
	extern int		optind;


	/* Parse the command line switches and arguments */
	while ( (param = getopt(argc, argv, "046abdhi:lm:p:qstu:xDFM:P:") ) != -1)
		switch( param ) {

		case '0':			/* HTTP/1.0 */
			httpversion = "0";
			break;
		case '4':			/* IPv4 only */
			ipversion = 4;
			break;
		case '6':			/* IPv6 only */
			ipversion = 6;
			break;
		case 'a':			/* adjust time */
			setmode = 1;
			break;
		case 'b':			/* burst mode */
			burstmode = 1;
			break;
		case 'd':			/* turn debug on */
			debug = 1;
			break;
		case 'h':			/* show help */
			showhelp();
			exit(0);
		case 'i':			/* pid file */
			pidfile = (char *)optarg;
			break;
		case 'l':			/* log mode */
			logmode = 1;
			break;
		case 'm':			/* minimum poll interval */
			if ( ( minsleep = atoi(optarg) ) <= 0 ) {
				fputs( "Invalid sleep time\n", stderr );
				exit(1);
			}
			sleeptime = minsleep;
			break;
		case 'p':			/* precision */
			precision = atoi(optarg) ;
			if ( (precision <= 0) || (precision >= 500) ) {
				fputs( "Invalid precision\n", stderr );
				exit(1);
			}
			precision *= 1000;
			break;
		case 'q':			/* query only */
			break;
		case 's':			/* set time */
			setmode = 2;
			break;
		case 't':			/* disable "sanity" time check */
			timelimit = NO_TIME_LIMIT;
			break;
		case 'u':			/* drop root privileges and run as user */
			user = (char *)optarg;
			userstr = strchr( user, ':' );
			if ( userstr != NULL ) {
				userstr[0] = '\0';
				group = userstr + 1;
			}
			if ( (pw = getpwnam(user)) != NULL ) {
				sw_uid = pw->pw_uid;
				sw_gid = pw->pw_gid;
			} else {
				printf( "Unknown user %s\n", user );
				exit(1);
			}
			if ( group != NULL ) {
				if ( (gr = getgrnam(group)) != NULL ) {
					sw_gid = gr->gr_gid;
				} else {
					printf( "Unknown group %s\n", group );
					exit(1);
				}
			}
			break;
		case 'x':			/* adjust time and "kernel" */
			setmode = 3;
			break;
		case 'D':			/* run as daemon */
			daemonize = 1;
			logmode = 1;
			break;
		case 'F':			/* run in the foreground */
			foreground = 1;
			break;
		case 'M':			/* maximum poll interval */
			if ( ( maxsleep = atoi(optarg) ) <= 0 ) {
				fputs( "Invalid sleep time\n", stderr );
				exit(1);
			}
			break;
		case 'P':
			proxy = (char *)optarg;
			proxyport = DEFAULT_PROXY_PORT;
			splithostport( &proxy, &proxyport );
			break;
		case '?':
			return 1;
		default:
			abort();
		}

	/* Display help page, if no servers are specified */
	if ( argv[optind] == NULL ) {
		showhelp();
		exit(1);
	}

	/* Exit if too many servers are specified */
	numservers = argc - optind;
	if ( numservers > MAX_HTTP_HOSTS ) {
		fputs( "Too many servers\n", stderr );
		exit(1);
	}

	/* One must be "root" to change the system time */
	if ( (getuid() != 0) && (setmode || daemonize || foreground) ) {
		fputs( "Only root can change time\n", stderr );
		exit(1);
	}

	/* Run as a daemonize when -D is set */
	if ( daemonize ) {
		runasdaemon( pidfile );
	}

	/* Query only mode doesn't exist in daemon or foreground mode */
	if ( (daemonize || foreground) && !setmode ) {
		setmode = 1;
	}

	/* Now we are root, we drop the privileges (if specified) */
	if ( sw_gid ) swgid( sw_gid );
	if ( sw_uid ) swuid( sw_uid );

	/* In case we have more than one web server defined, we
	   spread the polls equal within a second and take a "nap" in between
	*/
	if ( numservers > 1 )
		if ( precision && (numservers > 2) )
			nap = (1000000 - 2*precision) / (numservers - 1);
		else
			nap = 1000000 / (numservers + 1);
	else {
		precision = 0;
		nap = 500000;
	}

#ifdef ENABLE_HTTPS
	SSL_load_error_strings ();
	SSL_library_init ();
#endif

	/* Infinite poll cycle loop in daemonize or foreground mode */
	do {

		/* Initialize number of received valid timestamps, good timestamps
		   and the average of the good timestamps
		*/
		validtimes = goodtimes = sumtimes = offsetdetect = 0;
		if ( precision )
			when = precision;
		else
			when = nap;

		/* Loop through the time sources (web servers); poll cycle */
		for ( i = optind; i < argc; i++ ) {

			/* host:port is stored in argv[i] */
			hostport = (char *)argv[i];
			host = strdup(hostport);
			port = DEFAULT_HTTP_PORT;
			splithostport( &host, &port );

#ifndef ENABLE_HTTPS
			if (strncmp (port, "443", 3) == 0)
				printlog( 1, "HTTPS support not compiled in, "
				          "cannot get timestamp from %s", host);
#endif

			/* if burst mode, reset "when" */
			if ( burstmode ) {
				if ( precision )
					when = precision;
				else
					when = nap;
			}

			burst = 0;
			do {
				/* Retry if first poll shows time offset */
				try = MAX_ATTEMPT;
				do {
					if ( debug ) printlog( 0, "burst: %d try: %d when: %d", \
						                       burst + 1, MAX_ATTEMPT - try + 1, when );
					timestamp = getHTTPdate( host, port, proxy, proxyport,\
					                         httpversion, ipversion, when );
					try--;
				} while ( timestamp && try );

				/* Only include valid responses in timedelta[] */
				if ( timelimit == NO_TIME_LIMIT || ( timestamp < timelimit && timestamp > -timelimit ) ) {
					timedelta[validtimes] = timestamp;
					validtimes++;
				}

				/* If we detected a time offset, set the flag */
				if ( timestamp )
					offsetdetect = 1;

				/* Take a nap, to spread polls equally within a second.
				   Example:
				   2 servers => 0.333, 0.666
				   3 servers => 0.250, 0.500, 0.750
				   4 servers => 0.200, 0.400, 0.600, 0.800
				   ...
				   nap = 1000000 / (#servers + 1)

				   or when "precision" is specified, a different algorithm is used
				*/
				when += nap;

				burst++;
			} while ( burst < (argc - optind) * burstmode );

			/* Sleep for a while, unless we detected a time offset */
			if ( (daemonize || foreground) && !offsetdetect )
				sleep( sleeptime / numservers );

		}

		/* Sort the timedelta results */
		insertsort( timedelta, validtimes );

		/* Mean time value */
		mean = timedelta[validtimes/2];

		/* Filter out the bogus timevalues. A timedelta which is more than
		   1 seconde off from mean, is considered a 'false ticker'.
		   NTP synced web servers can never be more off than a second.
		*/
		for ( i = 0; i < validtimes; i++ ) {
			if ( timedelta[i]-mean <= 1 && timedelta[i]-mean >= -1 ) {
				sumtimes += timedelta[i];
				goodtimes++;
			}
		}

		/* Check if we have at least one valid response */
		if ( goodtimes ) {

			timeavg = sumtimes/(double)goodtimes;

			if ( debug ) {
				printlog( 0, "#: %d mean: %d average: %.3f", goodtimes, \
				          mean, timeavg );
			}

			/* Do I really need to change the time?  */
			if ( sumtimes || !(daemonize || foreground) ) {
				/* If a precision was specified and the time offset is small
				   (< +-1 second), adjust the time with the value of precision
				*/
				if ( precision && sumtimes < goodtimes && sumtimes > -goodtimes )
					timeavg = (double)precision / 1000000 * sign(sumtimes);

				/* Correct the clock, if not in "adjtimex" mode */
				if ( setclock( timeavg, setmode ) < 0 )
					printlog( 1, "Time change failed" );

				/* Drop root privileges again */
				swuid( sw_uid );

				if ( daemonize || foreground ) {
					if ( starttime ) {
						/* Calculate systematic clock drift */
						drift = timeavg / ( time(NULL) - starttime );
						printlog( 0, "Drift %.2f PPM, %.2f s/day", \
						          drift*1e6, drift*86400 );

						/* Adjust system clock */
						if ( setmode == 3 ) {
							starttime = time(NULL);
							/* Adjust the kernel clock */
							if ( htpdate_adjtimex( drift ) < 0 )
								printlog( 1, "Frequency change failed" );

							/* Drop root privileges again */
							swuid( sw_uid );
						}
					} else {
						starttime = time(NULL);
					}

					/* Decrease polling interval to minimum */
					sleeptime = minsleep;

					/* Sleep for 30 minutes after a time adjust or set */
					sleep( DEFAULT_MIN_SLEEP );
				}
			} else {
				/* Increase polling interval */
				if ( sleeptime < maxsleep )
					sleeptime <<= 1;
			}
			if ( debug && (daemonize || foreground) )
				printlog( 0, "poll %ld s", sleeptime );

		} else {
			printlog( 1, "No server suitable for synchronization found" );
			/* Sleep for minsleep to avoid flooding */
			if ( daemonize || foreground )
				sleep( minsleep );
			else
				exit(1);
		}

		/* After first poll cycle do not step through time, only adjust */
		if ( setmode != 3 ) {
			setmode = 1;
		}

	} while ( daemonize || foreground );		/* end of infinite while loop */

	exit(0);
}

/* vim: set ts=4 sw=4: */
