/*
 * fping: fast-ping, file-ping, favorite-ping, funky-ping
 *
 *   Ping a list of target hosts in a round robin fashion.
 *   A better ping overall.
 *
 * fping website:  http://www.fping.org
 *
 * Current maintainer of fping: David Schweikert
 * Please send suggestions and patches to: david@schweikert.ch
 *
 *
 * Original author:  Roland Schemers  <schemers@stanford.edu>
 * IPv6 Support:     Jeroen Massar    <jeroen@unfix.org / jeroen@ipng.nl>
 * Improved main loop: David Schweikert <david@schweikert.ch>
 * Debian Merge, TOS settings: Tobi Oetiker <tobi@oetiker.ch>
 * Bugfixes, byte order & senseful seq.-numbers: Stephan Fuhrmann (stephan.fuhrmann AT 1und1.de)
 *
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by Stanford University.  The name of the University may not be used
 * to endorse or promote products derived from this software without
 * specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "config.h"
#include "fping.h"
#include "options.h"
#include "optparse.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "seqmap.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#include <stddef.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#if HAVE_SYS_FILE_H
#include <sys/file.h>
#endif /* HAVE_SYS_FILE_H */

#ifdef IPV6
#include <netinet/icmp6.h>
#endif
#include <netinet/in_systm.h>

#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>

#include <sys/select.h>

/*** compatibility ***/

/* Mac OS X's getaddrinfo() does not fail if we use an invalid combination,
 * e.g. AF_INET6 with "127.0.0.1". If we pass AI_UNUSABLE to flags, it behaves
 * like other platforms. But AI_UNUSABLE isn't available on other platforms,
 * and we can safely use 0 for flags instead.
 */
#ifndef AI_UNUSABLE
#define AI_UNUSABLE 0
#endif

/* MSG_TRUNC available on Linux kernel 2.2+, makes recvmsg return the full
 * length of the raw packet received, even if the buffer is smaller */
#ifndef MSG_TRUNC
#define MSG_TRUNC 0
#define RECV_BUFSIZE 4096
#else
#define RECV_BUFSIZE 128
#endif

/*** externals ***/

extern char *optarg;
extern int optind, opterr;
#ifndef h_errno
extern int h_errno;
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

/*** Constants ***/

/* CLOCK_MONTONIC starts under macOS, OpenBSD and FreeBSD with undefined positive point and can not be use
 * see github PR #217
 * The configure script detect the predefined operating systems an set CLOCK_REALTIME using over ONLY_CLOCK_REALTIME variable
 */
#if HAVE_SO_TIMESTAMPNS || ONLY_CLOCK_REALTIME
#define CLOCKID CLOCK_REALTIME
#endif

#if !defined(CLOCKID)
#if defined(CLOCK_MONOTONIC)
#define CLOCKID CLOCK_MONOTONIC
#else
#define CLOCKID CLOCK_REALTIME
#endif
#endif

/*** Ping packet defines ***/

#define MAX_IP_PACKET 65535 /* (theoretical) max IPv4 packet size */
#define SIZE_IP_HDR 20 /* min IPv4 header size */
#define SIZE_ICMP_HDR 8 /* from ip_icmp.h */
#define MAX_PING_DATA (MAX_IP_PACKET - SIZE_IP_HDR - SIZE_ICMP_HDR)

#define MAX_GENERATE 131072 /* maximum number of hosts that -g can generate */
#define MAX_TARGET_NAME_LEN 255 /* maximum target name length read from file */

/* sized so as to be like traditional ping */
#define DEFAULT_PING_DATA_SIZE 56

/* ICMP Timestamp has a fixed payload size of 12 bytes */
#define ICMP_TIMESTAMP_DATA_SIZE 12

/* maxima and minima */
#ifdef FPING_SAFE_LIMITS
#define MIN_INTERVAL 1 /* in millisec */
#define MIN_PERHOST_INTERVAL 10 /* in millisec */
#else
#define MIN_INTERVAL 0
#define MIN_PERHOST_INTERVAL 0
#endif

/* response time array flags */
#define RESP_WAITING -1
#define RESP_UNUSED -2
#define RESP_ERROR -3
#define RESP_TIMEOUT -4

/* debugging flags */
#if defined(DEBUG) || defined(_DEBUG)
#define DBG_TRACE 1
#define DBG_SENT_TIMES 2
#define DBG_RANDOM_LOSE_FEW 4
#define DBG_RANDOM_LOSE_MANY 8
#define DBG_PRINT_PER_SYSTEM 16
#define DBG_REPORT_ALL_RTTS 32
#endif /* DEBUG || _DEBUG */

/* Long names for ICMP packet types */
#define ICMP_TYPE_STR_MAX 18
char *icmp_type_str[19] = {
    "ICMP Echo Reply", /* 0 */
    "",
    "",
    "ICMP Unreachable", /* 3 */
    "ICMP Source Quench", /* 4 */
    "ICMP Redirect", /* 5 */
    "",
    "",
    "ICMP Echo", /* 8 */
    "",
    "",
    "ICMP Time Exceeded", /* 11 */
    "ICMP Parameter Problem", /* 12 */
    "ICMP Timestamp Request", /* 13 */
    "ICMP Timestamp Reply", /* 14 */
    "ICMP Information Request", /* 15 */
    "ICMP Information Reply", /* 16 */
    "ICMP Mask Request", /* 17 */
    "ICMP Mask Reply" /* 18 */
};

char *icmp_unreach_str[16] = {
    "ICMP Network Unreachable", /* 0 */
    "ICMP Host Unreachable", /* 1 */
    "ICMP Protocol Unreachable", /* 2 */
    "ICMP Port Unreachable", /* 3 */
    "ICMP Unreachable (Fragmentation Needed)", /* 4 */
    "ICMP Unreachable (Source Route Failed)", /* 5 */
    "ICMP Unreachable (Destination Network Unknown)", /* 6 */
    "ICMP Unreachable (Destination Host Unknown)", /* 7 */
    "ICMP Unreachable (Source Host Isolated)", /* 8 */
    "ICMP Unreachable (Communication with Network Prohibited)", /* 9 */
    "ICMP Unreachable (Communication with Host Prohibited)", /* 10 */
    "ICMP Unreachable (Network Unreachable For Type Of Service)", /* 11 */
    "ICMP Unreachable (Host Unreachable For Type Of Service)", /* 12 */
    "ICMP Unreachable (Communication Administratively Prohibited)", /* 13 */
    "ICMP Unreachable (Host Precedence Violation)", /* 14 */
    "ICMP Unreachable (Precedence cutoff in effect)" /* 15 */
};

#define ICMP_UNREACH_MAXTYPE 15

struct event;
typedef struct host_entry {
    int i; /* index into array */
    char *name; /* name as given by user */
    char *host; /* text description of host */
    struct sockaddr_storage saddr; /* internet address */
    socklen_t saddr_len;
    int64_t timeout; /* time to wait for response */
    int64_t last_send_time; /* time of last packet sent */
    int num_sent; /* number of ping packets sent (for statistics) */
    int num_recv; /* number of pings received (duplicates ignored) */
    int num_recv_total; /* number of pings received, including duplicates */
    int64_t max_reply; /* longest response time */
    int64_t min_reply; /* shortest response time */
    int64_t total_time; /* sum of response times */
    /* _i -> splits (reset on every report interval) */
    int num_sent_i; /* number of ping packets sent */
    int num_recv_i; /* number of pings received */
    int64_t max_reply_i; /* longest response time */
    int64_t min_reply_i; /* shortest response time */
    int64_t total_time_i; /* sum of response times */
    int64_t *resp_times; /* individual response times */

    /* to avoid allocating two struct events each time that we send a ping, we
     * preallocate here two struct events for each ping that we might send for
     * this host. */
    struct event *event_storage_ping;
    struct event *event_storage_timeout;
} HOST_ENTRY;

int event_storage_count; /* how many events can be stored in host_entry->event_storage_xxx */

/* basic algorithm to ensure that we have correct data at all times:
 *
 * 1. when a ping is sent:
 *    - two events get added into event_queue:
 *      - t+PERIOD: ping event
 *      - t+TIMEOUT: timeout event
 *
 * 2. when a ping is received:
 *    - record statistics (increase num_sent and num_received)
 *    - remove timeout event (we store the event in seqmap, so that we can retrieve it when the response is received)
 *
 * 3. when a timeout happens:
 *    - record statistics (increase num_sent only)
 */

#define EV_TYPE_PING 1
#define EV_TYPE_TIMEOUT 2

struct event {
    struct event *ev_prev;
    struct event *ev_next;
    int64_t ev_time;
    struct host_entry *host;
    int ping_index;
};

struct event_queue {
    struct event *first;
    struct event *last;
};

/*** globals ***/

HOST_ENTRY **table = NULL; /* array of pointers to items in the list */

/* we keep two separate queues: a ping queue, for when the next ping should be
 * sent, and a timeout queue. the reason for having two separate queues is that
 * the ping period and the timeout value are different, so if we put them in
 * the same event queue, we would need to scan many more entries when inserting
 * into the sorted list.
 */
struct event_queue event_queue_ping;
struct event_queue event_queue_timeout;

char *prog;
int ident4 = 0; /* our icmp identity field */
int ident6 = 0;
int socket4 = -1;
int socktype4 = -1;
int using_sock_dgram4 = 0;
#ifndef IPV6
int hints_ai_family = AF_INET;
#else
int socket6 = -1;
int socktype6 = -1;
int hints_ai_family = AF_UNSPEC;
#endif

volatile sig_atomic_t status_snapshot = 0;
volatile sig_atomic_t finish_requested = 0;

unsigned int debugging = 0;

/* all time-related values are int64_t nanoseconds */
unsigned int retry = DEFAULT_RETRY;
int64_t timeout = (int64_t)DEFAULT_TIMEOUT * 1000000;
int64_t interval = (int64_t)DEFAULT_INTERVAL * 1000000;
int64_t perhost_interval = (int64_t)DEFAULT_PERHOST_INTERVAL * 1000000;
float backoff = DEFAULT_BACKOFF_FACTOR;
unsigned int ping_data_size = DEFAULT_PING_DATA_SIZE;
unsigned int count = 1, min_reachable = 0;
unsigned int trials;
int64_t report_interval = 0;
unsigned int ttl = 0;
int src_addr_set = 0;
struct in_addr src_addr;
#ifdef IPV6
int src_addr6_set = 0;
struct in6_addr src_addr6;
#endif

/* global stats */
int64_t max_reply = 0;
int64_t min_reply = 0;
int64_t total_replies = 0;
int64_t sum_replies = 0;
int max_hostname_len = 0;
int num_hosts = 0; /* total number of hosts */
int num_alive = 0, /* total number alive */
    num_unreachable = 0, /* total number unreachable */
    num_noaddress = 0; /* total number of addresses not found */
int num_timeout = 0, /* number of times select timed out */
    num_pingsent = 0, /* total pings sent */
    num_pingreceived = 0, /* total pings received */
    num_othericmprcvd = 0; /* total non-echo-reply ICMP received */

struct timespec current_time; /* current time (pseudo) */
int64_t current_time_ns;
int64_t start_time;
int64_t end_time;
int64_t last_send_time; /* time last ping was sent */
int64_t next_report_time; /* time next -Q report is expected */

/* switches */
int generate_flag = 0; /* flag for IP list generation */
int verbose_flag, quiet_flag, stats_flag, unreachable_flag, alive_flag;
int elapsed_flag, version_flag, count_flag, loop_flag, netdata_flag;
int per_recv_flag, report_all_rtts_flag, name_flag, addr_flag, backoff_flag, rdns_flag;
int multif_flag, timeout_flag, fast_reachable;
int outage_flag = 0;
int timestamp_flag = 0;
int timestamp_format_flag = 0;
int random_data_flag = 0;
int cumulative_stats_flag = 0;
int check_source_flag = 0;
int icmp_request_typ = 0;
int print_tos_flag = 0;
int print_ttl_flag = 0;
int size_flag = 0;
#if defined(DEBUG) || defined(_DEBUG)
int randomly_lose_flag, trace_flag, print_per_system_flag;
int lose_factor;
#endif /* DEBUG || _DEBUG */

unsigned int fwmark = 0;

char *filename = NULL; /* file containing hosts to ping */

/*** forward declarations ***/

void add_name(char *name);
void add_addr(char *name, char *host, struct sockaddr *ipaddr, socklen_t ipaddr_len);
char *na_cat(char *name, struct in_addr ipaddr);
void crash_and_burn(char *message);
void errno_crash_and_burn(char *message);
char *get_host_by_address(struct in_addr in);
int send_ping(HOST_ENTRY *h, int index);
void usage(int);
int wait_for_reply(int64_t);
void print_per_system_stats(void);
void print_per_system_splits(void);
void stats_reset_interval(HOST_ENTRY *h);
void print_netdata(void);
void print_global_stats(void);
void main_loop();
void signal_handler(int);
void finish();
const char *sprint_tm(int64_t t);
void ev_enqueue(struct event_queue *queue, struct event *event);
struct event *ev_dequeue(struct event_queue *queue);
void ev_remove(struct event_queue *queue, struct event *event);
void add_cidr(char *);
void add_cidr_ipv4(unsigned long, unsigned long);
void add_range(char *, char *);
void add_addr_range_ipv4(unsigned long, unsigned long);
#ifdef IPV6
uint64_t be_octets_to_uint64(uint8_t*);
void uint64_to_be_octets(uint64_t, uint8_t*);
void add_cidr_ipv6(uint64_t, uint64_t, unsigned long, const char *);
void add_addr_range_ipv6(uint64_t, uint64_t, uint64_t, uint64_t, const char *);
#endif
void print_warning(char *fmt, ...);
int addr_cmp(struct sockaddr *a, struct sockaddr *b);
void host_add_ping_event(HOST_ENTRY *h, int index, int64_t ev_time);
void host_add_timeout_event(HOST_ENTRY *h, int index, int64_t ev_time);
struct event *host_get_timeout_event(HOST_ENTRY *h, int index);
void stats_add(HOST_ENTRY *h, int index, int success, int64_t latency);
void update_current_time();
void print_timestamp_format(int64_t current_time_ns, int timestamp_format);
static uint32_t ms_since_midnight_utc(int64_t time_val);

