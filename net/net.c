/*
 * $Id: net.c,v 1.23 2003/04/18 02:04:15 timl Exp $
 *
 * net module - ici sockets interface
 *
 * This is a set of extra intrinsic functions to provide ici programs
 * with access to the BSD sockets interface and the IP protocol. The
 * ICI interface adopts practices suitable for the ICI environment and
 * introduces a new object type, socket, to represent sockets. Functions
 * follow the BSD model adapted to the ici environment. In particular
 * network addresses are represented using strings and there is support
 * for turning sockets into files to allow stream I/O via printf and
 * the like.
 *
 * The module is mostly portable to Windows. Some Unix specific
 * functions are not provided in the Windows version of the module
 * (currently only the socketpair() function is missing).
 *
 */

/*
 * Sockets based networking
 *
 * The ICI 'net' module provides sockets style network interface functions.
 * It is available on systems that provide BSD-compatible sockets calls and
 * for Win32 platforms.  The sockets extension is generally compatible with
 * the C sockets functions, but uses types and calling semantics more akin to
 * the ICI environment.
 *
 * The sockets extension introduces a new type, 'socket', to hold socket
 * objects.  The function, 'net.socket()', returns a 'socket'
 * object.
 *
 * This --intro-- and --synopsis-- forms part of --ici-net-- documentation.
 */

#include <ici.h>
#include "icistr.h"
#include <icistr-setup.h>

#ifdef  _WIN32
#define USE_WINSOCK /* Else use UNIX style sockets. */
#endif

/*
 * Substitute a function "ici_isset" for ICI's macro, because it
 * conflicts with the isset macro in sys/param.h.
 */
static int
ici_isset(object_t *o)
{
    return isset(o);
}
#undef isset

#ifdef  USE_WINSOCK
/*
 * Windows uses a special type to represent its SOCKET descriptors.
 * For correctness we include winsock.h here. Other platforms (i.e.,
 * BSD) are easier and use integers.
 */
#include <winsock.h>
/*
 * The f_hostname() function needs to know how long a host name may
 * be. WINSOCK doesn't seem to want to tell us.
 */
#define MAXHOSTNAMELEN (64)

#else /* USE_WINSOCK */

/*
 * For ease of compatibility with WINSOCK we use its definitions and
 * emulate them on Unix. Luckily such emulation is rather trivial.
 */
#define SOCKET          int
#define closesocket     close
#define SOCKET_ERROR    (-1)

#include <errno.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* for TCP_NODELAY (on most systems) */
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pwd.h>
#if __hpux || BSD4_4 || linux
#include <unistd.h>
#endif
#if NeXT
#include <libc.h>
#endif
#if sun
#define bzero(p,n)      (memset((p), 0, (n)))
#endif

#endif /* USE_WINSOCK else */

#if defined(ULTRIX) || defined(BSD4_4) && !defined(INADDR_LOOPBACK)
/*
 * Local loop back IP address (127.0.0.1) in host byte order.
 */
#define INADDR_LOOPBACK         (u_long)0x7F000001
#endif

#if defined(sun) && __STDC__
/*
 * SunOS 4.x doesn't prototype many things.
 */
int close(int);
int socket(int, int, int);
int socketpair(int, int, int, int *);
int listen(int, int);
#ifndef SUNOS5
int accept(int, struct sockaddr *, int *);
int connect(int, struct sockaddr *, int);
int bind(int, struct sockaddr *, int);
int sendto(int, char *, int, int, struct sockaddr *, int);
#endif
#ifndef SUNOS5
int send(int, char *, int, int);
#endif
#ifndef SUNOS5
int recvfrom(int, char *, int, int, struct sockaddr *, int *);
int recv(int, char *, int, int);
int getsockopt(int, int, int, char *, int *);
int setsockopt(int, int, int, char *, int);
int getpeername(int, struct sockaddr *, int *);
int getsockname(int, struct sockaddr *, int *);
#endif
int gethostname(char *, int);
int getuid(void);
int shutdown(int, int);
#endif

/*
 * Set the error string using a static message or a formatted message.
 * If fmt is NULL then the fallback string is used to set the ici error
 * string.  If fmt is non-NULL it is used to format a message using
 * sprintf and any actual parameters.  The formatted message must fit
 * within 256 characters. If ici_buf cannot size itself to contain the
 * formatted message the fallback message is used to set error.
 */
static int
seterror(char *fallback, char *fmt, ...)
{
    va_list     va;

    if (fmt == NULL || ici_chkbuf(256))
        ici_error = fallback;
    else
    {
        va_start(va, fmt);
        vsprintf(ici_buf, fmt, va);
        va_end(va);
        ici_error = ici_buf;
    }
    return 1;
}

/*
 * The handle representing our socket it about to be freed. Close the
 * socket if it isn't already.
 */
static void
socket_prefree(ici_handle_t *h)
{
    if ((objof(h)->o_flags & H_CLOSED) == 0)
        closesocket((SOCKET)h->h_ptr);
}

/*
 * Create a new socket object with the given descriptor.
 */
static ici_handle_t *
new_netsocket(SOCKET fd)
{
    ici_handle_t *h;

    if ((h = ici_handle_new((void *)fd, ICIS(socket), NULL)) == NULL)
        return NULL;
    objof(h)->o_flags &= ~H_CLOSED;
    h->h_pre_free = socket_prefree;
    /*
     * Turn off super support. This means you can't assign or fetch
     * values in a socket.
     */
    objof(h)->o_flags &= ~O_SUPER;
    return h;
}

/*
 * Is a socket closed? Set error if so.
 */
static int
isclosed(ici_handle_t *skt)
{
    if (!(objof(skt)->o_flags & H_CLOSED))
        return 0;
    ici_error = "attempt to use closed socket";
    return 1;
}

/*
 * Do what needs to be done just before calling a potentially
 * blocking system call.
 */
static ici_exec_t *
potentially_block()
{
#ifndef NOSIGNALS
    ici_signals_blocking_syscall(1);
#endif
    return ici_leave();
}

static void
unblock(ici_exec_t *x)
{
#ifndef NOSIGNALS
    ici_signals_blocking_syscall(0);
#endif
    ici_enter(x);
}

/*
 * The functions in the 'net' module use strings to specify IP network
 * addresses.  Addresses may be expressed in one of the forms:
 *
 *  [host:]portnum
 *
 * or
 *
 *  portnum[@host]
 *
 * or
 *
 *  [host:]service[/protocol]
 *
 * or
 *
 *  service[/protocol][@host]
 *
 * where '[...]' are optional elements, and:
 *
 * portnum              Is an integer port number.
 *
 * service              Is a service name that will be looked up in the
 *                      services database.  (See '/etc/services' on UNIX-like
 *                      systems).
 *
 * protocol             Is either 'tcp' or 'udp'.
 *
 * host                 Is either a domain name, an IP address in dotted
 *                      decimal notation, "." for the local address, "?" for
 *                      any, or "*" for all.
 *
 *                      If the '@host' is omitted, the the default host
 *                      depends on context (see 'net.bind()' and 'net.connect()').
 *
 * This also forms part of the --ici-net-- intro.
 */