/************************************************************

  Function: p_setsockopt

*************************************************************

  Inputs:  p_uid: privileged uid. Others as per setsockopt(2)

  Description:

  Elevates privileges to p_uid when required, calls
  setsockopt, and drops privileges back.

************************************************************/

int p_setsockopt(uid_t p_uid, int sockfd, int level, int optname,
    const void *optval, socklen_t optlen)
{
    const uid_t saved_uid = geteuid();
    int res;

    if (p_uid != saved_uid && seteuid(p_uid)) {
        perror("cannot elevate privileges for setsockopt");
    }

    res = setsockopt(sockfd, level, optname, optval, optlen);

    if (p_uid != saved_uid && seteuid(saved_uid)) {
        perror("fatal error: could not drop privileges after setsockopt");
        /* continuing would be a security hole */
        exit(4);
    }

    return res;
}

/************************************************************

  Function: main

*************************************************************

  Inputs:  int argc, char** argv

  Description:

  Main program entry point

************************************************************/

int main(int argc, char **argv)
{
/* Debug: CPU Performance */
#if defined(DEBUG) || defined(_DEBUG)
    clock_t perf_cpu_start, perf_cpu_end;
    double perf_cpu_time_used;
    perf_cpu_start = clock();
#endif /* DEBUG || _DEBUG */

    int c;
    const uid_t suid = geteuid();
    int tos = 0;
    struct optparse optparse_state;
#ifdef USE_SIGACTION
    struct sigaction act;
#endif

    /* pre-parse -h/--help, so that we also can output help information
     * without trying to open the socket, which might fail */
    prog = argv[0];
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(0);
    }

    socket4 = open_ping_socket_ipv4(&socktype4);
#ifdef __linux__
    /* We only treat SOCK_DGRAM differently on Linux, where the IPv4 header
     * structure is missing in the message.
     */
    using_sock_dgram4 = (socktype4 == SOCK_DGRAM);
#endif

#ifdef IPV6
    socket6 = open_ping_socket_ipv6(&socktype6);
    /* if called (sym-linked) via 'fping6', imply '-6'
     * for backward compatibility */
    if (strstr(prog, "fping6")) {
        hints_ai_family = AF_INET6;
    }
#endif

    memset(&src_addr, 0, sizeof(src_addr));
#ifdef IPV6
    memset(&src_addr6, 0, sizeof(src_addr6));
#endif

    if (!suid && suid != getuid()) {
        /* *temporarily* drop privileges */
        if (seteuid(getuid()) == -1)
            perror("cannot setuid");
    }

    optparse_init(&optparse_state, argv);
    ident4 = ident6 = htons(getpid() & 0xFFFF);
    verbose_flag = 1;
    backoff_flag = 1;
    opterr = 1;

    /* get command line options */

    struct optparse_long longopts[] = {
        { "ipv4", '4', OPTPARSE_NONE },
        { "ipv6", '6', OPTPARSE_NONE },
        { "alive", 'a', OPTPARSE_NONE },
        { "addr", 'A', OPTPARSE_NONE },
        { "size", 'b', OPTPARSE_REQUIRED },
        { "backoff", 'B', OPTPARSE_REQUIRED },
        { "count", 'c', OPTPARSE_REQUIRED },
        { "vcount", 'C', OPTPARSE_REQUIRED },
        { "rdns", 'd', OPTPARSE_NONE },
        { "timestamp", 'D', OPTPARSE_NONE },
        { "timestamp-format", '0', OPTPARSE_REQUIRED },
        { "elapsed", 'e', OPTPARSE_NONE },
        { "file", 'f', OPTPARSE_REQUIRED },
        { "generate", 'g', OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { "ttl", 'H', OPTPARSE_REQUIRED },
        { "interval", 'i', OPTPARSE_REQUIRED },
        { "iface", 'I', OPTPARSE_REQUIRED },
        { "icmp-timestamp", '0', OPTPARSE_NONE },
#ifdef SO_MARK
        { "fwmark", 'k', OPTPARSE_REQUIRED },
#endif
        { "loop", 'l', OPTPARSE_NONE },
        { "all", 'm', OPTPARSE_NONE },
        { "dontfrag", 'M', OPTPARSE_NONE },
        { "name", 'n', OPTPARSE_NONE },
        { "netdata", 'N', OPTPARSE_NONE },
        { "outage", 'o', OPTPARSE_NONE },
        { "tos", 'O', OPTPARSE_REQUIRED },
        { "period", 'p', OPTPARSE_REQUIRED },
        { "quiet", 'q', OPTPARSE_NONE },
        { "squiet", 'Q', OPTPARSE_REQUIRED },
        { "retry", 'r', OPTPARSE_REQUIRED },
        { "random", 'R', OPTPARSE_NONE },
        { "stats", 's', OPTPARSE_NONE },
        { "src", 'S', OPTPARSE_REQUIRED },
        { "timeout", 't', OPTPARSE_REQUIRED },
        { NULL, 'T', OPTPARSE_REQUIRED },
        { "unreach", 'u', OPTPARSE_NONE },
        { "version", 'v', OPTPARSE_NONE },
        { "reachable", 'x', OPTPARSE_REQUIRED },
        { "fast-reachable", 'X', OPTPARSE_REQUIRED },
        { "check-source", '0', OPTPARSE_NONE },
        { "print-tos", '0', OPTPARSE_NONE },
        { "print-ttl", '0', OPTPARSE_NONE },
#if defined(DEBUG) || defined(_DEBUG)
        { NULL, 'z', OPTPARSE_REQUIRED },
#endif
        { 0, 0, 0 }
    };

    float opt_value_float;
    while ((c = optparse_long(&optparse_state, longopts, NULL)) != EOF) {
        switch (c) {
        case '0':
            if(strstr(optparse_state.optlongname, "timestamp-format") != NULL) {
                if(strcmp(optparse_state.optarg, "ctime") == 0) {
                  timestamp_format_flag = 1;
                }else if(strcmp(optparse_state.optarg, "iso") == 0) {
                  timestamp_format_flag = 2;
                }else if(strcmp(optparse_state.optarg, "rfc3339") == 0) {
                  timestamp_format_flag = 3;
                }else{
                  usage(1);
                }
            } else if (strstr(optparse_state.optlongname, "check-source") != NULL) {
                check_source_flag = 1;
            } else if (strstr(optparse_state.optlongname, "icmp-timestamp") != NULL) {
#ifdef IPV6
                if (hints_ai_family != AF_UNSPEC && hints_ai_family != AF_INET) {
                    fprintf(stderr, "%s: ICMP Timestamp is IPv4 only\n", prog);
                    exit(1);
                }
                hints_ai_family = AF_INET;
#endif
                icmp_request_typ = 13;
                ping_data_size = ICMP_TIMESTAMP_DATA_SIZE;
            } else if (strstr(optparse_state.optlongname, "print-tos") != NULL) {
                print_tos_flag = 1;
            } else if (strstr(optparse_state.optlongname, "print-ttl") != NULL) {
                print_ttl_flag = 1;
            } else {
                usage(1);
            }
            break;
        case '4':
#ifdef IPV6
            if (hints_ai_family != AF_UNSPEC && hints_ai_family != AF_INET) {
                fprintf(stderr, "%s: can't specify both -4 and -6\n", prog);
                exit(1);
            }
            hints_ai_family = AF_INET;
#endif
            break;
        case '6':
#ifdef IPV6
            if (hints_ai_family != AF_UNSPEC && hints_ai_family != AF_INET6) {
                fprintf(stderr, "%s: can't specify both -4 and -6\n", prog);
                exit(1);
            }
            hints_ai_family = AF_INET6;
#else
            fprintf(stderr, "%s: IPv6 not supported by this binary\n", prog);
            exit(1);
#endif
            break;
        case 'M':
#ifdef IP_MTU_DISCOVER
            if (socket4 >= 0) {
                int val = IP_PMTUDISC_DO;
                if (setsockopt(socket4, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val))) {
                    perror("setsockopt IP_MTU_DISCOVER");
                }
            }
#ifdef IPV6
            if (socket6 >= 0) {
                int val = IPV6_PMTUDISC_DO;
                if (setsockopt(socket6, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val))) {
                    perror("setsockopt IPV6_MTU_DISCOVER");
                }
            }
#endif
#else
            fprintf(stderr, "%s, -M option not supported on this platform\n", prog);
            exit(1);
#endif
            break;

        case 't':
            if (sscanf(optparse_state.optarg, "%f", &opt_value_float) != 1)
                usage(1);
            if (opt_value_float < 0) {
                usage(1);
            }
            timeout = opt_value_float * 1000000;
            timeout_flag = 1;
            break;

        case 'r':
            if (sscanf(optparse_state.optarg, "%u", &retry) != 1)
                usage(1);
            break;

        case 'i':
            if (sscanf(optparse_state.optarg, "%f", &opt_value_float) != 1)
                usage(1);
            if (opt_value_float < 0) {
                usage(1);
            }
            interval = opt_value_float * 1000000;
            break;

        case 'p':
            if (sscanf(optparse_state.optarg, "%f", &opt_value_float) != 1)
                usage(1);
            if (opt_value_float < 0) {
                usage(1);
            }
            perhost_interval = opt_value_float * 1000000;

            break;

        case 'c':
            if (!(count = (unsigned int)atoi(optparse_state.optarg)))
                usage(1);

            count_flag = 1;
            break;

        case 'C':
            if (!(count = (unsigned int)atoi(optparse_state.optarg)))
                usage(1);

            count_flag = 1;
            report_all_rtts_flag = 1;
            break;

        case 'b':
            if (sscanf(optparse_state.optarg, "%u", &ping_data_size) != 1)
                usage(1);
            size_flag = 1;
            break;

        case 'h':
            usage(0);
            break;

        case 'q':
            verbose_flag = 0;
            quiet_flag = 1;
            break;

        case 'Q':
            verbose_flag = 0;
            quiet_flag = 1;
            if (sscanf(optparse_state.optarg, "%f", &opt_value_float) != 1)
                usage(1);
            if (opt_value_float < 0) {
                usage(1);
            }
            report_interval = opt_value_float * 1e9;

            /* recognize keyword(s) after number, ignore everything else */
            {
                char *comma = strchr(optparse_state.optarg, ',');
                if ((comma != NULL) && (strcmp(++comma, "cumulative") == 0)) {
                    cumulative_stats_flag = 1;
                }
            }

            break;

        case 'e':
            elapsed_flag = 1;
            break;

        case 'm':
            multif_flag = 1;
            break;

        case 'N':
            netdata_flag = 1;
            break;

        case 'n':
            name_flag = 1;
            if (rdns_flag) {
                fprintf(stderr, "%s: use either one of -d or -n\n", prog);
                exit(1);
            }
            break;

        case 'd':
            rdns_flag = 1;
            if (name_flag) {
                fprintf(stderr, "%s: use either one of -d or -n\n", prog);
                exit(1);
            }
            break;

        case 'A':
            addr_flag = 1;
            break;

        case 'B':
            if (!(backoff = atof(optparse_state.optarg)))
                usage(1);

            break;

        case 's':
            stats_flag = 1;
            break;

        case 'D':
            timestamp_flag = 1;
            break;

        case 'R':
            random_data_flag = 1;
            break;

        case 'l':
            loop_flag = 1;
            backoff_flag = 0;
            break;

        case 'u':
            unreachable_flag = 1;
            break;

        case 'a':
            alive_flag = 1;
            break;

        case 'H':
            if (!(ttl = (unsigned int)atoi(optparse_state.optarg)))
                usage(1);
            break;

#if defined(DEBUG) || defined(_DEBUG)
        case 'z':
            if (sscanf(optparse_state.optarg, "0x%x", &debugging) != 1)
                if (sscanf(optparse_state.optarg, "%u", &debugging) != 1)
                    usage(1);

            break;
#endif /* DEBUG || _DEBUG */

        case 'v':
            printf("%s: Version %s\n", prog, VERSION);
            exit(0);

        case 'x':
            if (!(min_reachable = (unsigned int)atoi(optparse_state.optarg)))
                usage(1);
            break;

        case 'X':
            if (!(min_reachable = (unsigned int)atoi(optparse_state.optarg)))
                usage(1);
            fast_reachable = 1;
            break;

        case 'f':
            filename = optparse_state.optarg;
            break;
#ifdef SO_MARK
        case 'k':
            if (!(fwmark = (unsigned int)atol(optparse_state.optarg)))
                usage(1);

            if (socket4 >= 0)
                if(-1 == p_setsockopt(suid, socket4, SOL_SOCKET, SO_MARK, &fwmark, sizeof fwmark))
                    perror("fwmark ipv4");

#ifdef IPV6
            if (socket6 >= 0)
                if(-1 == p_setsockopt(suid, socket6, SOL_SOCKET, SO_MARK, &fwmark, sizeof fwmark))
                    perror("fwmark ipv6");
#endif

            break;
#endif

        case 'g':
            /* use IP list generation */
            /* mutually exclusive with using file input or command line targets */
            generate_flag = 1;
            break;

        case 'S':
            if (inet_pton(AF_INET, optparse_state.optarg, &src_addr)) {
                src_addr_set = 1;
                break;
            }
#ifdef IPV6
            if (inet_pton(AF_INET6, optparse_state.optarg, &src_addr6)) {
                src_addr6_set = 1;
                break;
            }
#endif
            fprintf(stderr, "%s: can't parse source address: %s\n", prog, optparse_state.optarg);
            exit(1);

        case 'I':
#ifdef SO_BINDTODEVICE
            if (socket4 >= 0) {
                if (p_setsockopt(suid, socket4, SOL_SOCKET, SO_BINDTODEVICE, optparse_state.optarg, strlen(optparse_state.optarg))) {
                    perror("binding to specific interface (SO_BINDTODEVICE)");
                    exit(1);
                }
            }
#ifdef IPV6
            if (socket6 >= 0) {
                if (p_setsockopt(suid, socket6, SOL_SOCKET, SO_BINDTODEVICE, optparse_state.optarg, strlen(optparse_state.optarg))) {
                    perror("binding to specific interface (SO_BINDTODEVICE), IPV6");
                    exit(1);
                }
            }
#endif
#else
            printf("%s: cant bind to a particular net interface since SO_BINDTODEVICE is not supported on your os.\n", prog);
            exit(3);
            ;
#endif
            break;

        case 'T':
            /* This option is ignored for compatibility reasons ("select timeout" is not meaningful anymore) */
            break;

        case 'O':
            if (sscanf(optparse_state.optarg, "%i", &tos) == 1) {
                if (socket4 >= 0) {
                    if (setsockopt(socket4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos))) {
                        perror("setting type of service octet IP_TOS");
                    }
                }
#if defined(IPV6) && defined(IPV6_TCLASS)
                if (socket6 >= 0) {
                    if (setsockopt(socket6, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos))) {
                        perror("setting type of service octet IPV6_TCLASS");
                    }
                }
#endif
            }
            else {
                usage(1);
            }
            break;

        case 'o':
            outage_flag = 1;
            break;

        case '?':
            fprintf(stderr, "%s: %s\n", argv[0], optparse_state.errmsg);
            fprintf(stderr, "see 'fping -h' for usage information\n");
            exit(1);
            break;
        }
    }

    /* permanently drop privileges */
    if (suid != getuid() && setuid(getuid())) {
        perror("fatal: failed to permanently drop privileges");
        /* continuing would be a security hole */
        exit(4);
    }

    /* validate various option settings */

#ifndef IPV6
    if (socket4 < 0) {
        crash_and_burn("can't create socket (must run as root?)");
    }
#else
    if ((socket4 < 0 && socket6 < 0) || (hints_ai_family == AF_INET6 && socket6 < 0)) {
        crash_and_burn("can't create socket (must run as root?)");
    }
#endif

    if (ttl > 255) {
        fprintf(stderr, "%s: ttl %u out of range\n", prog, ttl);
        exit(1);
    }

    if (unreachable_flag && alive_flag) {
        fprintf(stderr, "%s: specify only one of a, u\n", prog);
        exit(1);
    }

    if (count_flag && loop_flag) {
        fprintf(stderr, "%s: specify only one of c, l\n", prog);
        exit(1);
    }

#ifdef FPING_SAFE_LIMITS
    if ((interval < (int64_t)MIN_INTERVAL * 1000000 || perhost_interval < (int64_t)MIN_PERHOST_INTERVAL * 1000000)
        && getuid()) {
        fprintf(stderr, "%s: these options are too risky for mere mortals.\n", prog);
        fprintf(stderr, "%s: You need -i >= %u and -p >= %u\n",
            prog, MIN_INTERVAL, MIN_PERHOST_INTERVAL);
        exit(1);
    }
#endif

    if (ping_data_size > MAX_PING_DATA) {
        fprintf(stderr, "%s: data size %u not valid, must not be larger than %u\n",
            prog, ping_data_size, (unsigned int)MAX_PING_DATA);
        exit(1);
    }

    if ((backoff > MAX_BACKOFF_FACTOR) || (backoff < MIN_BACKOFF_FACTOR)) {
        fprintf(stderr, "%s: backoff factor %.1f not valid, must be between %.1f and %.1f\n",
            prog, backoff, MIN_BACKOFF_FACTOR, MAX_BACKOFF_FACTOR);
        exit(1);
    }

    if (icmp_request_typ == 13 && size_flag != 0) {
        fprintf(stderr, "%s: cannot change ICMP Timestamp size\n", prog);
        exit(1);
    }

    if (count_flag) {
        if (verbose_flag)
            per_recv_flag = 1;

        alive_flag = unreachable_flag = verbose_flag = 0;
    }

    if (loop_flag) {
        if (!report_interval)
            per_recv_flag = 1;

        alive_flag = unreachable_flag = verbose_flag = 0;
    }

    if (alive_flag || unreachable_flag || min_reachable)
        verbose_flag = 0;

    trials = (count > retry + 1) ? count : retry + 1;

    /* auto-tune default timeout for count/loop modes
     * see also github #32 */
    if (loop_flag || count_flag) {
        if (!timeout_flag) {
            timeout = perhost_interval;
            if (timeout > (int64_t)AUTOTUNE_TIMEOUT_MAX * 1000000) {
                timeout = (int64_t)AUTOTUNE_TIMEOUT_MAX * 1000000;
            }
        }
    }

#if defined(DEBUG) || defined(_DEBUG)
    if (debugging & DBG_TRACE)
        trace_flag = 1;

    if (debugging & DBG_RANDOM_LOSE_FEW) {
        randomly_lose_flag = 1;
        lose_factor = 1; /* ie, 1/4 */
    }

    if (debugging & DBG_RANDOM_LOSE_MANY) {
        randomly_lose_flag = 1;
        lose_factor = 5; /* ie, 3/4 */
    }

    if (debugging & DBG_PRINT_PER_SYSTEM)
        print_per_system_flag = 1;

    if ((debugging & DBG_REPORT_ALL_RTTS) && !loop_flag)
        report_all_rtts_flag = 1;

    if (trace_flag) {
        fprintf(stderr, "%s:\n  count: %u, retry: %u, interval: %.0f ms\n",
            prog, count, retry, interval / 1e6);
        fprintf(stderr, "  perhost_interval: %.0f ms, timeout: %.0f\n",
            perhost_interval / 1e6, timeout / 1e6);
        fprintf(stderr, "  ping_data_size = %u, trials = %u\n",
            ping_data_size, trials);

        if (verbose_flag)
            fprintf(stderr, "  verbose_flag set\n");
        if (multif_flag)
            fprintf(stderr, "  multif_flag set\n");
        if (name_flag)
            fprintf(stderr, "  name_flag set\n");
        if (addr_flag)
            fprintf(stderr, "  addr_flag set\n");
        if (stats_flag)
            fprintf(stderr, "  stats_flag set\n");
        if (unreachable_flag)
            fprintf(stderr, "  unreachable_flag set\n");
        if (alive_flag)
            fprintf(stderr, "  alive_flag set\n");
        if (elapsed_flag)
            fprintf(stderr, "  elapsed_flag set\n");
        if (version_flag)
            fprintf(stderr, "  version_flag set\n");
        if (count_flag)
            fprintf(stderr, "  count_flag set\n");
        if (loop_flag)
            fprintf(stderr, "  loop_flag set\n");
        if (backoff_flag)
            fprintf(stderr, "  backoff_flag set\n");
        if (per_recv_flag)
            fprintf(stderr, "  per_recv_flag set\n");
        if (report_all_rtts_flag)
            fprintf(stderr, "  report_all_rtts_flag set\n");
        if (randomly_lose_flag)
            fprintf(stderr, "  randomly_lose_flag set\n");
        if (print_per_system_flag)
            fprintf(stderr, "  print_per_system_flag set\n");
        if (outage_flag)
            fprintf(stderr, "  outage_flag set\n");
        if (netdata_flag)
            fprintf(stderr, "  netdata_flag set\n");
    }
#endif /* DEBUG || _DEBUG */

    /* set the TTL, if the -H option was set (otherwise ttl will be = 0) */
    if (ttl > 0) {
        if (socket4 >= 0) {
            if (setsockopt(socket4, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl))) {
                perror("setting time to live");
            }
        }
#ifdef IPV6
        if (socket6 >= 0) {
            if (setsockopt(socket6, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl))) {
                perror("setting time to live");
            }
        }
#endif
    }

#if HAVE_SO_TIMESTAMPNS
    {
        int opt = 1;
        if (socket4 >= 0) {
            if (setsockopt(socket4, SOL_SOCKET, SO_TIMESTAMPNS, &opt, sizeof(opt))) {
                if (setsockopt(socket4, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt))) {
                    perror("setting SO_TIMESTAMPNS and SO_TIMESTAMP option");
                }
            }
        }
#ifdef IPV6
        if (socket6 >= 0) {
            if (setsockopt(socket6, SOL_SOCKET, SO_TIMESTAMPNS, &opt, sizeof(opt))) {
                if (setsockopt(socket6, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt))) {
                    perror("setting SO_TIMESTAMPNS and SO_TIMESTAMP option (IPv6)");
                }
            }
        }
#endif
    }
#endif

    update_current_time();
    start_time = current_time_ns;

    /* handle host names supplied on command line or in a file */
    /* if the generate_flag is on, then generate the IP list */

    argv = &argv[optparse_state.optind];
    argc -= optparse_state.optind;

    /* calculate how many ping can be in-flight per host */
    if (count_flag) {
        event_storage_count = count;
    }
    else if (loop_flag) {
        if (perhost_interval > timeout) {
            event_storage_count = 1;
        }
        else {
            event_storage_count = 1 + timeout / perhost_interval;
        }
    }
    else {
        event_storage_count = 1;
    }

    /* file and generate are mutually exclusive */
    /* file and command line are mutually exclusive */
    /* generate requires command line parameters beyond the switches */
    if ((*argv && filename) || (filename && generate_flag) || (generate_flag && !*argv))
        usage(1);

    /* if no conditions are specified, then assume input from stdin */
    if (!*argv && !filename && !generate_flag)
        filename = "-";

    if (*argv && !generate_flag) {
        while (*argv) {
            add_name(*argv);
            ++argv;
        }
    }
    else if (filename) {
        FILE *ping_file;
        char line[MAX_TARGET_NAME_LEN + 1];
        char host[MAX_TARGET_NAME_LEN + 1];
        char scratch[MAX_TARGET_NAME_LEN + 1];
        int skip, non_empty;

        if (strcmp(filename, "-") == 0)
            ping_file = fdopen(0, "r");
        else
            ping_file = fopen(filename, "r");

        if (!ping_file)
            errno_crash_and_burn("fopen");

        /*
         * Read the first word of every non-comment line, skip everything else.
         * (Empty and blank lines are ignored.  Lines where the first non-blank
         * character is a '#' are interpreted as comments and ignored.)
        */
        while (fgets(line, sizeof(line), ping_file)) {
            skip = non_empty = 0;

            /* skip over a prefix of the line where sscanf finds nothing */
            if ((sscanf(line, "%s", host) != 1) || (!*host)) {
                continue;
            }

            /* the first word of the line can indicate a comment line */
            if (host[0] == '#') {
                skip = 1; /* skip remainder of line */
            } else {
                non_empty = 1; /* we have something to add as a target name */
                /*
                 * We have found the start of a word.
                 * This part of the line may contain all of the first word.
                 */
                if (!strchr(line, '\n') && (strlen(line) == sizeof(line) - 1)) {
                    char discard1[MAX_TARGET_NAME_LEN + 1];
                    char discard2[MAX_TARGET_NAME_LEN + 1];
                    if (sscanf(line, "%s%s", discard1, discard2) == 2) {
                        skip = 1; /* a second word starts in this part */
                    }
                    if (isspace(line[sizeof(line) - 2])) {
                        skip = 1; /* the first word ends in this part */
                    }
                }
            }
            /* read remainder of this input line */
            while (!strchr(line, '\n') && fgets(line, sizeof(line), ping_file)) {
                if (skip) {
                    continue; /* skip rest of data in this input line */
                }
                if (isspace(line[0])) {
                    skip = 1; /* first word ended in previous part */
                    continue;
                }
                if ((sscanf(line, "%s", scratch) != 1) || (!*scratch)) {
                    skip = 1; /* empty or blank part of line, skip the rest */
                    continue;
                }
                if (sizeof(host) - strlen(host) < strlen(scratch) + 1) {
                    fprintf(stderr, "%s: target name too long\n", prog);
                    exit(1);
                }
                /* append remainder of word started in previous line part */
                strncat(host, scratch, sizeof(host) - strlen(host) - 1);
                /*
                 * Since the "host" buffer is the same size as the "line"
                 * buffer, a target name that fits into the "host" buffer
                 * cannot use more than two consecutive line parts.
                 * A target name that uses two consecutive line parts
                 * and fits into the "host" buffer must end before the
                 * end of the second "line" buffer.  Thus the rest of
                 * the line can be skipped.
                 */
                skip = 1;
            }

            if (non_empty)
                add_name(host);
        }

        fclose(ping_file);
    }
    else if (*argv && generate_flag) {
        if (argc == 1) {
            /* one target: we expect a cidr range (n.n.n.n/m) */
            add_cidr(argv[0]);
        }
        else if (argc == 2) {
            add_range(argv[0], argv[1]);
        }
        else {
            usage(1);
        }
    }
    else {
        usage(1);
    }

    if (!num_hosts) {
        exit(num_noaddress ? 2 : 1);
    }

    if (socket4 >= 0 && (src_addr_set || socktype4 == SOCK_DGRAM)) {
        socket_set_src_addr_ipv4(socket4, &src_addr, (socktype4 == SOCK_DGRAM) ? &ident4 : NULL);
    }