/*
 * Parse an IP address in the format described above.  The host part is
 * optional and if not specified defaults to the defhost parameter.  The
 * address may be NULL to just initialise the socket address to defhost port
 * 0.
 *
 * The sockaddr structure is filled in and 0 returned if all is okay.  When a
 * error occurs the error string is set and 1 is returned.
 */
static struct sockaddr_in *
parseaddr(char *raddr, long defhost, struct sockaddr_in *saddr)
{
    char addr[512];
    char *host;
    char *ports;
    short port;

    /*
     * Initialise address so if we fail to find a host or port
     * in the string passed in we don't have to deal with it.
     * The address is set to the default passed to us while the
     * port is set to zero which is used in bind() to have the
     * system allocate us a port.
     */
    saddr->sin_family = PF_INET;
    saddr->sin_addr.s_addr = htonl(defhost);
    saddr->sin_port = 0;
    port = 0;
    if (raddr == NULL)
        return saddr;

    if (strlen(raddr) > sizeof addr - 1)
    {
        seterror
        (
            "network address string too long",
            "network address string too long: \"%.32s\"",
            raddr
        );
        return NULL;
    }
    strcpy(addr, raddr);
    if ((host = strchr(addr, '@')) != NULL)
    {
        ports = addr;
        *host++ = '\0';
    }
    else if ((ports = strchr(addr, ':')) != NULL)
    {
        if (ports != addr)
        {
            host = addr;
            *ports++ = '\0';
        }
    }
    else
    {
        host = NULL;
        ports = addr;
    }
    if (host != NULL)
    {
        struct hostent *hostent;
        long            hostaddr;

        if (!strcmp(host, "."))
            hostaddr = htonl(INADDR_LOOPBACK);
        else if (!strcmp(host, "?"))
            hostaddr = htonl(INADDR_ANY);
        else if (!strcmp(host, "*"))
            hostaddr = htonl(INADDR_BROADCAST);
        else if ((hostaddr = inet_addr(host)) != (unsigned long)-1)
            /* NOTHING */ ;
        else if ((hostent = gethostbyname(host)) != NULL)
            memcpy(&hostaddr, hostent->h_addr, sizeof hostaddr);
        else
        {
            seterror("unknown host", "unknown host: \"%.32s\"", host);
            return NULL;
        }
        saddr->sin_addr.s_addr = hostaddr;
    }
    if (sscanf(ports, "%hu", &port) != 1)
    {
        char *proto;
        struct servent *servent;
        if ((proto = strchr(addr, '/')) != NULL)
            *proto++ = 0;
        if ((servent = getservbyname(ports, proto)) == NULL)
        {
            seterror("unknown service", "unknown service: \"%s\"", ports);
            return NULL;
        }
        port = ntohs(servent->s_port);
    }
    saddr->sin_port = htons(port);
    return saddr;
}

/*
 * Turn a port number and IP address into a nice looking string ;-)
 */
static char *
unparse_addr(addr)
struct sockaddr_in *addr;
{
    static char addr_buf[256];
#if 0
    struct servent *serv;
#endif
    struct hostent *host;

#if 0
    if ((serv = getservbyport(addr->sin_port, NULL)) != NULL)
        strcpy(addr_buf, serv->s_name);
    else
#endif
        sprintf(addr_buf, "%u", ntohs(addr->sin_port));
    strcat(addr_buf, "@");
    if (addr->sin_addr.s_addr == INADDR_ANY)
        strcat(addr_buf, "?");
    else if (addr->sin_addr.s_addr == INADDR_LOOPBACK)
        strcat(addr_buf, ".");
    else if (addr->sin_addr.s_addr == INADDR_BROADCAST)
        strcat(addr_buf, "*");
    else if
    (
        (
            host
            =
            gethostbyaddr
            (
                (char *)&addr->sin_addr.s_addr,
                sizeof addr->sin_addr,
                AF_INET
            )
        )
        ==
        NULL
    )
        strcat(addr_buf, inet_ntoa(addr->sin_addr));
    else
        strcat(addr_buf, host->h_name);
    return addr_buf;
}

/*
 * skt = net.socket([proto])
 *
 * Create a socket with a certain protocol (currently only TCP or UDP) and return
 * its descriptor.  Raises exception if the protocol is unknown or the socket
 * cannot be created.
 *
 * Where proto is one of the strings 'tcp', 'tcp/ip', 'udp' or 'udp/ip'.  The
 * default, if no argument is passed, is 'tcp'.  If proto is an int it is a
 * file descriptor for a socket and is a socket object is created with that
 * file descriptor.
 *
 * The '/ip' is the start of handling different protocol families (as
 * implemented in BSD and WINSOCK 2).  For compatibiliy with exisitng ICI
 * sockets code the default protocol family is defined to be 'ip'.
 *
 * Returns a socket object representing a communications end-point.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_socket()
{
    ici_handle_t        *skt;
    string_t            *proto;
    int                 type;
    SOCKET              fd;

    if (NARGS() == 0)
        proto = ICIS(tcp);
    else if (ici_typecheck("o", &proto))
    {
        long            i;
#ifdef ICI_PEDANTIC_SKTS
        struct stat     statbuf;
#endif

        if (ici_typecheck("i", &i))
            return 1;
#ifdef ICI_PEDANTIC_SKTS
        if (fstat(i, &statbuf) == -1)
        {
            error = strerror(errno);
            return 1;
        }
        if ((statbuf.st_mode & S_IFMT) != S_IFSOCK)
        {
            error = "not a socket";
            return 1;
        }
#endif
        return ici_ret_with_decref(objof(new_netsocket(i)));
    }
    if (proto == ICIS(tcp) || proto == ICIS(tcp_ip))
        type = SOCK_STREAM;
    else if (proto == ICIS(udp) || proto == ICIS(udp_ip))
        type = SOCK_DGRAM;
    else
    {
        return seterror
        (
            "unsupported protocol or address family",
            "unsupported protocol or address family: %s", proto->s_chars
        );
    }
    if ((fd = socket(PF_INET, type, 0)) == -1)
        return ici_get_last_errno("net.socket", NULL);
    if ((skt = new_netsocket(fd)) == NULL)
    {
        closesocket(fd);
        return 1;
    }
    return ici_ret_with_decref(objof(skt));
}

/*
 * net.close(socket)
 *
 * Close a socket.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_close()
{
    ici_handle_t *skt;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    closesocket((SOCKET)skt->h_ptr);
    objof(skt)->o_flags |= H_CLOSED;
    return ici_null_ret();
}

/*
 * skt = net.listen(skt [, backlog])
 *
 * Notify the sytem of willingness to accept connections on 'skt'.  This would
 * be done to a socket created with 'net.socket()' prior to using
 * 'net.accept()' to accept connections.  The 'backlog' parameter defines the
 * maximum length the queue of pending connections may grow to (default 5).
 * Returns the given 'skt'.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_listen()
{
    ici_handle_t *skt;
    long         backlog = 5;    /* ain't tradition grand */

    switch (NARGS())
    {
    case 1:
        if (ici_typecheck("h", ICIS(socket), &skt))
            return 1;
        break;

    case 2:
        if (ici_typecheck("hi", ICIS(socket), &skt, &backlog))
            return 1;
        break;

    default:
        return ici_argcount(2);
    }
    if (isclosed(skt))
        return 1;
    if (listen((SOCKET)skt->h_ptr, (int)backlog) == -1)
        return ici_get_last_errno("net.listen", NULL);
    return ici_ret_no_decref(objof(skt));
}