#ifdef IPV6
    if (socket6 >= 0 && (src_addr6_set || socktype6 == SOCK_DGRAM)) {
        socket_set_src_addr_ipv6(socket6, &src_addr6, (socktype6 == SOCK_DGRAM) ? &ident6 : NULL);
    }
#endif

    /* allocate and initialize array to map host nr to host_entry */
    {
        struct event *cursor = event_queue_ping.first;
        int i = 0;
        table = (HOST_ENTRY **)calloc(num_hosts, sizeof(HOST_ENTRY *));
        if (!table)
            crash_and_burn("Can't malloc array of hosts");
        /* initialize table of hosts. we know that we have ping events scheduled
         * for each of them */
        for (cursor = event_queue_ping.first; cursor; cursor = cursor->ev_next) {
            table[i] = cursor->host;
            cursor->host->i = i;
            i++;
        }
    }

    init_ping_buffer_ipv4(ping_data_size);
#ifdef IPV6
    init_ping_buffer_ipv6(ping_data_size);
#endif

#ifdef USE_SIGACTION
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGINT);
    sigaddset(&act.sa_mask, SIGQUIT);
    act.sa_flags = SA_RESTART;
    if (sigaction(SIGQUIT, &act, NULL) || sigaction(SIGINT, &act, NULL)) {
        crash_and_burn("failure to set signal handler");
    }
#else
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);
#endif
    setlinebuf(stdout);

    if (report_interval) {
        next_report_time = current_time_ns + report_interval;
    }

    last_send_time = 0;

    seqmap_init();

    /* main loop */
    main_loop();

/* Debug: CPU Performance */
#if defined(DEBUG) || defined(_DEBUG)
    perf_cpu_end = clock();
    perf_cpu_time_used = ((double) (perf_cpu_end - perf_cpu_start)) / CLOCKS_PER_SEC;
    printf("[DEBUG] CPU time used: %f sec\n", perf_cpu_time_used);
#endif /* DEBUG || _DEBUG */

    finish();

    return 0;
}

static inline int64_t timespec_ns(struct timespec *a)
{
    return ((int64_t)a->tv_sec * 1000000000) + a->tv_nsec;
}

#if HAVE_SO_TIMESTAMPNS
/* convert a struct timeval to nanoseconds */
static inline int64_t timeval_ns(struct timeval *a)
{
    return ((int64_t)a->tv_sec * 1000000000) + ((int64_t)a->tv_usec * 1000);
}
#endif /* HAVE_SO_TIMESTAMPNS */