/*
 * new_skt = net.accept(skt)
 *
 * Wait for and accept a connection on the socket 'skt'.  Returns the
 * descriptor for a new socket connection.  The argument 'skt' is a socket
 * that has been created with 'net.socket()', bound to a local address with
 * 'net.bind()', and has been conditioned to listen for connections by a call
 * to 'net.listen()'.
 *
 * This may block, but will allow thread switching while blocked.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_accept()
{
    ici_handle_t  *skt;
    SOCKET        fd;
    exec_t        *x;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    x = potentially_block();
    fd = accept((SOCKET)skt->h_ptr, NULL, NULL);
    unblock(x);
    if (fd == -1)
    {
        return ici_get_last_errno("net.accept", NULL);
    }
    return ici_ret_with_decref(objof(new_netsocket(fd)));
}

/*
 * skt = net.connect(skt, address)
 *
 * Connect 'skt' to 'address'.  Returns the socket passed; which is now
 * connected to 'address'.  While connected, sent data will be directed to the
 * address, and only data received from the address will be accepted.  See the
 * introduction for a description of address formats.
 *
 * This may block, but will allow thread switching while blocked.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_connect()
{
    ici_handle_t        *skt;
    char                *addr;
    object_t            *arg;
    struct sockaddr_in  saddr;
    exec_t              *x;
    int                 rc;

    if (ici_typecheck("ho", ICIS(socket), &skt, &arg))
        return 1;
    if (isstring(arg))
        addr = stringof(arg)->s_chars;
    else if (isint(arg))
    {
        sprintf(ici_buf, "%ld", intof(arg)->i_value);
        addr = ici_buf;
    }
    else
        return ici_argerror(1);
    if (parseaddr(addr, INADDR_LOOPBACK, &saddr) == NULL)
        return 1;
    if (isclosed(skt))
        return 1;
    x = potentially_block();
    rc = connect((SOCKET)skt->h_ptr, (struct sockaddr *)&saddr, sizeof saddr);
    unblock(x);
    return rc == -1 ? ici_get_last_errno("net.connect", NULL) : ici_ret_no_decref(objof(skt));
}

/*
 * skt = net.bind(skt [, address])
 *
 * Bind the socket 'skt' to the local 'address' so that others may connect to
 * it.  The given socket is returned.
 *
 * If 'address' is not given or is 0, the system allocates a local address
 * (including port number).  In this case the port number allocated can be
 * recovered with 'net.getportno()'.
 *
 * If 'address' is given, it may be an integer, in which case it is
 * interpreted as a port number.  Otherwise it must be a string and will be
 * interpreted as described in the introduction.
 *
 * If 'address' is given, but has no host portion, "?" (any) is used.  Thus,
 * for example:
 *
 *  skt = bind(socket("tcp"), port);
 *
 * Will create a socket that accepts connections to the given port originating
 * from any network interface.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_bind()
{
    ici_handle_t        *skt;
    char                *addr;
    struct sockaddr_in  saddr;

    if (NARGS() == 2)
    {
        skt = handleof(ARG(0));
        if (!ishandleof(skt, ICIS(socket)))
            return ici_argerror(0);
        if (isstring(ARG(1)))
            addr = stringof(ARG(1))->s_chars;
        else if (isint(ARG(1)))
        {
            sprintf(ici_buf, "%ld", intof(ARG(1))->i_value);
            addr = ici_buf;
        }
        else
            return ici_argerror(1);
    }
    else
    {
        if (ici_typecheck("h", ICIS(socket), &skt))
            return 1;
        addr = "0";
    }
    if (parseaddr(addr, INADDR_ANY, &saddr) == NULL)
        return 1;
    if (isclosed(skt))
        return 1;
    if (bind((SOCKET)skt->h_ptr, (struct sockaddr *)&saddr, sizeof saddr) == -1)
        return ici_get_last_errno("net.bind", NULL);
    return ici_ret_no_decref(objof(skt));
}

/*
 * Helper function for f_select(). Adds a set of ready socket descriptors
 * to the struct object returned by f_select() by scanning the descriptors
 * actually selected and seeing if they were returned as being ready. If
 * so they are added to a set. Finally the set is added to the struct
 * object. Uses, and updates, the count of ready descriptors that is
 * returned by select(2) so we can avoid unneccesary work.
 */
#ifdef __GNUC__
__inline__
#endif
static int
select_add_result
(
    struct_t            *result,
    string_t            *key,
    set_t               *set,
    fd_set              *fds,
    int                 *n
)
{
    set_t       *rset;
    SOCKET      fd;
    int         i;
    slot_t      *sl;

    if ((rset = ici_set_new()) == NULL)
        return 1;
    if (set != NULL)
    {
        for (i = 0; *n > 0 && i < set->s_nslots; ++i)
        {
            if ((sl = (slot_t *)&set->s_slots[i])->sl_key == NULL)
                continue;
            if (!ishandleof(sl->sl_key, ICIS(socket)))
                continue;
            fd = (SOCKET)handleof(sl->sl_key)->h_ptr;
            if (FD_ISSET(fd, fds))
            {
                --*n;
                if (ici_assign(rset, handleof(sl->sl_key), ici_one))
                {
                    goto fail;
                }
            }
        }
    }
    if (ici_assign(result, key, rset))
        goto fail;
    ici_decref(rset);
    return 0;

fail:
    ici_decref(rset);
    return 1;
}

/*
 * struct = net.select([int,] set|NULL [, set|NULL [, set|NULL]])
 *
 * Checks sockets for I/O readiness with an optional timeout.  Select may be
 * passed up to three sets of sockets that are checked for readiness to
 * perform I/O.  The first set holds the sockets to test for input pending,
 * the second set the sockets to test for output able and the third set the
 * sockets to test for exceptional states.  NULL may be passed in place of a
 * set parameter to avoid passing empty sets.  An integer may also appear in
 * the parameter list.  This integer specifies the number of milliseconds to
 * wait for the sockets to become ready.  If a zero timeout is passed the
 * sockets are polled to test their state.  If no timeout is passed the call
 * blocks until at least one of the sockets is ready for I/O.
 *
 * The result of select is a struct containing three sets, of sockets,
 * identified by the keys 'read', 'write' and 'except'.
 *
 * This may block, but will allow thread switching while blocked.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
/*
 * Aldem: dtabsize now is computed (as max found FD). It is more efficient
 * than select() on ALL FDs.
 */
static int
ici_net_select()
{
    int                 i;
    int                 n;
    int                 dtabsize = -1;
    long                timeout  = -1;
    fd_set              fds[3];
    fd_set              *rfds = NULL;
    set_t               *rset = NULL;
    fd_set              *wfds = NULL;
    set_t               *wset = NULL;
    fd_set              *efds = NULL;
    set_t               *eset = NULL;
    struct timeval      timeval;
    struct timeval      *tv;
    struct_t            *result;
    set_t               *set  = NULL; /* Init. to remove compiler warning */
    int                 whichset = -1;  /* 0 == read, 1 == write, 2 == except*/
    slot_t              *sl;
    exec_t              *x;

    if (NARGS() == 0)
        return seterror("incorrect number of arguments for net.select()", NULL);
    for (i = 0; i < NARGS(); ++i)
    {
        if (isint(ARG(i)))
        {
            if (timeout != -1)
                return seterror("too many timeout parameters passed to net.select", NULL);
            timeout = intof(ARG(i))->i_value;
            if (timeout < 0)
                return seterror("-ve timeout passed to net.select", NULL);
        }
        else if (ici_isset(ARG(i)) || isnull(ARG(i)))
        {
            int j;

            if (++whichset > 2)
                return seterror("too many set/NULL params to select()", NULL);
            if (ici_isset(ARG(i)))
            {
                fd_set *fs = 0;

                switch (whichset)
                {
                case 0:
                    fs = rfds = &fds[0];
                    set = rset = setof(ARG(i));
                    break;
                case 1:
                    fs = wfds = &fds[1];
                    set = wset = setof(ARG(i));
                    break;
                case 2:
                    fs = efds = &fds[2];
                    set = eset = setof(ARG(i));
                    break;
                }
                FD_ZERO(fs);
                for (n = j = 0; j < set->s_nslots; ++j)
                {
                    int k;

                    if ((sl = (slot_t *)&set->s_slots[j])->sl_key == NULL)
                        continue;
                    if (!ishandleof(sl->sl_key, ICIS(socket)))
                        continue;
                    if (isclosed(handleof(sl->sl_key)))
                        return seterror("attempt to use a closed socket in net.select", NULL);
                    k = (SOCKET)handleof(sl->sl_key)->h_ptr;
                    FD_SET(k, fs);
                    if (k > dtabsize)
                        dtabsize = k;
                    ++n;
                }
                if (n == 0)
                {
                    switch (whichset)
                    {
                    case 0:
                        rfds = NULL;
                        rset = NULL;
                        break;
                    case 1:
                        wfds = NULL;
                        wset = NULL;
                        break;
                    case 2:
                        efds = NULL;
                        eset = NULL;
                        break;
                    }
                }
            }
        }
        else
        {
            return ici_argerror(i);
        }
    }
    if (rfds == NULL && wfds == NULL && efds == NULL)
        return seterror("nothing to select, all socket sets are empty", NULL);
    if (timeout == -1)
        tv = NULL;
    else
    {
        tv = &timeval;
        tv->tv_sec = timeout / 1000000;
        tv->tv_usec = (timeout % 1000000); /* * 1000000.0; */
    }
    x = potentially_block();
    n = select(dtabsize + 1, rfds, wfds, efds, tv);
    unblock(x);
    if (n < 0)
        return ici_get_last_errno("net.select", NULL);
    if ((result = ici_struct_new()) == NULL)
        return 1;
    /* Add in count */
    {
        int_t   *nobj;

        if ((nobj = ici_int_new(n)) == NULL)
            goto fail;
        if (ici_assign(result, ICIS(n), nobj))
        {
            ici_decref(nobj);
            goto fail;
        }
        ici_decref(nobj);
    }
    if (select_add_result(result, ICIS(read), rset, rfds, &n))
        goto fail;
    /* Simpler return, one set of ready sockets */
    if (NARGS() == 1 && ici_isset(ARG(0)))
    {
        object_t        *o;

        o = ici_fetch(result, ICIS(read));
        ici_incref(o);
        ici_decref(result);
        return ici_ret_with_decref(o);
    }
    if (select_add_result(result, ICIS(write), wset, wfds, &n))
        goto fail;
    if (select_add_result(result, ICIS(except), eset, efds, &n))
        goto fail;
    return ici_ret_with_decref(objof(result));

fail:
    ici_decref(objof(result));
    return 1;
}

/*
 * int = net.sendto(skt, msg, address)
 *
 * Send a 'msg' (a string) to a specific 'address'.  This may be used even if
 * 'skt' is not connected.  If the host portion of the address is missing the
 * local host address is used.
 *
 * Returns the count of the number of bytes transferred.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_sendto()
{
    char                *addr;
    string_t            *msg;
    int                 n;
    ici_handle_t        *skt;
    struct sockaddr_in  sockaddr;

    if (ici_typecheck("hos", ICIS(socket), &skt, &msg, &addr))
        return 1;
    if (!isstring(objof(msg)))
        return ici_argerror(1);
    if (parseaddr(addr, INADDR_LOOPBACK, &sockaddr) == NULL)
        return 1;
    if (isclosed(skt))
        return 1;
    n = sendto
    (
        (SOCKET)skt->h_ptr,
        msg->s_chars,
        msg->s_nchars,
        0,
        (struct sockaddr *)&sockaddr,
        sizeof sockaddr
    );
    if (n < 0)
        return ici_get_last_errno("net.sendto", NULL);
    return ici_int_ret(n);
}

#if 0
/*
 * Turn a textual send option into the correct bits
 */