void add_cidr(char *addr)
{
    char *addr_end;
    char *mask_str;
    unsigned long mask;
    int ret;
    struct addrinfo addr_hints;
    struct addrinfo *addr_res;
    unsigned long net_addr;
#ifdef IPV6
    uint64_t net_upper, net_lower;
    char *scope_str;
#endif /* IPV6 */

    /* Split address from mask */
    addr_end = strrchr(addr, '/');
    if (addr_end == NULL) {
        usage(1);
    }
    mask_str = addr_end + 1;

#ifdef IPV6
    /* IPv6 addresses can have a scope */
    scope_str = strchr(addr, '%');
    if (scope_str && mask_str < scope_str) {
        fprintf(stderr, "%s: address scope must precede prefix length\n", prog);
        exit(1);
    }
#endif /*IPV6 */

    *addr_end = '\0';
    mask = atoi(mask_str);

    /* parse address */
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_family = hints_ai_family;
    addr_hints.ai_flags = AI_NUMERICHOST;
    ret = getaddrinfo(addr, NULL, &addr_hints, &addr_res);
    if (ret) {
        fprintf(stderr, "%s, can't parse address %s: %s\n", prog, addr, gai_strerror(ret));
        exit(1);
    }
    if (addr_res->ai_family == AF_INET) {
        net_addr = ntohl(((struct sockaddr_in*)addr_res->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(addr_res);
        add_cidr_ipv4(net_addr, mask);
#ifdef IPV6
    } else if (addr_res->ai_family == AF_INET6) {
        uint8_t *ipv6_addr = ((struct sockaddr_in6*)addr_res->ai_addr)->sin6_addr.s6_addr;
        net_upper = be_octets_to_uint64(ipv6_addr);
        net_lower = be_octets_to_uint64(ipv6_addr + 8);
        freeaddrinfo(addr_res);
        add_cidr_ipv6(net_upper, net_lower, mask, scope_str);
#endif /* IPV6 */
    } else {
        freeaddrinfo(addr_res);
        fprintf(stderr, "%s: -g does not support this address family\n", prog);
        exit(1);
    }
}

void add_cidr_ipv4(unsigned long net_addr, unsigned long mask)
{
    unsigned long bitmask;
    unsigned long net_last;

    /* check mask */
    if (mask < 1 || mask > 32) {
        fprintf(stderr, "%s: netmask must be between 1 and 32 (is: %lu)\n", prog, mask);
        exit(1);
    }

    /* convert mask integer from 1 to 32 to a bitmask */
    bitmask = ((unsigned long)0xFFFFFFFF) << (32 - mask);

    /* calculate network range */
    net_addr &= bitmask;
    net_last = net_addr + ((unsigned long)0x1 << (32 - mask)) - 1;

    /* exclude network and broadcast address for regular prefixes */
    if (mask < 31) {
        net_last--;
        net_addr++;
    }

    /* add all hosts in that network (net_addr and net_last inclusive) */
    add_addr_range_ipv4(net_addr, net_last);
}

#ifdef IPV6
void add_cidr_ipv6(uint64_t net_upper, uint64_t net_lower, unsigned long mask, const char *scope_str)
{
    uint64_t bitmask_lower;
    uint64_t last_lower;

    /* check mask -- 2^63 addresses should suffice for now */
    if (mask < 65 || mask > 128) {
        fprintf(stderr, "%s: netmask must be between 65 and 128 (is: %lu)\n", prog, mask);
        exit(1);
    }

    /* convert mask integer from 65 to 128 to the lower part of a bitmask */
    bitmask_lower = ((uint64_t)-1) << (128 - mask);

    /* calculate network range */
    net_lower &= bitmask_lower;
    last_lower = net_lower + ((uint64_t)1 << (128 - mask)) - 1;

    add_addr_range_ipv6(net_upper, net_lower, net_upper, last_lower, scope_str);
}
#endif /* IPV6 */

void add_range(char *start, char *end)
{
    struct addrinfo addr_hints;
    struct addrinfo *addr_res;
    unsigned long start_long;
    unsigned long end_long;
    int ret;
#ifdef IPV6
    uint64_t start_upper, start_lower;
    uint64_t end_upper, end_lower;
    char *start_scope_str, *end_scope_str;

    /*
     * The compiler does not know that setting the address family hint to
     * ensure that start and end are from the same address family also
     * ensures that either start_long and end_long are initialized and used,
     * or start_upper, start_lower, end_upper, and end_lower are initialized
     * and used.  Thus initialize all variables when both IPv4 and IPv6 are
     * supported to suppress compiler warnings.
     */
    start_long = -1;
    end_long = 0;
    start_upper = start_lower = -1;
    end_upper = end_lower = 0;
    start_scope_str = end_scope_str = NULL;
#endif /* IPV6 */

    /* parse start address */
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_family = hints_ai_family;
    addr_hints.ai_flags = AI_NUMERICHOST;
    ret = getaddrinfo(start, NULL, &addr_hints, &addr_res);
    if (ret) {
        fprintf(stderr, "%s: can't parse address %s: %s\n", prog, start, gai_strerror(ret));
        exit(1);
    }
    /* start and end must be from the same address family */
    hints_ai_family = addr_res->ai_family;
    if (addr_res->ai_family == AF_INET) {
        start_long = ntohl(((struct sockaddr_in*)addr_res->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(addr_res);
#ifdef IPV6
    } else if (addr_res->ai_family == AF_INET6) {
        uint8_t *ipv6_addr = ((struct sockaddr_in6*)addr_res->ai_addr)->sin6_addr.s6_addr;
        start_upper = be_octets_to_uint64(ipv6_addr);
        start_lower = be_octets_to_uint64(ipv6_addr + 8);
        freeaddrinfo(addr_res);
#endif /* IPV6 */
    } else {
        freeaddrinfo(addr_res);
        fprintf(stderr, "%s: -g does not support this address family\n", prog);
        exit(1);
    }

    /* IPv6 addresses can have a scope */
    if (hints_ai_family == AF_INET6) {
        start_scope_str = strchr(start, '%');
        end_scope_str = strchr(end, '%');
        if ((!start_scope_str && end_scope_str) ||
            (start_scope_str && !end_scope_str) ||
            (start_scope_str && end_scope_str && strcmp(start_scope_str, end_scope_str) != 0)) {
                fprintf(stderr, "%s: different scopes for start and end addresses\n", prog);
                exit(1);
        }
    }

    /* parse end address */
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_family = hints_ai_family;
    addr_hints.ai_flags = AI_NUMERICHOST;
    ret = getaddrinfo(end, NULL, &addr_hints, &addr_res);
    if (ret) {
        fprintf(stderr, "%s: can't parse address %s: %s\n", prog, end, gai_strerror(ret));
        exit(1);
    }
    if (addr_res->ai_family == AF_INET) {
        end_long = ntohl(((struct sockaddr_in*)addr_res->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(addr_res);
        add_addr_range_ipv4(start_long, end_long);
#ifdef IPV6
    } else if (addr_res->ai_family == AF_INET6) {
        uint8_t *ipv6_addr = ((struct sockaddr_in6*)addr_res->ai_addr)->sin6_addr.s6_addr;
        end_upper = be_octets_to_uint64(ipv6_addr);
        end_lower = be_octets_to_uint64(ipv6_addr + 8);
        freeaddrinfo(addr_res);
        add_addr_range_ipv6(start_upper, start_lower, end_upper, end_lower, start_scope_str);
#endif /* IPV6 */
    } else {
        freeaddrinfo(addr_res);
        fprintf(stderr, "%s: -g does not support this address family\n", prog);
        exit(1);
    }
}

void add_addr_range_ipv4(unsigned long start_long, unsigned long end_long)
{
    /* check if generator limit is exceeded */
    if (end_long >= start_long + MAX_GENERATE) {
        fprintf(stderr, "%s: -g parameter generates too many addresses\n", prog);
        exit(1);
    }

    /* generate */
    for (; start_long <= end_long; start_long++) {
        struct in_addr in_addr_tmp;
        char buffer[20];
        in_addr_tmp.s_addr = htonl(start_long);
        inet_ntop(AF_INET, &in_addr_tmp, buffer, sizeof(buffer));
        add_name(buffer);
    }
}

#ifdef IPV6
uint64_t be_octets_to_uint64(uint8_t *be_octets)
{
    int i;
    uint64_t ret = 0;
    for (i = 0; i < 8; i++) {
        ret |= (uint64_t)be_octets[7 - i] << (i * 8);
    }
    return ret;
}

void uint64_to_be_octets(uint64_t num, uint8_t *be_octets)
{
    int i;
    for (i = 0; i < 8; i++) {
        be_octets[7 - i] = (uint8_t)((num >> (i * 8)) & 0xff);
    }
}

void add_addr_range_ipv6(uint64_t start_upper, uint64_t start_lower,
                         uint64_t end_upper, uint64_t end_lower,
                         const char *scope_str)
{
    struct in6_addr in6_addr_tmp;
    char buffer[100];

    /* prevent generating too many addresses */
    if ((start_upper + 1 < end_upper) ||
        (start_upper + 1 == end_upper && end_lower >= start_lower) ||
        (start_upper + 1 == end_upper && end_lower - MAX_GENERATE >= start_lower) ||
        (start_upper == end_upper && end_lower - MAX_GENERATE >= start_lower &&
                                     start_lower + MAX_GENERATE <= end_lower)) {
        fprintf(stderr, "%s: -g parameter generates too many addresses\n", prog);
        exit(1);
    }

    while ((start_upper < end_upper) ||
           (start_upper == end_upper && start_lower <= end_lower)) {
        uint64_to_be_octets(start_upper, in6_addr_tmp.s6_addr);
        uint64_to_be_octets(start_lower, in6_addr_tmp.s6_addr + 8);
        inet_ntop(AF_INET6, &in6_addr_tmp, buffer, sizeof(buffer));
        if (scope_str) {
	    if (strlen(buffer) + strlen(scope_str) + 1 > sizeof(buffer)) {
                fprintf(stderr, "%s: scope identifier is too long\n", prog);
                exit(1);
            }
            strncat(buffer, scope_str, sizeof(buffer) - strlen(buffer) - 1);
        }
        add_name(buffer);
        start_lower++;
        if (start_lower == 0) {
            start_upper++;
        }
    }
}
#endif /* IPv6 */

void main_loop()
{
    int64_t lt;
    int64_t wait_time_ns;
    struct event *event;
    struct host_entry *h;

    while (event_queue_ping.first || event_queue_timeout.first) {
        dbg_printf("%s", "# main_loop\n");

        /* timeout event ? */
        if (event_queue_timeout.first && event_queue_timeout.first->ev_time - current_time_ns <= 0) {
            event = ev_dequeue(&event_queue_timeout);
            h = event->host;

            dbg_printf("%s [%d]: timeout event\n", h->host, event->ping_index);

            stats_add(h, event->ping_index, 0, -1);

            if (per_recv_flag) {
                if (timestamp_flag) {
                    print_timestamp_format(current_time_ns, timestamp_format_flag);
                }
                printf("%-*s : [%d], timed out",
                    max_hostname_len, h->host, event->ping_index);
                if (h->num_recv > 0) {
                    printf(" (%s avg, ", sprint_tm(h->total_time / h->num_recv));
                }
                else {
                    printf(" (NaN avg, ");
                }
                if (h->num_recv <= h->num_sent) {
                    printf("%d%% loss)",
                        ((h->num_sent - h->num_recv) * 100) / h->num_sent);
                }
                else {
                    printf("%d%% return)",
                        (h->num_recv_total * 100) / h->num_sent);
                }
                printf("\n");
            }

            /* do we need to send a retry? */
            if (!loop_flag && !count_flag) {
                if (h->num_sent < retry + 1) {
                    if (backoff_flag) {
                        h->timeout *= backoff;
                    }
                    send_ping(h, event->ping_index);
                }
            }

            /* note: we process first timeout events, because we might need to
             * wait to process ping events, while we for sure never need to
             * wait for timeout events.
             */
            continue;
        }

        /* ping event ? */
        if (event_queue_ping.first && event_queue_ping.first->ev_time - current_time_ns <= 0) {
            /* Make sure that we don't ping more than once every "interval" */
            lt = current_time_ns - last_send_time;
            if (lt < interval)
                goto wait_for_reply;

            /* Dequeue the event */
            event = ev_dequeue(&event_queue_ping);
            h = event->host;

            dbg_printf("%s [%d]: ping event\n", h->host, event->ping_index);

            /* Send the ping */
            send_ping(h, event->ping_index);

            /* Loop and count mode: schedule next ping */
            if (loop_flag || (count_flag && event->ping_index + 1 < count)) {
                host_add_ping_event(h, event->ping_index + 1, event->ev_time + perhost_interval);
            }
        }

    wait_for_reply:

        /* When is the next ping next event? */
        wait_time_ns = -1;
        if (event_queue_ping.first) {
            wait_time_ns = event_queue_ping.first->ev_time - current_time_ns;
            if (wait_time_ns < 0)
                wait_time_ns = 0;
            /* make sure that we wait enough, so that the inter-ping delay is
             * bigger than 'interval' */
            if (wait_time_ns < interval) {
                lt = current_time_ns - last_send_time;
                if (lt < interval) {
                    wait_time_ns = interval - lt;
                }
            }

            dbg_printf("next ping event in %.0f ms (%s)\n", wait_time_ns / 1e6, event_queue_ping.first->host->host);
        }

        /* When is the next timeout event? */
        if (event_queue_timeout.first) {
            int64_t wait_time_timeout = event_queue_timeout.first->ev_time - current_time_ns;
            if (wait_time_ns < 0 || wait_time_timeout < wait_time_ns) {
                wait_time_ns = wait_time_timeout;
                if (wait_time_ns < 0) {
                    wait_time_ns = 0;
                }
            }

            dbg_printf("next timeout event in %.0f ms (%s)\n", wait_time_timeout / 1e6, event_queue_timeout.first->host->host);
        }

        /* When is the next report due? */
        if (report_interval && (loop_flag || count_flag)) {
            int64_t wait_time_next_report = next_report_time - current_time_ns;
            if (wait_time_next_report < wait_time_ns) {
                wait_time_ns = wait_time_next_report;
                if (wait_time_ns < 0) {
                    wait_time_ns = 0;
                }
            }

            dbg_printf("next report  event in %0.f ms\n", wait_time_next_report / 1e6);
        }

        /* if wait_time is still -1, it means that we are waiting for nothing... */
        if (wait_time_ns == -1) {
            break;
        }

        /* end of loop was requested by interrupt signal handler */
        if (finish_requested) {
            break;
        }

        /* Receive replies */
        /* (this is what sleeps during each loop iteration) */
        dbg_printf("waiting up to %.0f ms\n", wait_time_ns / 1e6);
        if (wait_for_reply(wait_time_ns)) {
            while (wait_for_reply(0))
                ; /* process other replies in the queue */
        }

        update_current_time();

        if (status_snapshot) {
            status_snapshot = 0;
            print_per_system_splits();
        }

        /* Print report */
        if (report_interval && (loop_flag || count_flag) && (current_time_ns >= next_report_time)) {
            if (netdata_flag)
                print_netdata();
            else
                print_per_system_splits();

            while (current_time_ns >= next_report_time) {
                next_report_time += report_interval;
            }
        }
    }
}

/************************************************************

  Function: signal_handler

*************************************************************

  Inputs:  int signum

  Description:

  SIGQUIT signal handler - set flag and return
  SIGINT signal handler - set flag and return

************************************************************/

void signal_handler(int signum)
{
    switch (signum) {
    case SIGINT:
        finish_requested = 1;
        break;

    case SIGQUIT:
        status_snapshot = 1;
        break;
    }
}

/************************************************************

  Function: update_current_time

*************************************************************/

void update_current_time()
{
    clock_gettime(CLOCKID, &current_time);
    current_time_ns = timespec_ns(&current_time);
}

/************************************************************

  Function: finish

*************************************************************

  Inputs:  void (none)

  Description:

  Main program clean up and exit point

************************************************************/

void finish()
{
    int i;
    HOST_ENTRY *h;

    update_current_time();
    end_time = current_time_ns;

    /* tot up unreachables */
    for (i = 0; i < num_hosts; i++) {
        h = table[i];

        if (!h->num_recv) {
            num_unreachable++;

            if (verbose_flag || unreachable_flag) {
                printf("%s", h->host);

                if (verbose_flag)
                    printf(" is unreachable");

                printf("\n");
            }
        }
    }

    if (count_flag || loop_flag)
        print_per_system_stats();
#if defined(DEBUG) || defined(_DEBUG)
    else if (print_per_system_flag)
        print_per_system_stats();
#endif /* DEBUG || _DEBUG */

    if (stats_flag)
        print_global_stats();

    if (min_reachable) {
        if ((num_hosts - num_unreachable) >= min_reachable) {
            printf("Enough hosts reachable (required: %d, reachable: %d)\n", min_reachable, num_hosts - num_unreachable);
            exit(0);
        }
        else {
            printf("Not enough hosts reachable (required: %d, reachable: %d)\n", min_reachable, num_hosts - num_unreachable);
            exit(1);
        }
    }

    if (num_noaddress)
        exit(2);
    else if (num_alive != num_hosts)
        exit(1);

    exit(0);
}

/************************************************************

  Function: print_per_system_stats

*************************************************************

  Inputs:  void (none)

  Description:


************************************************************/

void print_per_system_stats(void)
{
    int i, j, avg, outage_ms;
    HOST_ENTRY *h;
    int64_t resp;

    if (verbose_flag || per_recv_flag)
        fprintf(stderr, "\n");

    for (i = 0; i < num_hosts; i++) {
        h = table[i];
        fprintf(stderr, "%-*s :", max_hostname_len, h->host);

        if (report_all_rtts_flag) {
            for (j = 0; j < h->num_sent; j++) {
                if ((resp = h->resp_times[j]) >= 0)
                    fprintf(stderr, " %s", sprint_tm(resp));
                else
                    fprintf(stderr, " -");
            }

            fprintf(stderr, "\n");
        }
        else {
            if (h->num_recv <= h->num_sent) {
                fprintf(stderr, " xmt/rcv/%%loss = %d/%d/%d%%",
                    h->num_sent, h->num_recv, h->num_sent > 0 ? ((h->num_sent - h->num_recv) * 100) / h->num_sent : 0);

                if (outage_flag) {
                    /* Time outage total */
                    outage_ms = (h->num_sent - h->num_recv) * perhost_interval / 1e6;
                    fprintf(stderr, ", outage(ms) = %d", outage_ms);
                }
            }
            else {
                fprintf(stderr, " xmt/rcv/%%return = %d/%d/%d%%",
                    h->num_sent, h->num_recv,
                    h->num_sent > 0 ? ((h->num_recv * 100) / h->num_sent) : 0);
            }

            if (h->num_recv) {
                avg = h->total_time / h->num_recv;
                fprintf(stderr, ", min/avg/max = %s", sprint_tm(h->min_reply));
                fprintf(stderr, "/%s", sprint_tm(avg));
                fprintf(stderr, "/%s", sprint_tm(h->max_reply));
            }

            fprintf(stderr, "\n");
        }
    }
}

/************************************************************

  Function: print_netdata

*************************************************************

  Inputs:  void (none)

  Description:


************************************************************/

void print_netdata(void)
{
    static int sent_charts = 0;

    int i;
    int64_t avg;
    HOST_ENTRY *h;

    for (i = 0; i < num_hosts; i++) {
        h = table[i];

        if (!sent_charts) {
            printf("CHART fping.%s_packets '' 'FPing Packets' packets '%s' fping.packets line 110020 %.0f\n", h->name, h->host, report_interval / 1e9);
            printf("DIMENSION xmt sent absolute 1 1\n");
            printf("DIMENSION rcv received absolute 1 1\n");
        }

        printf("BEGIN fping.%s_packets\n", h->name);
        printf("SET xmt = %d\n", h->num_sent_i);
        printf("SET rcv = %d\n", h->num_recv_i);
        printf("END\n");

        if (!sent_charts) {
            printf("CHART fping.%s_quality '' 'FPing Quality' percentage '%s' fping.quality area 110010 %.0f\n", h->name, h->host, report_interval / 1e9);
            printf("DIMENSION returned '' absolute 1 1\n");
            /* printf("DIMENSION lost '' absolute 1 1\n"); */
        }

        printf("BEGIN fping.%s_quality\n", h->name);
        /*
        if( h->num_recv_i <= h->num_sent_i )
            printf("SET lost = %d\n", h->num_sent_i > 0 ? ( ( h->num_sent_i - h->num_recv_i ) * 100 ) / h->num_sent_i : 0 );
        else
            printf("SET lost = 0\n");
*/

        printf("SET returned = %d\n", h->num_sent_i > 0 ? ((h->num_recv_i * 100) / h->num_sent_i) : 0);
        printf("END\n");

        if (!sent_charts) {
            printf("CHART fping.%s_latency '' 'FPing Latency' ms '%s' fping.latency area 110000 %.0f\n", h->name, h->host, report_interval / 1e9);
            printf("DIMENSION min minimum absolute 1 1000000\n");
            printf("DIMENSION max maximum absolute 1 1000000\n");
            printf("DIMENSION avg average absolute 1 1000000\n");
        }

        printf("BEGIN fping.%s_latency\n", h->name);
        if (h->num_recv_i) {
            avg = h->total_time_i / h->num_recv_i;
            printf("SET min = %" PRId64 "\n", h->min_reply_i);
            printf("SET avg = %" PRId64 "\n", avg);
            printf("SET max = %" PRId64 "\n", h->max_reply_i);
        }
        printf("END\n");

        stats_reset_interval(h);
    }

    sent_charts = 1;
}

/************************************************************

  Function: print_per_system_splits

*************************************************************

  Inputs:  void (none)

  Description:


************************************************************/

void print_per_system_splits(void)
{
    int i, avg, outage_ms_i;
    HOST_ENTRY *h;
    struct tm *curr_tm;

    if (verbose_flag || per_recv_flag)
        fprintf(stderr, "\n");

    update_current_time();
    curr_tm = localtime((time_t *)&current_time.tv_sec);
    fprintf(stderr, "[%2.2d:%2.2d:%2.2d]\n", curr_tm->tm_hour,
        curr_tm->tm_min, curr_tm->tm_sec);

    for (i = 0; i < num_hosts; i++) {
        h = table[i];
        fprintf(stderr, "%-*s :", max_hostname_len, h->host);

        if (h->num_recv_i <= h->num_sent_i) {
            fprintf(stderr, " xmt/rcv/%%loss = %d/%d/%d%%",
                h->num_sent_i, h->num_recv_i, h->num_sent_i > 0 ? ((h->num_sent_i - h->num_recv_i) * 100) / h->num_sent_i : 0);

            if (outage_flag) {
                /* Time outage  */
                outage_ms_i = (h->num_sent_i - h->num_recv_i) * perhost_interval / 1e6;
                fprintf(stderr, ", outage(ms) = %d", outage_ms_i);
            }
        }
        else {
            fprintf(stderr, " xmt/rcv/%%return = %d/%d/%d%%",
                h->num_sent_i, h->num_recv_i, h->num_sent_i > 0 ? ((h->num_recv_i * 100) / h->num_sent_i) : 0);
        }

        if (h->num_recv_i) {
            avg = h->total_time_i / h->num_recv_i;
            fprintf(stderr, ", min/avg/max = %s", sprint_tm(h->min_reply_i));
            fprintf(stderr, "/%s", sprint_tm(avg));
            fprintf(stderr, "/%s", sprint_tm(h->max_reply_i));
        }

        fprintf(stderr, "\n");
        if (!cumulative_stats_flag) {
            stats_reset_interval(h);
        }
    }
}

/************************************************************

  Function: print_global_stats

*************************************************************

  Inputs:  void (none)

  Description:


************************************************************/

void print_global_stats(void)
{
    fprintf(stderr, "\n");
    fprintf(stderr, " %7d targets\n", num_hosts);
    fprintf(stderr, " %7d alive\n", num_alive);
    fprintf(stderr, " %7d unreachable\n", num_unreachable);
    fprintf(stderr, " %7d unknown addresses\n", num_noaddress);
    fprintf(stderr, "\n");
    fprintf(stderr, " %7d timeouts (waiting for response)\n", num_timeout);
    fprintf(stderr, " %7d ICMP Echos sent\n", num_pingsent);
    fprintf(stderr, " %7d ICMP Echo Replies received\n", num_pingreceived);
    fprintf(stderr, " %7d other ICMP received\n", num_othericmprcvd);
    fprintf(stderr, "\n");

    if (total_replies == 0) {
        min_reply = 0;
        max_reply = 0;
        total_replies = 1;
        sum_replies = 0;
    }

    fprintf(stderr, " %s ms (min round trip time)\n", sprint_tm(min_reply));
    fprintf(stderr, " %s ms (avg round trip time)\n",
        sprint_tm(sum_replies / total_replies));
    fprintf(stderr, " %s ms (max round trip time)\n", sprint_tm(max_reply));
    fprintf(stderr, " %12.3f sec (elapsed real time)\n",
        (end_time - start_time) / 1e9);
    fprintf(stderr, "\n");
}

/************************************************************

  Function: send_ping

*************************************************************

  Inputs:  int s, HOST_ENTRY *h

  Description:

  Compose and transmit an ICMP_ECHO REQUEST packet.  The IP packet
  will be added on by the kernel.  The ID field is our UNIX process ID,
  and the sequence number is an index into an array of outstanding
  ping requests. The sequence number will later be used to quickly
  figure out who the ping reply came from.

************************************************************/

int send_ping(HOST_ENTRY *h, int index)
{
    int n;
    int myseq;
    int ret = 1;
    uint8_t proto = ICMP_ECHO;

    update_current_time();
    h->last_send_time = current_time_ns;
    myseq = seqmap_add(h->i, index, current_time_ns);

    dbg_printf("%s [%d]: send ping\n", h->host, index);

    if (h->saddr.ss_family == AF_INET && socket4 >= 0) {
        if(icmp_request_typ == 13)
            proto = ICMP_TSTAMP;
        n = socket_sendto_ping_ipv4(socket4, (struct sockaddr *)&h->saddr, h->saddr_len, myseq, ident4, proto);
    }
#ifdef IPV6
    else if (h->saddr.ss_family == AF_INET6 && socket6 >= 0) {
        n = socket_sendto_ping_ipv6(socket6, (struct sockaddr *)&h->saddr, h->saddr_len, myseq, ident6);
    }
#endif
    else {
        return 0;
    }

    /* error sending? */
    if (
        (n < 0)
#if defined(EHOSTDOWN)
        && errno != EHOSTDOWN
#endif
    ) {
        if (verbose_flag) {
            print_warning("%s: error while sending ping: %s\n", h->host, strerror(errno));
        }
        else {
            dbg_printf("%s: error while sending ping: %s\n", h->host, strerror(errno));
        }

        h->num_sent++;
        h->num_sent_i++;
        if (!loop_flag)
            h->resp_times[index] = RESP_ERROR;

        ret = 0;
    }
    else {
        /* schedule timeout */
        host_add_timeout_event(h, index, current_time_ns + h->timeout);

        /* mark this trial as outstanding */
        if (!loop_flag) {
            h->resp_times[index] = RESP_WAITING;
        }
    }

    num_pingsent++;
    last_send_time = h->last_send_time;

    return (ret);
}

int socket_can_read(struct timeval *timeout)
{
    int nfound;
    fd_set readset;
    int socketmax;

#ifndef IPV6
    socketmax = socket4;
#else
    socketmax = socket4 > socket6 ? socket4 : socket6;
#endif

select_again:
    FD_ZERO(&readset);
    if (socket4 >= 0)
        FD_SET(socket4, &readset);
#ifdef IPV6
    if (socket6 >= 0)
        FD_SET(socket6, &readset);
#endif

    nfound = select(socketmax + 1, &readset, NULL, NULL, timeout);
    if (nfound < 0) {
        if (errno == EINTR) {
            /* interrupted system call: redo the select */
            goto select_again;
        }
        else {
            perror("select");
        }
    }

    if (nfound > 0) {
        if (socket4 >= 0 && FD_ISSET(socket4, &readset)) {
            return socket4;
        }
#ifdef IPV6
        if (socket6 >= 0 && FD_ISSET(socket6, &readset)) {
            return socket6;
        }
#endif
    }

    return -1;
}

int receive_packet(int64_t wait_time,
#if HAVE_SO_TIMESTAMPNS
    int64_t *reply_timestamp,
#else
    int64_t *reply_timestamp __attribute__((unused)),
#endif
    struct sockaddr *reply_src_addr,
    size_t reply_src_addr_len,
    char *reply_buf,
    size_t reply_buf_len)
{
    struct timeval to;
    int s = 0;
    int recv_len;
    static unsigned char msg_control[40];
    struct iovec msg_iov = {
        reply_buf,
        reply_buf_len
    };
    struct msghdr recv_msghdr = {0};
    recv_msghdr.msg_name = reply_src_addr;
    recv_msghdr.msg_namelen = reply_src_addr_len;
    recv_msghdr.msg_iov = &msg_iov;
    recv_msghdr.msg_iovlen = 1;
    recv_msghdr.msg_control = &msg_control;
    recv_msghdr.msg_controllen = sizeof(msg_control);
#if HAVE_SO_TIMESTAMPNS
    struct cmsghdr *cmsg;
#endif

    /* Wait for a socket to become ready */
    if (wait_time) {
        to.tv_sec = wait_time / UINT64_C(1000000000);
        to.tv_usec = (wait_time % UINT64_C(1000000000)) / 1000 + 1;
    }
    else {
        to.tv_sec = 0;
        to.tv_usec = 0;
    }
    s = socket_can_read(&to);
    if (s == -1) {
        return 0; /* timeout */
    }

    recv_len = recvmsg(s, &recv_msghdr, MSG_TRUNC);
    if (recv_len <= 0) {
        return 0;
    }

#if HAVE_SO_TIMESTAMPNS
    /* ancilliary data */
    {
        struct timespec reply_timestamp_ts;
        struct timeval reply_timestamp_tv;
        for (cmsg = CMSG_FIRSTHDR(&recv_msghdr);
             cmsg != NULL;
             cmsg = CMSG_NXTHDR(&recv_msghdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS) {
                memcpy(&reply_timestamp_ts, CMSG_DATA(cmsg), sizeof(reply_timestamp_ts));
                *reply_timestamp = timespec_ns(&reply_timestamp_ts);
            }
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP) {
                memcpy(&reply_timestamp_tv, CMSG_DATA(cmsg), sizeof(reply_timestamp_tv));
                *reply_timestamp = timeval_ns(&reply_timestamp_tv);
            }
        }
    }
#endif

#if defined(DEBUG) || defined(_DEBUG)
    if (randomly_lose_flag) {
        if ((random() & 0x07) <= lose_factor)
            return 0;
    }
#endif

    return recv_len;
}

/* stats_add: update host statistics for a single packet that was received (or timed out)
 * h: host entry to update
 * index: if in count mode: index number for this ping packet (-1 otherwise)
 * success: 1 if response received, 0 otherwise
 * latency: response time, in ns
 */
void stats_add(HOST_ENTRY *h, int index, int success, int64_t latency)
{
    /* sent count - we update only on receive/timeout, so that we don't get
     * weird loss percentage, just because a packet was note recived yet.
     */
    h->num_sent++;
    h->num_sent_i++;

    if (!success) {
        if (!loop_flag && index >= 0) {
            h->resp_times[index] = RESP_TIMEOUT;
        }
        num_timeout++;
        return;
    }

    /* received count */
    h->num_recv++;
    h->num_recv_i++;

    /* maximum */
    if (!h->max_reply || latency > h->max_reply) {
        h->max_reply = latency;
    }
    if (!h->max_reply_i || latency > h->max_reply_i) {
        h->max_reply_i = latency;
    }

    /* minimum */
    if (!h->min_reply || latency < h->min_reply) {
        h->min_reply = latency;
    }
    if (!h->min_reply_i || latency < h->min_reply_i) {
        h->min_reply_i = latency;
    }

    /* total time (for average) */
    h->total_time += latency;
    h->total_time_i += latency;

    /* response time per-packet (count mode) */
    if (!loop_flag && index >= 0) {
        h->resp_times[index] = latency;
    }
}

/* stats_reset_interval: reset interval statistics
 * h: host entry to update
 */
void stats_reset_interval(HOST_ENTRY *h)
{
    h->num_sent_i = 0;
    h->num_recv_i = 0;
    h->max_reply_i = 0;
    h->min_reply_i = 0;
    h->total_time_i = 0;
}

int decode_icmp_ipv4(
    struct sockaddr *response_addr,
    size_t response_addr_len,
    char *reply_buf,
    size_t reply_buf_len,
    unsigned short *id,
    unsigned short *seq,
    int *ip_header_tos,
    int *ip_header_ttl,
    uint32_t *ip_header_otime_ms,
    uint32_t *ip_header_rtime_ms,
    uint32_t *ip_header_ttime_ms)
{
    struct icmp *icp;
    int hlen = 0;

    if (!using_sock_dgram4) {
        struct ip *ip = (struct ip *)reply_buf;
        *ip_header_tos = ip->ip_tos;
        *ip_header_ttl = ip->ip_ttl;

#if defined(__alpha__) && __STDC__ && !defined(__GLIBC__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
        /* The alpha headers are decidedly broken.
         * Using an ANSI compiler, it provides ip_vhl instead of ip_hl and
         * ip_v.  So, to get ip_hl, we mask off the bottom four bits.
         */
        hlen = (ip->ip_vhl & 0x0F) << 2;
#else
        hlen = ip->ip_hl << 2;
#endif
    }

    if (reply_buf_len < hlen + ICMP_MINLEN) {
        /* too short */
        if (verbose_flag) {
            char buf[INET6_ADDRSTRLEN];
            getnameinfo(response_addr, response_addr_len, buf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);
            printf("received packet too short for ICMP (%d bytes from %s)\n", (int)reply_buf_len, buf);
        }
        return -1;
    }

    icp = (struct icmp *)(reply_buf + hlen);

    if ((icmp_request_typ == 0 && icp->icmp_type != ICMP_ECHOREPLY) ||
        (icmp_request_typ == 13 && icp->icmp_type != ICMP_TSTAMPREPLY)) {
        /* Handle other ICMP packets */
        struct icmp *sent_icmp;
        SEQMAP_VALUE *seqmap_value;
        char addr_ascii[INET6_ADDRSTRLEN];
        HOST_ENTRY *h;

        /* reply icmp packet (hlen + ICMP_MINLEN) followed by "sent packet" (ip + icmp headers) */
        if (reply_buf_len < hlen + ICMP_MINLEN + sizeof(struct ip) + ICMP_MINLEN) {
            /* discard ICMP message if we can't tell that it was caused by us (i.e. if the "sent packet" is not included). */
            return -1;
        }

        sent_icmp = (struct icmp *)(reply_buf + hlen + ICMP_MINLEN + sizeof(struct ip));

        if ((icmp_request_typ == 0 && sent_icmp->icmp_type != ICMP_ECHO) ||
            (icmp_request_typ == 13 && sent_icmp->icmp_type != ICMP_TSTAMP) ||
            sent_icmp->icmp_id != ident4) {
            /* not caused by us */
            return -1;
        }

        seqmap_value = seqmap_fetch(ntohs(sent_icmp->icmp_seq), current_time_ns);
        if (seqmap_value == NULL) {
            return -1;
        }

        getnameinfo(response_addr, response_addr_len, addr_ascii, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);

        switch (icp->icmp_type) {
        case ICMP_UNREACH:
            h = table[seqmap_value->host_nr];
            if (icp->icmp_code > ICMP_UNREACH_MAXTYPE) {
                print_warning("ICMP Unreachable (Invalid Code) from %s for ICMP Echo sent to %s",
                    addr_ascii, h->host);
            }
            else {
                print_warning("%s from %s for ICMP Echo sent to %s",
                    icmp_unreach_str[icp->icmp_code], addr_ascii, h->host);
            }

            print_warning("\n");
            num_othericmprcvd++;
            break;

        case ICMP_SOURCEQUENCH:
        case ICMP_REDIRECT:
        case ICMP_TIMXCEED:
        case ICMP_PARAMPROB:
            h = table[seqmap_value->host_nr];
            if (icp->icmp_type <= ICMP_TYPE_STR_MAX) {
                print_warning("%s from %s for ICMP Echo sent to %s",
                    icmp_type_str[icp->icmp_type], addr_ascii, h->host);
            }
            else {
                print_warning("ICMP %d from %s for ICMP Echo sent to %s",
                    icp->icmp_type, addr_ascii, h->host);
            }
            print_warning("\n");
            num_othericmprcvd++;
            break;
        }

        return -1;
    }

    *id = icp->icmp_id;
    *seq = ntohs(icp->icmp_seq);
    if(icp->icmp_type == ICMP_TSTAMPREPLY) {

        /* Check that reply_buf_len is sufficiently big to contain the timestamps */
        if (reply_buf_len < hlen + ICMP_MINLEN + ICMP_TIMESTAMP_DATA_SIZE) {
            if (verbose_flag) {
                char buf[INET6_ADDRSTRLEN];
                getnameinfo(response_addr, response_addr_len, buf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);
                printf("received packet too short for ICMP Timestamp Reply (%d bytes from %s)\n", (int)reply_buf_len, buf);
            }
            return -1;
        }

        *ip_header_otime_ms = ntohl(icp->icmp_dun.id_ts.its_otime);
        *ip_header_rtime_ms = ntohl(icp->icmp_dun.id_ts.its_rtime);
        *ip_header_ttime_ms = ntohl(icp->icmp_dun.id_ts.its_ttime);
    }

    return hlen;
}

#ifdef IPV6
int decode_icmp_ipv6(
    struct sockaddr *response_addr,
    size_t response_addr_len,
    char *reply_buf,
    size_t reply_buf_len,
    unsigned short *id,
    unsigned short *seq)
{
    struct icmp6_hdr *icp;

    if (reply_buf_len < sizeof(struct icmp6_hdr)) {
        if (verbose_flag) {
            char buf[INET6_ADDRSTRLEN];
            getnameinfo(response_addr, response_addr_len, buf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);
            printf("received packet too short for ICMP (%d bytes from %s)\n", (int)reply_buf_len, buf);
        }
        return 0; /* too short */
    }

    icp = (struct icmp6_hdr *)reply_buf;

    if (icp->icmp6_type != ICMP6_ECHO_REPLY) {
        /* Handle other ICMP packets */
        struct icmp6_hdr *sent_icmp;
        SEQMAP_VALUE *seqmap_value;
        char addr_ascii[INET6_ADDRSTRLEN];
        HOST_ENTRY *h;

        /* reply icmp packet (ICMP_MINLEN) followed by "sent packet" (ip + icmp headers) */
        if (reply_buf_len < ICMP_MINLEN + sizeof(struct ip) + ICMP_MINLEN) {
            /* discard ICMP message if we can't tell that it was caused by us (i.e. if the "sent packet" is not included). */
            return 0;
        }

        sent_icmp = (struct icmp6_hdr *)(reply_buf + sizeof(struct icmp6_hdr) + sizeof(struct ip));

        if (sent_icmp->icmp6_type != ICMP_ECHO || sent_icmp->icmp6_id != ident6) {
            /* not caused by us */
            return 0;
        }

        seqmap_value = seqmap_fetch(ntohs(sent_icmp->icmp6_seq), current_time_ns);
        if (seqmap_value == NULL) {
            return 0;
        }

        getnameinfo(response_addr, response_addr_len, addr_ascii, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);

        switch (icp->icmp6_type) {
        case ICMP_UNREACH:
            h = table[seqmap_value->host_nr];
            if (icp->icmp6_code > ICMP_UNREACH_MAXTYPE) {
                print_warning("ICMP Unreachable (Invalid Code) from %s for ICMP Echo sent to %s",
                    addr_ascii, h->host);
            }
            else {
                print_warning("%s from %s for ICMP Echo sent to %s",
                    icmp_unreach_str[icp->icmp6_code], addr_ascii, h->host);
            }

            print_warning("\n");
            num_othericmprcvd++;
            break;

        case ICMP_SOURCEQUENCH:
        case ICMP_REDIRECT:
        case ICMP_TIMXCEED:
        case ICMP_PARAMPROB:
            h = table[seqmap_value->host_nr];
            if (icp->icmp6_type <= ICMP_TYPE_STR_MAX) {
                print_warning("%s from %s for ICMP Echo sent to %s",
                    icmp_type_str[icp->icmp6_type], addr_ascii, h->host);
            }
            else {
                print_warning("ICMP %d from %s for ICMP Echo sent to %s",
                    icp->icmp6_type, addr_ascii, h->host);
            }
            print_warning("\n");
            num_othericmprcvd++;
            break;
        }

        return 0;
    }

    *id = icp->icmp6_id;
    *seq = ntohs(icp->icmp6_seq);

    return 1;
}
#endif

int wait_for_reply(int64_t wait_time)
{
    int result;
    static char buffer[RECV_BUFSIZE];
    struct sockaddr_storage response_addr;
    int n, avg;
    HOST_ENTRY *h;
    int64_t this_reply;
    int this_count;
    int64_t recv_time = 0;
    SEQMAP_VALUE *seqmap_value;
    unsigned short id;
    unsigned short seq;
    int ip_header_tos = -1;
    int ip_header_ttl = -1;
    // ICMP Timestamp
    uint32_t ip_header_otime_ms = 0x80000000U;
    uint32_t ip_header_rtime_ms = 0x80000000U;
    uint32_t ip_header_ttime_ms = 0x80000000U;

    /* Receive packet */
    result = receive_packet(wait_time, /* max. wait time, in ns */
        &recv_time, /* reply_timestamp */
        (struct sockaddr *)&response_addr, /* reply_src_addr */
        sizeof(response_addr), /* reply_src_addr_len */
        buffer, /* reply_buf */
        sizeof(buffer) /* reply_buf_len */
    );

    if (result <= 0) {
        return 0;
    }

    update_current_time();
    if (recv_time == 0)
        recv_time = current_time_ns;

    /* Process ICMP packet and retrieve id/seq */
    if (response_addr.ss_family == AF_INET) {
        int ip_hlen = decode_icmp_ipv4(
            (struct sockaddr *)&response_addr,
            sizeof(response_addr),
            buffer,
            sizeof(buffer),
            &id,
            &seq,
            &ip_header_tos,
            &ip_header_ttl,
            &ip_header_otime_ms,
            &ip_header_rtime_ms,
            &ip_header_ttime_ms);
        if (ip_hlen < 0) {
            return 1;
        }
        if (id != ident4) {
            return 1; /* packet received, but not the one we are looking for! */
        }
        if (!using_sock_dgram4) {
            /* do not include IP header in returned size, to be consistent with ping(8) and also
             * with fping with IPv6 hosts */
            result -= ip_hlen;
        }
    }
#ifdef IPV6
    else if (response_addr.ss_family == AF_INET6) {
        if (!decode_icmp_ipv6(
                (struct sockaddr *)&response_addr,
                sizeof(response_addr),
                buffer,
                sizeof(buffer),
                &id,
                &seq)) {
            return 1;
        }
        if (id != ident6) {
            return 1; /* packet received, but not the one we are looking for! */
        }
    }
#endif
    else {
        return 1;
    }

    seqmap_value = seqmap_fetch(seq, current_time_ns);
    if (seqmap_value == NULL) {
        return 1;
    }

    /* find corresponding host_entry */
    n = seqmap_value->host_nr;
    h = table[n];
    this_count = seqmap_value->ping_count;
    this_reply = recv_time - seqmap_value->ping_ts;

    /* update stats that include invalid replies */
    h->num_recv_total++;
    num_pingreceived++;

    dbg_printf("received [%d] from %s\n", this_count, h->host);

    /* optionally require reply source equal to target address */
    if (check_source_flag && addr_cmp((struct sockaddr *)&response_addr, (struct sockaddr *)&h->saddr)) {
        dbg_printf("%s\n", "discarding reply from wrong source address");
        return 1;
    }

    /* discard duplicates */
    if (!loop_flag && h->resp_times[this_count] >= 0) {
        if (!per_recv_flag) {
            fprintf(stderr, "%s : duplicate for [%d], %d bytes, %s ms",
                h->host, this_count, result, sprint_tm(this_reply));

            if (addr_cmp((struct sockaddr *)&response_addr, (struct sockaddr *)&h->saddr)) {
                char buf[INET6_ADDRSTRLEN];
                getnameinfo((struct sockaddr *)&response_addr, sizeof(response_addr), buf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);
                fprintf(stderr, " [<- %s]", buf);
            }
            fprintf(stderr, "\n");
        }
        return 1;
    }

    /* discard reply if delay is larger than timeout
     * (see also: github #32) */
    if (this_reply > h->timeout) {
        return 1;
    }

    /* update stats */
    stats_add(h, this_count, 1, this_reply);
    // TODO: move to stats_add?
    if (!max_reply || this_reply > max_reply)
        max_reply = this_reply;
    if (!min_reply || this_reply < min_reply)
        min_reply = this_reply;
    sum_replies += this_reply;
    total_replies++;

    /* initialize timeout to initial timeout (without backoff) */
    h->timeout = timeout;

    /* remove timeout event */
    struct event *timeout_event = host_get_timeout_event(h, this_count);
    if (timeout_event) {
        ev_remove(&event_queue_timeout, timeout_event);
    }

    /* print "is alive" */
    if (h->num_recv == 1) {
        num_alive++;
        if (fast_reachable && num_alive >= min_reachable)
                finish_requested = 1;

        if (verbose_flag || alive_flag) {
            printf("%s", h->host);

            if (verbose_flag)
                printf(" is alive");
        }
    }

    /* print received ping (unless --quiet) */
    if (per_recv_flag) {
        if (timestamp_flag) {
            print_timestamp_format(recv_time, timestamp_format_flag);
        }
        avg = h->total_time / h->num_recv;
        printf("%-*s : [%d], %d bytes, %s ms",
            max_hostname_len, h->host, this_count, result, sprint_tm(this_reply));
        printf(" (%s avg, ", sprint_tm(avg));

        if (h->num_recv <= h->num_sent) {
            printf("%d%% loss)",
                ((h->num_sent - h->num_recv) * 100) / h->num_sent);
        }
        else {
            printf("%d%% return)",
                (h->num_recv_total * 100) / h->num_sent);
        }
    }

    if (verbose_flag || alive_flag || per_recv_flag) {

        if (addr_cmp((struct sockaddr *)&response_addr, (struct sockaddr *)&h->saddr)) {
            char buf[INET6_ADDRSTRLEN];
            getnameinfo((struct sockaddr *)&response_addr, sizeof(response_addr), buf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST);
            fprintf(stderr, " [<- %s]", buf);
        }

        if (icmp_request_typ == 13) {
            printf("%s timestamps: Originate=%u Receive=%u Transmit=%u Localreceive=%u",
                   alive_flag ? "" : ",",
                   ip_header_otime_ms, ip_header_rtime_ms, ip_header_ttime_ms,
                   ms_since_midnight_utc(recv_time));
        }

        if(print_tos_flag) {
            if(ip_header_tos != -1) {
                printf(" (TOS %d)", ip_header_tos);
            }
            else {
                printf(" (TOS unknown)");
            }
        }

        if (print_ttl_flag) {
          if(ip_header_ttl != -1) {
              printf(" (TTL %d)", ip_header_ttl);
          }
          else {
              printf(" (TTL unknown)");
          }
        }

        if (elapsed_flag && !per_recv_flag)
                printf(" (%s ms)", sprint_tm(this_reply));

        printf("\n");
    }

    return 1;
}

/************************************************************

  Function: add_name

*************************************************************

  Inputs:  char* name

  Description:

  process input name for addition to target list
  name can turn into multiple targets via multiple interfaces (-m)
  or via NIS groups

************************************************************/

void add_name(char *name)
{
    struct addrinfo *res0, *res, hints;
    int ret_ga;
    char *printname;
    char namebuf[256];
    char addrbuf[256];

    /* getaddrinfo */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_flags = AI_UNUSABLE;
    hints.ai_socktype = SOCK_RAW;
    hints.ai_family = hints_ai_family;
    if (hints_ai_family == AF_INET) {
        hints.ai_protocol = IPPROTO_ICMP;
    }
#ifdef IPV6
    else if (hints_ai_family == AF_INET6) {
        hints.ai_protocol = IPPROTO_ICMPV6;
    }
#endif
    else {
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;
    }
    ret_ga = getaddrinfo(name, NULL, &hints, &res0);
    if (ret_ga) {
        if (!quiet_flag)
            print_warning("%s: %s\n", name, gai_strerror(ret_ga));
        num_noaddress++;
        return;
    }

    /* NOTE: we could/should loop with res on all addresses like this:
     * for (res = res0; res; res = res->ai_next) {
     * We don't do it yet, however, because is is an incompatible change
     * (need to implement a separate option for this)
     */
    for (res = res0; res; res = res->ai_next) {
        /* name_flag: addr -> name lookup requested) */
        if (name_flag || rdns_flag) {
            int do_rdns = rdns_flag ? 1 : 0;
            if (name_flag) {
                /* Was it a numerical address? Only then do a rdns-query */
                struct addrinfo *nres;
                hints.ai_flags = AI_NUMERICHOST;
                if (getaddrinfo(name, NULL, &hints, &nres) == 0) {
                    do_rdns = 1;
                    freeaddrinfo(nres);
                }
            }

            if (do_rdns && getnameinfo(res->ai_addr, res->ai_addrlen, namebuf, sizeof(namebuf) / sizeof(char), NULL, 0, 0) == 0) {
                printname = namebuf;
            }
            else {
                printname = name;
            }
        }
        else {
            printname = name;
        }

        /* addr_flag: name -> addr lookup requested */
        if (addr_flag) {
            int ret;
            ret = getnameinfo(res->ai_addr, res->ai_addrlen, addrbuf,
                sizeof(addrbuf) / sizeof(char), NULL, 0, NI_NUMERICHOST);
            if (ret) {
                if (!quiet_flag) {
                    print_warning("%s: can't forward-lookup address (%s)\n", name, gai_strerror(ret));
                }
                continue;
            }

            if (name_flag || rdns_flag) {
                char nameaddrbuf[512 + 3];
                snprintf(nameaddrbuf, sizeof(nameaddrbuf) / sizeof(char), "%s (%s)", printname, addrbuf);
                add_addr(name, nameaddrbuf, res->ai_addr, res->ai_addrlen);
            }
            else {
                add_addr(name, addrbuf, res->ai_addr, res->ai_addrlen);
            }
        }
        else {
            add_addr(name, printname, res->ai_addr, res->ai_addrlen);
        }

        if (!multif_flag) {
            break;
        }
    }

    freeaddrinfo(res0);
}

/************************************************************

  Function: add_addr

*************************************************************

  Description:

  add single address to list of hosts to be pinged

************************************************************/

void add_addr(char *name, char *host, struct sockaddr *ipaddr, socklen_t ipaddr_len)
{
    HOST_ENTRY *p;
    int n;
    int64_t *i;

    p = (HOST_ENTRY *)calloc(1, sizeof(HOST_ENTRY));
    if (!p)
        crash_and_burn("can't allocate HOST_ENTRY");

    p->name = strdup(name);
    p->host = strdup(host);
    memcpy(&p->saddr, ipaddr, ipaddr_len);
    p->saddr_len = ipaddr_len;
    p->timeout = timeout;
    p->min_reply = 0;

    if (netdata_flag) {
        char *s = p->name;
        while (*s) {
            if (!isalnum(*s))
                *s = '_';
            s++;
        }
    }

    if (strlen(p->host) > max_hostname_len)
        max_hostname_len = strlen(p->host);

    /* array for response time results */
    if (!loop_flag) {
        i = (int64_t *)malloc(trials * sizeof(int64_t));
        if (!i)
            crash_and_burn("can't allocate resp_times array");

        for (n = 1; n < trials; n++)
            i[n] = RESP_UNUSED;

        p->resp_times = i;
    }

    /* allocate event storage */
    p->event_storage_ping = (struct event *)calloc(event_storage_count, sizeof(struct event));
    p->event_storage_timeout = (struct event *)calloc(event_storage_count, sizeof(struct event));

    /* schedule first ping */
    host_add_ping_event(p, 0, current_time_ns);

    num_hosts++;
}

/************************************************************

  Function: crash_and_burn

*************************************************************

  Inputs:  char* message

  Description:

************************************************************/

void crash_and_burn(char *message)
{
    fprintf(stderr, "%s: %s\n", prog, message);
    exit(4);
}

/************************************************************

  Function: errno_crash_and_burn

*************************************************************

  Inputs:  char* message

  Description:

************************************************************/

void errno_crash_and_burn(char *message)
{
    fprintf(stderr, "%s: %s : %s\n", prog, message, strerror(errno));
    exit(4);
}

/************************************************************

  Function: print_warning

  Description: fprintf(stderr, ...), unless running with -q

*************************************************************/

void print_warning(char *format, ...)
{
    va_list args;
    if (!quiet_flag) {
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
}

/************************************************************

  Function: sprint_tm

*************************************************************

  render nanosecond int64_t value into milliseconds string with three digits of
  precision.

************************************************************/

const char *sprint_tm(int64_t ns)
{
    static char buf[10];
    double t = (double)ns / 1e6;

    if (t < 0.0) {
        /* negative (unexpected) */
        sprintf(buf, "%.2g", t);
    }
    else if (t < 1.0) {
        /* <= 0.99 ms */
        sprintf(buf, "%.3f", t);
    }
    else if (t < 10.0) {
        /* 1.00 - 9.99 ms */
        sprintf(buf, "%.2f", t);
    }
    else if (t < 100.0) {
        /* 10.0 - 99.9 ms */
        sprintf(buf, "%.1f", t);
    }
    else if (t < 1000000.0) {
        /* 100 - 1'000'000 ms */
        sprintf(buf, "%.0f", t);
    }
    else {
        sprintf(buf, "%.3e", t);
    }

    return (buf);
}

/************************************************************

  Function: addr_cmp

*************************************************************/
int addr_cmp(struct sockaddr *a, struct sockaddr *b)
{
    if (a->sa_family != b->sa_family) {
        return a->sa_family - b->sa_family;
    }
    else {
        if (a->sa_family == AF_INET) {
            return ((struct sockaddr_in *)a)->sin_addr.s_addr - ((struct sockaddr_in *)b)->sin_addr.s_addr;
        }
        else if (a->sa_family == AF_INET6) {
            return memcmp(&((struct sockaddr_in6 *)a)->sin6_addr,
                &((struct sockaddr_in6 *)b)->sin6_addr,
                sizeof(((struct sockaddr_in6 *)a)->sin6_addr));
        }
    }

    return 0;
}

void host_add_ping_event(HOST_ENTRY *h, int index, int64_t ev_time)
{
    struct event *event = &h->event_storage_ping[index % event_storage_count];
    event->host = h;
    event->ping_index = index;
    event->ev_time = ev_time;
    ev_enqueue(&event_queue_ping, event);

    dbg_printf("%s [%d]: add ping event in %.0f ms\n",
        event->host->host, index, (ev_time - current_time_ns) / 1e6);
}

void host_add_timeout_event(HOST_ENTRY *h, int index, int64_t ev_time)
{
    struct event *event = &h->event_storage_timeout[index % event_storage_count];
    event->host = h;
    event->ping_index = index;
    event->ev_time = ev_time;
    ev_enqueue(&event_queue_timeout, event);

    dbg_printf("%s [%d]: add timeout event in %.0f ms\n",
        event->host->host, index, (ev_time - current_time_ns) / 1e6);
}

struct event *host_get_timeout_event(HOST_ENTRY *h, int index)
{
    return &h->event_storage_timeout[index % event_storage_count];
}

/************************************************************

  Function: ev_enqueue

  Enqueue an event

  The queue is sorted by event->ev_time, so that queue->first always points to
  the earliest event.

  We start scanning the queue from the tail, because we assume
  that new events mostly get inserted with a event time higher
  than the others.

*************************************************************/
void ev_enqueue(struct event_queue *queue, struct event *event)
{
    struct event *i;
    struct event *i_prev;

    /* Empty list */
    if (queue->last == NULL) {
        event->ev_next = NULL;
        event->ev_prev = NULL;
        queue->first = event;
        queue->last = event;
        return;
    }

    /* Insert on tail? */
    if (event->ev_time - queue->last->ev_time >= 0) {
        event->ev_next = NULL;
        event->ev_prev = queue->last;
        queue->last->ev_next = event;
        queue->last = event;
        return;
    }

    /* Find insertion point */
    i = queue->last;
    while (1) {
        i_prev = i->ev_prev;
        if (i_prev == NULL || event->ev_time - i_prev->ev_time >= 0) {
            event->ev_prev = i_prev;
            event->ev_next = i;
            i->ev_prev = event;
            if (i_prev != NULL) {
                i_prev->ev_next = event;
            }
            else {
                queue->first = event;
            }
            return;
        }
        i = i_prev;
    }
}

/************************************************************

  Function: ev_dequeue

*************************************************************/
struct event *ev_dequeue(struct event_queue *queue)
{
    struct event *dequeued;

    if (queue->first == NULL) {
        return NULL;
    }
    dequeued = queue->first;
    ev_remove(queue, dequeued);

    return dequeued;
}

/************************************************************

  Function: ev_remove

*************************************************************/
void ev_remove(struct event_queue *queue, struct event *event)
{
    if (queue->first == event) {
        queue->first = event->ev_next;
    }
    if (queue->last == event) {
        queue->last = event->ev_prev;
    }
    if (event->ev_prev) {
        event->ev_prev->ev_next = event->ev_next;
    }
    if (event->ev_next) {
        event->ev_next->ev_prev = event->ev_prev;
    }
    event->ev_prev = NULL;
    event->ev_next = NULL;
}

/************************************************************

  Function: print_human_readable_time from current_time_ns

*************************************************************/
void print_timestamp_format(int64_t current_time_ns, int timestamp_format)
{
    char time_buffer[100];
    time_t current_time_s;
    struct tm *local_time;

    current_time_s = current_time_ns / 1000000000;
    local_time = localtime(&current_time_s);
    switch(timestamp_format) {
        case 1:
            // timestamp-format ctime
            strftime(time_buffer, sizeof(time_buffer), "%c", local_time);
            printf("[%s] ", time_buffer);
            break;
        case 2:
            // timestamp-format iso
            strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%dT%T%z", local_time);
            printf("[%s] ", time_buffer);
            break;
        case 3:
            // timestamp-format rfc3339
            strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", local_time);
            printf("[%s] ", time_buffer);
            break;
        default:
            printf("[%.5f] ", (double)current_time_ns / 1e9);
    }
}

/************************************************************

  Function: ms_since_midnight_utc

*************************************************************

  Input: int64_t: current UTC time in ns

  Output: uint32_t: current time in ms since midnight UTC

  Description:

  Return ICMP Timestamp value corresponding to the given time value.
  The given time value must be in UTC.

*************************************************************/
static uint32_t ms_since_midnight_utc(int64_t time_val)
{
    return (uint32_t)((time_val / 1000000) % (24 * 60 * 60 * 1000));
}

/************************************************************

  Function: usage

*************************************************************

  Inputs:  int: 0 if output on request, 1 if output because of wrong argument

  Description:

************************************************************/

void usage(int is_error)
{
    FILE *out = is_error ? stderr : stdout;
    fprintf(out, "Usage: %s [options] [targets...]\n", prog);
    fprintf(out, "\n");
    fprintf(out, "Probing options:\n");
    fprintf(out, "   -4, --ipv4         only ping IPv4 addresses\n");
    fprintf(out, "   -6, --ipv6         only ping IPv6 addresses\n");
    fprintf(out, "   -b, --size=BYTES   amount of ping data to send, in bytes (default: %d)\n", DEFAULT_PING_DATA_SIZE);
    fprintf(out, "   -B, --backoff=N    set exponential backoff factor to N (default: 1.5)\n");
    fprintf(out, "   -c, --count=N      count mode: send N pings to each target and report stats\n");
    fprintf(out, "   -f, --file=FILE    read list of targets from a file ( - means stdin)\n");
    fprintf(out, "   -g, --generate     generate target list (only if no -f specified),\n");
    fprintf(out, "                      limited to at most %d targets\n", MAX_GENERATE);
    fprintf(out, "                      (give start and end IP in the target list, or a CIDR address)\n");
    fprintf(out, "                      (ex. %s -g 192.168.1.0 192.168.1.255 or %s -g 192.168.1.0/24)\n", prog, prog);
    fprintf(out, "   -H, --ttl=N        set the IP TTL value (Time To Live hops)\n");
    fprintf(out, "   -i, --interval=MSEC  interval between sending ping packets (default: %.0f ms)\n", interval / 1e6);
#ifdef SO_BINDTODEVICE
    fprintf(out, "   -I, --iface=IFACE  bind to a particular interface\n");
#endif
#ifdef SO_MARK
    fprintf(out, "   -k, --fwmark=FWMARK set the routing mark\n");
#endif
    fprintf(out, "   -l, --loop         loop mode: send pings forever\n");
    fprintf(out, "   -m, --all          use all IPs of provided hostnames (e.g. IPv4 and IPv6), use with -A\n");
    fprintf(out, "   -M, --dontfrag     set the Don't Fragment flag\n");
    fprintf(out, "   -O, --tos=N        set the type of service (tos) flag on the ICMP packets\n");
    fprintf(out, "   -p, --period=MSEC  interval between ping packets to one target (in ms)\n");
    fprintf(out, "                      (in loop and count modes, default: %.0f ms)\n", perhost_interval / 1e6);
    fprintf(out, "   -r, --retry=N      number of retries (default: %d)\n", DEFAULT_RETRY);
    fprintf(out, "   -R, --random       random packet data (to foil link data compression)\n");
    fprintf(out, "   -S, --src=IP       set source address\n");
    fprintf(out, "   -t, --timeout=MSEC individual target initial timeout (default: %.0f ms,\n", timeout / 1e6);
    fprintf(out, "                      except with -l/-c/-C, where it's the -p period up to 2000 ms)\n");
    fprintf(out, "       --check-source discard replies not from target address\n");
    fprintf(out, "       --icmp-timestamp use ICMP Timestamp instead of ICMP Echo\n");
    fprintf(out, "\n");
    fprintf(out, "Output options:\n");
    fprintf(out, "   -a, --alive        show targets that are alive\n");
    fprintf(out, "   -A, --addr         show targets by address\n");
    fprintf(out, "   -C, --vcount=N     same as -c, report results (not stats) in verbose format\n");
    fprintf(out, "   -d, --rdns         show targets by name (force reverse-DNS lookup)\n");
    fprintf(out, "   -D, --timestamp    print timestamp before each output line\n");
    fprintf(out, "       --timestamp-format=FORMAT  show timestamp in the given format (-D required): ctime|iso|rfc3339\n");
    fprintf(out, "   -e, --elapsed      show elapsed time on return packets\n");
    fprintf(out, "   -n, --name         show targets by name (reverse-DNS lookup for target IPs)\n");
    fprintf(out, "   -N, --netdata      output compatible for netdata (-l -Q are required)\n");
    fprintf(out, "   -o, --outage       show the accumulated outage time (lost packets * packet interval)\n");
    fprintf(out, "   -q, --quiet        quiet (don't show per-target/per-ping results)\n");
    fprintf(out, "   -Q, --squiet=SECS[,cumulative]  same as -q, but add interval summary every SECS seconds,\n");
    fprintf(out, "                                   with 'cumulative', print stats since beginning\n");
    fprintf(out, "   -s, --stats        print final stats\n");
    fprintf(out, "   -u, --unreach      show targets that are unreachable\n");
    fprintf(out, "   -v, --version      show version\n");
    fprintf(out, "   -x, --reachable=N  shows if >=N hosts are reachable or not\n");
    fprintf(out, "   -X, --fast-reachable=N exits true immediately when N hosts are found\n");
    fprintf(out, "       --print-tos    show received TOS value\n");
    fprintf(out, "       --print-ttl    show IP TTL value\n");
    exit(is_error);
}