static int
flagval(flag)
char *flag;
{
    if (!strcmp(flag, "oob"))
        return MSG_OOB;
    if (!strcmp(flag, "peek"))
        return MSG_PEEK;
    if (!strcmp(flag, "dontroute"))
        return MSG_DONTROUTE;
    return -1;
}
#endif

/*
 * struct = net.recvfrom(skt, int)
 *
 * Receive a message on the socket 'skt' and return a struct containing the
 * data of the message and the source address of the data.  The 'int'
 * parameter gives the maximum number of bytes to receive.  The result is a
 * struct with the keys 'msg' (a string) being the data data received and
 * 'addr' (a string) the address it was received from (in one of the @
 * forms described in the introduction).
 *
 * This may block, but will allow thread switching while blocked.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_recvfrom()
{
    ici_handle_t        *skt;
    int                 len;
    int                 nb;
    char                *msg;
    struct sockaddr_in  addr;
    int                 addrsz = sizeof addr;
    struct_t            *result;
    string_t            *s;
    exec_t              *x;

    if (ici_typecheck("hi", ICIS(socket), &skt, &len))
        return 1;
    if (isclosed(skt))
        return 1;
    if (len < 0)
        return seterror("negative transfer count", NULL);
    if ((msg = ici_nalloc(len + 1)) == NULL)
        return 1;
    x = potentially_block();
    nb = recvfrom((SOCKET)skt->h_ptr, msg, len, 0, (struct sockaddr *)&addr, &addrsz);
    unblock(x);
    if (nb == -1)
    {
        ici_nfree(msg, len + 1);
        return ici_get_last_errno("net.recvfrom", NULL);
    }
    if (nb == 0)
    {
        ici_nfree(msg, len + 1);
        return ici_null_ret();
    }
    if ((result = ici_struct_new()) == NULL)
    {
        ici_nfree(msg, len + 1);
        return 1;
    }
    if ((s = ici_str_new(msg, nb)) == NULL)
    {
        ici_nfree(msg, len + 1);
        return 1;
    }
    ici_nfree(msg, len + 1);
    msg = NULL;
    if (ici_assign(result, ICIS(msg), s))
    {
        ici_decref(s);
        goto fail;
    }
    ici_decref(s);
    if ((s = ici_str_new_nul_term(unparse_addr(&addr))) == NULL)
    {
        goto fail;
    }
    if (ici_assign(result, ICIS(addr), s))
    {
        ici_decref(s);
        goto fail;
    }
    ici_decref(s);
    return ici_ret_with_decref(objof(result));

fail:
    if (msg != NULL)
        ici_nfree(msg, len + 1);
    ici_decref(result);
    return 1;
}

/*
 * int = net.send(skt, string)
 *
 * Send the message 'string' on the socket 'skt'. The socket must
 * be connected.
 *
 * Returns the count of the number of bytes transferred.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_send()
{
    ici_handle_t *skt;
    int          len;
    string_t     *msg;

    if (ici_typecheck("ho", ICIS(socket), &skt, &msg))
        return 1;
    if (!isstring(objof(msg)))
        return ici_argerror(1);
    if (isclosed(skt))
        return 1;
    len = send((SOCKET)skt->h_ptr, msg->s_chars, msg->s_nchars, 0);
    if (len < 0)
        return ici_get_last_errno("net.send", NULL);
    return ici_int_ret(len);
}

/*
 * string = net.recv(skt, int)
 *
 * Receive data from a socket 'skt' and return it as a string.  The 'int'
 * parameter gives the maximum size of message that will be received.
 * The actual number of bytes transferred may be determined from the 
 * length of the returned string. If the connection is closed NULL is
 * returned.
 *
 * This may block, but will allow thread switching while blocked.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_recv()
{
    ici_handle_t *skt;
    int          len;
    int          nb;
    char         *msg;
    string_t     *s;
    exec_t       *x;

    if (ici_typecheck("hi", ICIS(socket), &skt, &len))
        return 1;
    if (isclosed(skt))
        return 1;
    if (len < 0)
        return seterror("negative transfer count", NULL);
    if ((msg = ici_nalloc(len + 1)) == NULL)
        return 1;
    x = potentially_block();
    nb = recv((SOCKET)skt->h_ptr, msg, len, 0);
    unblock(x);
    if (nb == -1)
    {
        ici_nfree(msg, len + 1);
        return ici_get_last_errno("net.recv", NULL);
    }
    if (nb == 0)
    {
        ici_nfree(msg, len + 1);
        return ici_null_ret();
    }
    if ((s = ici_str_new(msg, nb)) == NULL)
        return 1;
    ici_nfree(msg, len + 1);
    return ici_ret_with_decref(objof(s));
}


/*
 * sockopt - turn a socket option name into an option code and level.
 *
 * Parameters:
 *      opt             The socket option, a string.
 *      level           A pointer to somewhere to store the
 *                      level parameter for {get,set}sockopt.
 *
 * Returns:
 *      The option code or -1 if no matching option found.
 */
static int
sockopt(char *opt, int *level)
{
    int code;
    int i;

    static struct
    {
        char    *name;
        int     value;
        int     level;
    }
    opts[] =
    {
        {"debug",       SO_DEBUG,       SOL_SOCKET},
        {"reuseaddr",   SO_REUSEADDR,   SOL_SOCKET},
        {"keepalive",   SO_KEEPALIVE,   SOL_SOCKET},
        {"dontroute",   SO_DONTROUTE,   SOL_SOCKET},
#ifndef __linux__
        {"useloopback", SO_USELOOPBACK, SOL_SOCKET},
#endif
        {"linger",      SO_LINGER,      SOL_SOCKET},
        {"broadcast",   SO_BROADCAST,   SOL_SOCKET},
        {"oobinline",   SO_OOBINLINE,   SOL_SOCKET},
        {"sndbuf",      SO_SNDBUF,      SOL_SOCKET},
        {"rcvbuf",      SO_RCVBUF,      SOL_SOCKET},
        {"type",        SO_TYPE,        SOL_SOCKET},
        {"error",       SO_ERROR,       SOL_SOCKET},

        {"nodelay",     TCP_NODELAY,    IPPROTO_TCP}

    };

    for (code = -1, i = 0; i < nels(opts); ++i)
    {
        if (!strcmp(opt, opts[i].name))
        {
            code = opts[i].value;
            *level = opts[i].level;
            break;
        }
    }
    return code;
}

/*
 * int = net.getsockopt(skt, string)
 *
 * Retrieve the value of a socket option. A socket may have various
 * attributes associated with it. These are accessed via the 'getsockopt' and
 * 'setsockopt' functions.  The attributes are named with 'string' from
 * the following list:
 *
 *  "debug"
 *  "reuseaddr"
 *  "keepalive"
 *  "dontroute"
 *  "useloopback"   (Linux only)
 *  "linger"
 *  "broadcast"
 *  "oobinline"
 *  "sndbuf"
 *  "rcvbuf"
 *  "type"
 *  "error"
 *
 * All option values get returned as integers. The only special processing
 * is of the "linger" option. This gets returned as the lingering time if
 * it is set or -1 if lingering is not enabled.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_getsockopt()
{
    ici_handle_t        *skt;
    char                *opt;
    int                 o;
    char                *optval;
    int                 optlen;
    int                 optlevel;
    struct linger       linger;
    int                 intvar;

    optval = (char *)&intvar;
    optlen = sizeof intvar;
    if (ici_typecheck("hs", ICIS(socket), &skt, &opt))
        return 1;

    o = sockopt(opt, &optlevel);

    if (optlevel == SOL_SOCKET)
    {
        switch (o)
        {
        case SO_DEBUG:
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
        case SO_DONTROUTE:
        case SO_BROADCAST:
        case SO_TYPE:
        case SO_OOBINLINE:
        case SO_SNDBUF:
        case SO_RCVBUF:
        case SO_ERROR:
            break;

        case SO_LINGER:
            optval = (char *)&linger;
            optlen = sizeof linger;
            break;

        default:
            goto bad;
        }
    }
    else if (optlevel == IPPROTO_TCP)
    {
        switch (o)
        {
        case TCP_NODELAY:
            break;

        default:
            goto bad;
        }
    }
    else
    {
        /* Shouldn't happen - sockopt returned a bogus level */
#ifndef NDEBUG
        abort();
#endif
        return seterror("internal ici error in net.c:sockopt()", NULL);
    }

    if (isclosed(skt))
        return 1;
    if (getsockopt((SOCKET)skt->h_ptr, optlevel, o, optval, &optlen) == -1)
        return ici_get_last_errno("net.getsockopt", NULL);
    if (o == SO_LINGER)
        intvar = linger.l_onoff ? linger.l_linger : -1;
    else
    {
        switch (o)
        {
        case SO_TYPE:
        case SO_SNDBUF:
        case SO_RCVBUF:
        case SO_ERROR:
            break;
        default:
            intvar = !!intvar;
        }
    }
    return ici_int_ret(intvar);

bad:
    return seterror("bad socket option", "bad socket option \"%s\"", opt);
}

/*
 * setsockopt(skt, string [, int])
 *
 * Set socket a socket option named by 'string' to 'int' (default 1).
 * See 'net.getsockopt()' for a list of option names.
 *
 * All socket options are integers. Again linger is a special case. The
 * option value is the linger time, if zero or negative lingering is
 * turned off.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_setsockopt()
{
    ici_handle_t        *skt;
    char                *opt;
    int                 optcode;
    int                 optlevel;
    char                *optval;
    int                 optlen;
    int                 intvar;
    struct linger       linger;

    if (ici_typecheck("hs", ICIS(socket), &skt, &opt) == 0)
        intvar = 1; /* default to +ve action ... "set..." */
    else if (ici_typecheck("hsi", ICIS(socket), &skt, &opt, &intvar))
        return 1;
    optcode = sockopt(opt, &optlevel);
    optval = (char *)&intvar;
    optlen = sizeof intvar;
    if (optlevel == SOL_SOCKET)
    {
        switch (optcode)
        {
        case SO_DEBUG:
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
        case SO_DONTROUTE:
        case SO_BROADCAST:
        case SO_TYPE:
        case SO_OOBINLINE:
        case SO_SNDBUF:
        case SO_RCVBUF:
        case SO_ERROR:
            break;

        case SO_LINGER:
            linger.l_onoff = intvar > 0;
            linger.l_linger = intvar;
            optval = (char *)&linger;
            optlen = sizeof linger;
            break;

        default:
            goto bad;
        }
    }
    else if (optlevel == IPPROTO_TCP)
    {
        switch (optcode)
        {
        case TCP_NODELAY:
            break;

        default:
            goto bad;
        }
    }
    else
    {
        /* Shouldn't happen - sockopt returned a bogus level */
#ifndef NDEBUG
        abort();
#endif
        return seterror("internal ici error in net.c:sockopt()", NULL);
    }

    if (isclosed(skt))
        return 1;
    if (setsockopt((SOCKET)skt->h_ptr, optlevel, optcode, optval, optlen) == -1)
        return ici_get_last_errno("net.setsockopt", NULL);
    return ici_ret_no_decref(objof(skt));

bad:
    return seterror("bad socket option", "bad socket option \"%s\"", opt);
}

/*
 * string = net.hostname()
 *
 * Return the name of the current host.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_hostname()
{
    static string_t     *hostname = NULL;

    if (hostname == NULL)
    {
        char name_buf[MAXHOSTNAMELEN];
        if (gethostname(name_buf, sizeof name_buf) == -1)
            return ici_get_last_errno("net.gethostname", NULL);
        if ((hostname = ici_str_new_nul_term(name_buf)) == NULL)
            return 1;
        ici_incref(hostname);
    }
    return ici_ret_no_decref((object_t *)stringof(hostname));
}

#if 0
/*
 * Return the name of the current user or the user with the given uid.
 */
static
ici_net_username()
{
    char        *s;
#ifdef _WINDOWS
    char        buffer[64];     /* I hope this is long enough! */
    int         len;

    len = sizeof buffer;
    if (!GetUserName(buffer, &len))
        strcpy(buffer, "Windows User");
    s = buffer;
#else   /* #ifdef _WINDOWS */
    /*
     * Do a password file lookup under Unix
     */
    char                *getenv();
    struct passwd       *pwent;
    long                uid = getuid();

    if (NARGS() > 0)
    {
        if (ici_typecheck("i", &uid))
            return 1;
    }
    if ((pwent = getpwuid(uid)) == NULL)
    {
        sprintf(buf, "can't find name for uid %ld", uid);
        error = buf;
        return 1;
    }
    s = pwent->pw_name;
#endif
    return ici_str_ret(s);
}
#endif


/*
 * string = net.getpeername(skt)
 *
 * Return the address of the peer of a TCP socket. That is, the
 * name of the thing it is connected to.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_getpeername()
{
    struct sockaddr_in  addr;
    int                 len = sizeof addr;
    ici_handle_t        *skt;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    if (getpeername((SOCKET)skt->h_ptr, (struct sockaddr *)&addr, &len) == -1)
        return ici_get_last_errno("net.getpeername", NULL);
    return ici_str_ret(unparse_addr(&addr));
}

/*
 * string = net.getsockname(skt)
 *
 * Return the local address of a socket.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_getsockname()
{
    struct sockaddr_in  addr;
    int                 len = sizeof addr;
    ici_handle_t        *skt;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    if (getsockname((SOCKET)skt->h_ptr, (struct sockaddr *)&addr, &len) == -1)
        return ici_get_last_errno("net.getsockname", NULL);
    return ici_str_ret(unparse_addr(&addr));
}

/*
 * int = net.getportno(skt)
 *
 * Return the local port number bound to a TCP or UDP socket.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_getportno()
{
    struct sockaddr_in  addr;
    int                 len = sizeof addr;
    ici_handle_t        *skt;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    if (getsockname((SOCKET)skt->h_ptr, (struct sockaddr *)&addr, &len) == -1)
        return ici_get_last_errno("net.getsockname", NULL);
    return ici_int_ret(ntohs(addr.sin_port));
}

/*
 * string = net.gethostbyname(string)
 *
 * Return the IP address for the specified host. The address is returned
 * as a string containing the dotted decimal form of the host's address.
 * If the host's address cannot be resolved an error, "no such host"
 * is raised.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_gethostbyname()
{
    char                *name;
    struct hostent      *hostent;
    struct in_addr      addr;

    if (ici_typecheck("s", &name))
        return 1;
    if ((hostent = gethostbyname(name)) == NULL)
        return seterror("no such host", "no such host: \"%.32s\"", name);
    memcpy(&addr, *hostent->h_addr_list, sizeof addr);
    return ici_str_ret(inet_ntoa(addr));
}

/*
 * string = net.gethostbyaddr(int|string)
 *
 * Return the name of a host given an IP address. The IP address is
 * specified as either a string containing an address in dotted
 * decimal or an integer containing the IP address in host byte
 * order (remember ICI ints are at least 32 bits so they can store
 * a 32 bit IP address).
 *
 * The name is returned as a string. If the name cannot be resolved
 * an exception, "unknown host", is raised.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
ici_net_gethostbyaddr()
{
    long                addr;
    char                *s;
    struct hostent      *hostent;

    if (NARGS() != 1)
        return ici_argcount(1);
    if (isint(ARG(0)))
        addr = htonl((unsigned long)intof(ARG(0))->i_value);
    else if (ici_typecheck("s", &s))
        return 1;
    else if ((addr = inet_addr(s)) == 0xFFFFFFFF)
        return seterror("invalid IP address", "invalid IP address: %32s", s);
    if ((hostent = gethostbyaddr((char *)&addr, sizeof addr, AF_INET)) == NULL)
        return seterror("unknown host", s != NULL ? "unknown host: %32s" : "unkown host", s);
    return ici_str_ret((char *)hostent->h_name);
}

/*
 * int = net.sktno(skt)
 *
 * Return the underlying OS file descriptor associated with a socket.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_sktno()
{
    ici_handle_t *skt;

    if (ici_typecheck("h", ICIS(socket), &skt))
        return 1;
    if (isclosed(skt))
        return 1;
    return ici_int_ret((long)(SOCKET)skt->h_ptr);
}

/*
 * For turning sockets into files we implement a complete ICI ftype_t.
 * This is done because, (a) it works for all platforms and, (b) we
 * can control when the socket is closed (we don't close the underlying
 * socket when the file object is closed as occurs if fdopen() is used).
 */

enum
{
    SF_BUFSIZ   = 2048, /* Read/write buffer size */
    SF_READ     = 1,    /* Open for reading */
    SF_WRITE    = 2,    /* Open for writing */
    SF_EOF      = 4     /* EOF read */
};

typedef struct
{
    ici_handle_t *sf_socket;
    char         sf_buf[SF_BUFSIZ];
    char         *sf_bufp;
    int          sf_nbuf;
    int         sf_pbchar;
    int         sf_flags;
}
skt_file_t;

static int
skt_getch(skt_file_t *sf)
{
    char        c;

    if (!(sf->sf_flags & SF_READ) || (sf->sf_flags & SF_EOF))
        return EOF;
    if (sf->sf_pbchar != EOF)
    {
        c = sf->sf_pbchar;
        sf->sf_pbchar = EOF;
    }
    else
    {
        if (sf->sf_nbuf == 0)
        {
            ici_exec_t *x = potentially_block();
            sf->sf_nbuf = recv((SOCKET)sf->sf_socket->h_ptr, sf->sf_buf, SF_BUFSIZ, 0);
            unblock(x);
            if (sf->sf_nbuf <= 0)
            {
                sf->sf_flags |= SF_EOF;
                return EOF;
            }
            sf->sf_bufp = sf->sf_buf;
        }
        c = *sf->sf_bufp++;
        --sf->sf_nbuf;
    }
    return (unsigned char)c;
}

static int
skt_ungetc(int c, skt_file_t *sf)
{
    if (!(sf->sf_flags & SF_READ))
        return EOF;
    if (sf->sf_pbchar != EOF)
        return EOF;
    sf->sf_pbchar = c;
    return 0;
}

static int
skt_flush(skt_file_t *sf)
{
    if (sf->sf_flags & SF_WRITE && sf->sf_nbuf > 0)
    {
        int     rc;
        ici_exec_t *x = potentially_block();
        rc = send((SOCKET)sf->sf_socket->h_ptr, sf->sf_buf, sf->sf_nbuf, 0);
        unblock(x);
        if (rc != sf->sf_nbuf)
            return 1;
    }
    else /* sf->sf_flags & SF_READ */
    {
        sf->sf_pbchar = EOF;
    }
    sf->sf_bufp = sf->sf_buf;
    sf->sf_nbuf = 0;
    return 0;
}

static int
skt_putch(int c, skt_file_t *sf)
{
    char    ch = c;

    if (!(sf->sf_flags & SF_WRITE))
        return EOF;
    if (sf->sf_nbuf == SF_BUFSIZ)
    {
        if (skt_flush(sf))
            return EOF;
    }
    *sf->sf_bufp++ = c;
    ++sf->sf_nbuf;
    return 0;
}

static int
skt_fclose(skt_file_t *sf)
{
    int         rc = 0;

    if (sf->sf_flags & SF_WRITE)
        rc = skt_flush(sf);
    ici_decref(sf->sf_socket);
    ici_tfree(sf, skt_file_t);
    return rc;
}

static long
skt_seek(void)
{
    ici_error = "cannot seek on a socket";
    return -1;
}

static int
skt_eof(skt_file_t *sf)
{
    return sf->sf_flags & SF_EOF;
}

static int
skt_write(char *buf, int n, skt_file_t *sf)
{
    int         nb;
    int         rc;

    if (!(sf->sf_flags & SF_WRITE))
        return EOF;
    for (rc = nb = 0; n > 0; n -= nb, rc += nb)
    {
        if (sf->sf_nbuf == SF_BUFSIZ && skt_flush(sf))
            return EOF;
        if ((nb = n) > (SF_BUFSIZ - sf->sf_nbuf))
            nb = SF_BUFSIZ - sf->sf_nbuf;
        memcpy(sf->sf_bufp, buf, nb);
        sf->sf_bufp += nb;
        sf->sf_nbuf += nb;
        buf += nb;
    }
    return rc;
}

static ftype_t  net_skt_ftype =
{
    0,
    skt_getch,
    skt_ungetc,
    skt_putch,
    skt_flush,
    skt_fclose,
    skt_seek,
    skt_eof,
    skt_write
};

static skt_file_t *
skt_open(ici_handle_t *s, char *mode)
{
    skt_file_t  *sf;

    if ((sf = ici_talloc(skt_file_t)) != NULL)
    {
        sf->sf_socket = s;
        ici_incref(sf->sf_socket);
        sf->sf_pbchar = EOF;
        sf->sf_bufp = sf->sf_buf;
        sf->sf_nbuf = 0;
        switch (*mode)
        {
        case 'r':
            sf->sf_flags = SF_READ;
            break;
        case 'w':
            sf->sf_flags = SF_WRITE;
            break;
        default:
            seterror("bad open mode for socket", "bad open mode, \"%s\", for socket", mode);
            return NULL;
        }
    }
    return sf;
}

/*
 * Note that ICI 'socket' objects can not be used directly where
 * an ICI file object is required (for example, in such functions
 * as 'printf' and 'getchar'), but a file that refers to the socket
 * can be obtained by calling 'net.sktopen()' (see below).
 *
 * This --intro-- forms part of the --ici-net-- documentation.
 */

/*
 * file = net.sktopen(skt [, mode])
 *
 * Open a socket as a file, for input or output according to mode (see
 * 'fopen').
 *
 * Note that closing this file does not close the underlying socket.
 * Also note that no "text" mode is supported, even on Win32 systems.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_sktopen()
{
    ici_handle_t        *skt;
    char                *mode;
    file_t              *f;
    skt_file_t          *sf;

    if (ici_typecheck("hs", ICIS(socket), &skt, &mode))
    {
        if (ici_typecheck("h", ICIS(socket), &skt))
            return 1;
        mode = "r";
    }
    if (isclosed(skt))
        return 1;
    if ((sf = skt_open(skt, mode)) == NULL)
        return 1;
    if ((f = ici_file_new((char *)sf, &net_skt_ftype, NULL, NULL)) == NULL)
    {
        skt_fclose(sf);
        return 1;
    }
    return ici_ret_with_decref(objof(f));
}

#ifndef USE_WINSOCK
/*
 * array = net.socketpair()
 *
 * Returns an array containing a pair of connected sockets.
 *
 * This function is not currently available on Win32 platforms.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_socketpair()
{
    array_t             *a;
    ici_handle_t        *s;
    int                 sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1)
        return ici_get_last_errno("net.socketpair", NULL);
    if ((a = ici_array_new(2)) == NULL)
        goto fail1;
    if ((s = new_netsocket(sv[0])) == NULL)
    {
        ici_decref(a);
        goto fail1;
    }
    *a->a_top++ = objof(s);
    ici_decref(s);
    if ((s = new_netsocket(sv[1])) == NULL)
    {
        close(sv[1]);
        ici_decref(a);
        goto fail;
    }
    *a->a_top++ = objof(s);
    ici_decref(s);
    return ici_ret_with_decref(objof(a));

fail1:
    close(sv[0]);
    close(sv[1]);
fail:
    return 1;
}
#endif  /* #ifndef USE_WINSOCK */

/*
 * socket = net.shutdown(socket [, int ])
 *
 * Shutdown the send or receive or both sides of a TCP connection.  The
 * optional int specifies which direction to shut down, 0 for send, 1 for
 * receive and 2 for both.  The default is 2.  Returns the same socket it is
 * given.
 *
 * This --topic-- forms part of the --ici-net-- documentation.
 */
static int
ici_net_shutdown()
{
    ici_handle_t    *skt;
    long            flags;

    switch (NARGS())
    {
    case 1:
        if (ici_typecheck("h", ICIS(socket), &skt))
            return 1;
        flags = 2;
        break;

    case 2:
        if (ici_typecheck("hi", ICIS(socket), &skt, &flags))
            return 1;
        break;

    default:
        return ici_argcount(2);
    }
    shutdown((SOCKET)skt->h_ptr, (int)flags);
    return ici_ret_no_decref(objof(skt));
}

static cfunc_t ici_net_cfuncs[] =
{
    {CF_OBJ, "socket", ici_net_socket},
    {CF_OBJ, "close", ici_net_close},
    {CF_OBJ, "listen", ici_net_listen},
    {CF_OBJ, "accept", ici_net_accept},
    {CF_OBJ, "connect", ici_net_connect},
    {CF_OBJ, "bind", ici_net_bind},
    {CF_OBJ, "select", ici_net_select},
    {CF_OBJ, "sendto", ici_net_sendto},
    {CF_OBJ, "recvfrom", ici_net_recvfrom},
    {CF_OBJ, "send", ici_net_send},
    {CF_OBJ, "recv", ici_net_recv},
    {CF_OBJ, "getsockopt", ici_net_getsockopt},
    {CF_OBJ, "setsockopt", ici_net_setsockopt},
    {CF_OBJ, "hostname", ici_net_hostname},
    {CF_OBJ, "getpeername", ici_net_getpeername},
    {CF_OBJ, "getsockname", ici_net_getsockname},
    {CF_OBJ, "getportno", ici_net_getportno},
    {CF_OBJ, "gethostbyname", ici_net_gethostbyname},
    {CF_OBJ, "gethostbyaddr", ici_net_gethostbyaddr},
    {CF_OBJ, "sktno", ici_net_sktno},
    {CF_OBJ, "sktopen", ici_net_sktopen},
    {CF_OBJ, "shutdown", ici_net_shutdown},
#ifndef USE_WINSOCK
    {CF_OBJ, "socketpair", ici_net_socketpair},
#endif
    {CF_OBJ}
};

object_t *
ici_net_library_init(void)
{
#ifdef  USE_WINSOCK
    {
        WSADATA             wsadata;

        if (WSAStartup(MAKEWORD(1, 1), &wsadata))
        {
            /*
             * Can't use GetLastError() on WSAStartup() failure.
             */
            ici_error = "failed to initialise Windows socket";
            return NULL;
        }
    }
#endif
    if (ici_interface_check(ICI_VER, ICI_BACK_COMPAT_VER, "net"))
        return NULL;
    if (init_ici_str())
        return NULL;
    return objof(ici_module_new(ici_net_cfuncs));
}

