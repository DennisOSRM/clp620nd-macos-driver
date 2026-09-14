/*
 * clp620 - CUPS backend wrapping "socket", to undo a firmware bug.
 *
 * The CLP-620ND's SNMP agent reports an empty input tray as a paper jam.
 * hrPrinterDetectedErrorState comes back as 04 00, which is bit 5, "jammed".
 * The bit it means is 13, "inputTrayEmpty", whose encoding is 00 04: the two
 * octets of the bitmap are written the wrong way round.  Out of paper entirely
 * is 40 04, noPaper plus inputTrayEmpty, and arrives as 04 40 - jammed plus
 * outputTrayMissing - so neither real bit survives.  The device's prtAlertTable is
 * correct at the same moment - it reports code 808, inputMediaSupplyEmpty, and
 * no jam - so the two disagree and only the bitmap is wrong.
 *
 * CUPS believes the bitmap, maps bit 5 to media-jam-warning, and the queue
 * shows a paper jam.  Because an empty multi-purpose tray is the normal resting
 * state of this printer, the phantom jam is permanent.
 *
 * So: exec the stock socket backend, pass everything through untouched, and
 * drop media-jam-warning from its STATE lines only when prtAlertTable confirms
 * there is no jam.  A real jam still reports.  Supply levels, page counts and
 * every other state reason are passed through, which is the point of doing this
 * rather than switching SNMP off.
 *
 * The same poll also forwards prtConsoleDisplayBufferText as a CUPS INFO line,
 * so the queue shows what the panel shows, in the panel's own language, rather
 * than a generic idle state; it is only re-sent when the text changes.  And it
 * reads prtInputCurrentLevel, which answers -3 for a tray that holds paper
 * without counting sheets and 0 for an empty one.  That names the real cause in
 * the debug line, and drives the media-empty state below, but it never decides
 * whether there is a jam: prtAlertTable stays authoritative for that.
 *
 * Device URI: clp620://host[:port]  ->  socket://host[:port]
 *
 * SNMPv1 is spoken directly over UDP here.  Linking net-snmp would add a
 * dependency whose ABI moves between macOS releases, and it wants MIB files and
 * config the backend sandbox need not grant; three short GETNEXT walks on a
 * five second cadence are not worth it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>

/* Both overridable, so the test suite can substitute stubs. */
#ifndef SOCKET_BACKEND
#define SOCKET_BACKEND "/usr/libexec/cups/backend/socket"
#endif

#ifndef SNMP_PORT
#define SNMP_PORT "161"
#endif
#define JAM_REASON     "media-jam-warning"

/* prtAlertCode and prtAlertDescription. */
static const unsigned long OID_ALERT_CODE[] = { 1,3,6,1,2,1,43,18,1,1,7 };
static const unsigned long OID_ALERT_DESC[] = { 1,3,6,1,2,1,43,18,1,1,8 };

/*
 * prtConsoleDisplayBufferText: the front panel's own text, one row per display
 * line, already in whatever language the panel is set to.  Worth forwarding
 * because it says what the machine says - "Sparbetrieb..." rather than a
 * generic idle state.
 */
static const unsigned long OID_CONSOLE[] = { 1,3,6,1,2,1,43,16,5,1,2 };

/*
 * prtInputCurrentLevel, one row per tray.  0 is empty; -3 means the tray holds
 * paper but does not count sheets, which is what a loaded tray answers here.
 * An empty multi-purpose tray is this printer's normal resting state, and is
 * the condition the firmware miscodes as a jam.
 */
static const unsigned long OID_INPUT_LEVEL[] = { 1,3,6,1,2,1,43,8,2,1,10 };

static pid_t Child = 0;

static void fwd_signal(int sig)
{
    if (Child > 0) kill(Child, sig);
}

/* ---------------------------------------------------------------- BER ---- */

static size_t enc_oid(unsigned char *dst, const unsigned long *oid, size_t n)
{
    size_t o = 0;
    dst[o++] = (unsigned char)(oid[0] * 40 + oid[1]);
    for (size_t i = 2; i < n; i++) {
        unsigned long v = oid[i];
        unsigned char tmp[8];
        int k = 0;
        do { tmp[k++] = (unsigned char)(v & 0x7f); v >>= 7; } while (v);
        while (k--) dst[o++] = (unsigned char)(tmp[k] | (k ? 0x80 : 0));
    }
    return o;
}

/*
 * Read one TLV.  Returns the start of the next TLV, or NULL past the end.
 * Long-form lengths matter here: prtAlertDescription runs to a few hundred
 * bytes on this firmware.
 */
static const unsigned char *tlv(const unsigned char *p, const unsigned char *end,
                                unsigned *tag, const unsigned char **val, size_t *vlen)
{
    if (!p || end - p < 2) return NULL;
    *tag = *p++;
    size_t len = *p++;
    if (len & 0x80) {
        size_t nb = len & 0x7f;
        if (nb == 0 || nb > 4 || (size_t)(end - p) < nb) return NULL;
        len = 0;
        while (nb--) len = (len << 8) | *p++;
    }
    if ((size_t)(end - p) < len) return NULL;
    *val = p; *vlen = len;
    return p + len;
}

static const unsigned char *descend(const unsigned char *p, const unsigned char *end,
                                    unsigned want, const unsigned char **cend)
{
    unsigned tag; const unsigned char *v; size_t n;
    if (!tlv(p, end, &tag, &v, &n) || tag != want) return NULL;
    *cend = v + n;
    return v;
}

/* -------------------------------------------------------------- SNMP ---- */

struct snmp {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char community[128];
};

static int snmp_open(struct snmp *s, const char *host, const char *community)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, SNMP_PORT, &hints, &res) || !res) return -1;

    s->fd = socket(res->ai_family, SOCK_DGRAM, 0);
    if (s->fd < 0) { freeaddrinfo(res); return -1; }
    memcpy(&s->addr, res->ai_addr, res->ai_addrlen);
    s->addrlen = res->ai_addrlen;
    freeaddrinfo(res);

    struct timeval tv = { 2, 0 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    snprintf(s->community, sizeof s->community, "%s", community);
    return 0;
}

/*
 * One GETNEXT.  Fills oid/noid with the returned OID and val/vlen with the
 * value TLV body, and sets *vtag.  Returns 0 on success.
 * The request is small enough that short-form lengths always suffice.
 */
static int snmp_getnext(struct snmp *s, const unsigned long *oid, size_t noid,
                        unsigned long *roid, size_t *nroid,
                        unsigned *vtag, const unsigned char **val, size_t *vlen,
                        unsigned char *rbuf, size_t rbufsz)
{
    unsigned char oidbuf[128];
    size_t oidlen = enc_oid(oidbuf, oid, noid);
    size_t clen = strlen(s->community);
    static unsigned reqid = 1;
    reqid++;

    unsigned char pkt[512], *p = pkt;
    size_t vb_inner  = 2 + oidlen + 2;                 /* OID + NULL */
    size_t vb_list   = 2 + vb_inner;
    size_t pdu_inner = 6 + 3 + 3 + 2 + vb_list;        /* id, err, idx, vblist */
    size_t msg_inner = 3 + (2 + clen) + 2 + pdu_inner;

    *p++ = 0x30; *p++ = (unsigned char)msg_inner;
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x00;             /* version 1 (v1 == 0) */
    *p++ = 0x04; *p++ = (unsigned char)clen;
    memcpy(p, s->community, clen); p += clen;
    *p++ = 0xA1; *p++ = (unsigned char)pdu_inner;      /* GetNextRequest */
    *p++ = 0x02; *p++ = 0x04;
    *p++ = (unsigned char)(reqid >> 24); *p++ = (unsigned char)(reqid >> 16);
    *p++ = (unsigned char)(reqid >> 8);  *p++ = (unsigned char)reqid;
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x00;             /* error-status */
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x00;             /* error-index  */
    *p++ = 0x30; *p++ = (unsigned char)vb_list;
    *p++ = 0x30; *p++ = (unsigned char)vb_inner;
    *p++ = 0x06; *p++ = (unsigned char)oidlen;
    memcpy(p, oidbuf, oidlen); p += oidlen;
    *p++ = 0x05; *p++ = 0x00;                          /* NULL */

    if (sendto(s->fd, pkt, (size_t)(p - pkt), 0,
               (struct sockaddr *)&s->addr, s->addrlen) < 0)
        return -1;

    ssize_t n = recv(s->fd, rbuf, rbufsz, 0);
    if (n <= 0) return -1;
    const unsigned char *end = rbuf + n, *q, *cend;

    if (!(q = descend(rbuf, end, 0x30, &cend))) return -1;
    unsigned tag; const unsigned char *v; size_t vn;
    if (!(q = tlv(q, cend, &tag, &v, &vn))) return -1;   /* version   */
    if (!(q = tlv(q, cend, &tag, &v, &vn))) return -1;   /* community */
    if (!(q = descend(q, cend, 0xA2, &cend))) return -1; /* GetResponse */
    if (!(q = tlv(q, cend, &tag, &v, &vn))) return -1;   /* request-id */
    if (!(q = tlv(q, cend, &tag, &v, &vn))) return -1;   /* error-status */
    if (vn == 1 && v[0] != 0) return -1;
    if (!(q = tlv(q, cend, &tag, &v, &vn))) return -1;   /* error-index */
    if (!(q = descend(q, cend, 0x30, &cend))) return -1; /* varbind list */
    if (!(q = descend(q, cend, 0x30, &cend))) return -1; /* varbind */
    if (!(q = tlv(q, cend, &tag, &v, &vn)) || tag != 0x06) return -1;

    /* decode the returned OID */
    size_t k = 0;
    if (vn < 1) return -1;
    roid[k++] = v[0] / 40; roid[k++] = v[0] % 40;
    unsigned long acc = 0;
    for (size_t i = 1; i < vn && k < *nroid; i++) {
        acc = (acc << 7) | (v[i] & 0x7f);
        if (!(v[i] & 0x80)) { roid[k++] = acc; acc = 0; }
    }
    *nroid = k;

    if (!tlv(q, cend, vtag, val, vlen)) return -1;
    return 0;
}

static int oid_prefix(const unsigned long *base, size_t nbase,
                      const unsigned long *o, size_t no)
{
    if (no < nbase) return 0;
    return memcmp(base, o, nbase * sizeof *base) == 0;
}

/*
 * Is the printer actually jammed?  Walk prtAlertCode for the IANA jam code (8)
 * and prtAlertDescription for the word "jam", so a vendor that only fills in
 * the text is still caught.
 *   1 = a real jam, 0 = no jam, -1 = could not tell.
 */
static int real_jam(struct snmp *s)
{
    int result = 0, answered = 0;
    unsigned char rbuf[2048];

    for (int pass = 0; pass < 2; pass++) {
        const unsigned long *base = pass ? OID_ALERT_DESC : OID_ALERT_CODE;
        size_t nbase = pass ? sizeof OID_ALERT_DESC / sizeof *OID_ALERT_DESC
                            : sizeof OID_ALERT_CODE / sizeof *OID_ALERT_CODE;
        unsigned long cur[64];
        size_t ncur = nbase;
        memcpy(cur, base, nbase * sizeof *cur);

        for (int step = 0; step < 64; step++) {
            unsigned long roid[64]; size_t nroid = 64;
            unsigned tag; const unsigned char *val; size_t vlen;
            if (snmp_getnext(s, cur, ncur, roid, &nroid, &tag, &val, &vlen,
                             rbuf, sizeof rbuf) < 0)
                break;
            if (!oid_prefix(base, nbase, roid, nroid)) break;
            answered = 1;

            if (!pass && tag == 0x02) {
                long v = 0;
                for (size_t i = 0; i < vlen; i++) v = (v << 8) | val[i];
                if (v == 8) result = 1;                 /* IANA jam(8) */
            } else if (pass && tag == 0x04) {
                for (size_t i = 0; i + 2 < vlen; i++)
                    if (tolower(val[i]) == 'j' && tolower(val[i+1]) == 'a' &&
                        tolower(val[i+2]) == 'm') { result = 1; break; }
            }
            if (result) break;
            memcpy(cur, roid, nroid * sizeof *cur);
            ncur = nroid;
        }
        if (result) break;
    }

    return answered ? result : -1;
}

/*
 * Copy the front panel's text into buf.  The table holds one row per display
 * line and the firmware pads every line to the panel width, so rows are
 * trimmed and joined with a single space.  Control bytes are dropped rather
 * than forwarded: this string goes into a CUPS INFO line, and a stray newline
 * there would be read as the start of another directive.
 * Returns 1 if anything readable came back.
 */
static int console_text(struct snmp *s, char *buf, size_t buflen)
{
    unsigned char rbuf[2048];
    unsigned long cur[64];
    size_t nbase = sizeof OID_CONSOLE / sizeof *OID_CONSOLE, ncur = nbase;
    size_t o = 0;
    int answered = 0;

    memcpy(cur, OID_CONSOLE, nbase * sizeof *cur);
    buf[0] = '\0';

    for (int step = 0; step < 8; step++) {
        unsigned long roid[64]; size_t nroid = 64;
        unsigned tag; const unsigned char *val; size_t vlen;
        if (snmp_getnext(s, cur, ncur, roid, &nroid, &tag, &val, &vlen,
                         rbuf, sizeof rbuf) < 0)
            break;
        if (!oid_prefix(OID_CONSOLE, nbase, roid, nroid)) break;
        answered = 1;
        if (tag == 0x04) {
            size_t n = vlen;
            while (n && (val[n-1] == ' ' || val[n-1] == '\t')) n--;
            for (size_t i = 0; i < n && o + 2 < buflen; i++) {
                unsigned char c = val[i];
                if (c < 0x20 || c == 0x7f) continue;
                if (c == ' ' && (o == 0 || buf[o-1] == ' ')) continue;
                buf[o++] = (char)c;
            }
            if (o && o + 2 < buflen && buf[o-1] != ' ') buf[o++] = ' ';
        }
        memcpy(cur, roid, nroid * sizeof *cur);
        ncur = nroid;
    }
    while (o && buf[o-1] == ' ') o--;
    buf[o] = '\0';
    return answered && o > 0;
}

/*
 * Is some input tray empty?  This is corroboration, not the verdict: it says
 * whether the condition the firmware miscodes as a jam is actually present, so
 * the debug line can name the real cause.  prtAlertTable stays authoritative
 * for whether there is a jam, because a tray that cannot report its level
 * (-3, which is Tray 1 here always) must not be read as "not empty".
 *   1 = at least one tray reads empty, 0 = none does, -1 = could not tell.
 */
static int tray_empty(struct snmp *s, int *all_empty)
{
    unsigned char rbuf[2048];
    unsigned long cur[64];
    size_t nbase = sizeof OID_INPUT_LEVEL / sizeof *OID_INPUT_LEVEL, ncur = nbase;
    int answered = 0, empty = 0, trays = 0, empties = 0;

    memcpy(cur, OID_INPUT_LEVEL, nbase * sizeof *cur);
    *all_empty = 0;

    for (int step = 0; step < 16; step++) {
        unsigned long roid[64]; size_t nroid = 64;
        unsigned tag; const unsigned char *val; size_t vlen;
        if (snmp_getnext(s, cur, ncur, roid, &nroid, &tag, &val, &vlen,
                         rbuf, sizeof rbuf) < 0)
            break;
        if (!oid_prefix(OID_INPUT_LEVEL, nbase, roid, nroid)) break;
        answered = 1;
        if (tag == 0x02 && vlen) {
            long v = (val[0] & 0x80) ? -1 : 0;          /* sign-extend */
            for (size_t i = 0; i < vlen; i++) v = (v << 8) | val[i];
            trays++;
            if (v == 0) { empty = 1; empties++; }
        }
        memcpy(cur, roid, nroid * sizeof *cur);
        ncur = nroid;
    }
    if (trays && empties == trays) *all_empty = 1;
    return answered ? empty : -1;
}

struct printer_state {
    int  jam;           /* 1 a real jam, 0 none, -1 could not tell   */
    int  empty;         /* 1 a tray reads empty, 0 none, -1 unknown  */
    int  all_empty;     /* 1 every tray that answered reads empty    */
    char panel[160];    /* front-panel text, empty if unavailable    */
};

/* Everything the backend wants to know, over one socket rather than three. */
static void probe_printer(const char *host, const char *community,
                          struct printer_state *st)
{
    struct snmp s;

    st->jam = st->empty = -1;
    st->all_empty = 0;
    st->panel[0] = '\0';

    if (snmp_open(&s, host, community) < 0) return;
    st->jam   = real_jam(&s);
    st->empty = tray_empty(&s, &st->all_empty);
    console_text(&s, st->panel, sizeof st->panel);
    close(s.fd);
}

/* -------------------------------------------------------------- main ---- */

/* Read the community string CUPS uses, so we match the socket backend. */
static void read_community(char *buf, size_t len)
{
    snprintf(buf, len, "public");
    FILE *fp = fopen("/etc/cups/snmp.conf", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "Community", 9) || !isspace((unsigned char)p[9])) continue;
        p += 9;
        while (isspace((unsigned char)*p)) p++;
        char *e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) e--;
        *e = '\0';
        if (*p) snprintf(buf, len, "%s", p);
    }
    fclose(fp);
}

/* Strip one reason from a STATE: line. Returns 1 if the line was rewritten. */
static int filter_state(char *line)
{
    if (strncmp(line, "STATE:", 6)) return 0;
    char *hit = strstr(line, JAM_REASON);
    if (!hit) return 0;

    /* splice the token out, along with one adjacent comma */
    char *after = hit + strlen(JAM_REASON);
    if (*after == ',') after++;
    else if (hit > line && hit[-1] == ',') hit--;
    memmove(hit, after, strlen(after) + 1);

    /* if nothing is left, actively clear the reason instead */
    char *p = line + 6;
    while (*p == ' ' || *p == '\t' || *p == '+' || *p == '-') p++;
    if (!*p || *p == '\n')
        strcpy(line, "STATE: -" JAM_REASON "\n");
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc == 1)
        return 0;                       /* no device discovery of our own */

    if (argc < 6 || argc > 7) {
        fputs("ERROR: clp620: usage: clp620 job user title copies options [file]\n", stderr);
        return 1;                       /* CUPS_BACKEND_FAILED */
    }

    const char *uri = getenv("DEVICE_URI");
    if (!uri) uri = argv[0];
    if (strncmp(uri, "clp620://", 9)) {
        fprintf(stderr, "ERROR: clp620: device URI %s is not clp620://\n", uri);
        return 1;
    }

    /* clp620://host:port -> socket://host:port */
    char sock_uri[1024], host[256];
    snprintf(sock_uri, sizeof sock_uri, "socket://%s", uri + 9);
    snprintf(host, sizeof host, "%s", uri + 9);
    if (host[0] == '[') {                       /* [::1]:9100 -> ::1 */
        memmove(host, host + 1, strlen(host));
        char *close = strchr(host, ']');
        if (close) *close = '\0';
    } else {
        char *cut = strpbrk(host, ":/?");
        if (cut) *cut = '\0';
    }

    char community[128];
    read_community(community, sizeof community);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        fprintf(stderr, "ERROR: clp620: pipe failed: %s\n", strerror(errno));
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fwd_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if ((Child = fork()) < 0) {
        fprintf(stderr, "ERROR: clp620: fork failed: %s\n", strerror(errno));
        return 1;
    }

    if (Child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        setenv("DEVICE_URI", sock_uri, 1);
        argv[0] = sock_uri;             /* cupsBackendDeviceURI() falls back to this */
        execv(SOCKET_BACKEND, argv);
        fprintf(stderr, "ERROR: clp620: cannot exec %s: %s\n",
                SOCKET_BACKEND, strerror(errno));
        _exit(1);
    }

    close(pipefd[1]);
    FILE *in = fdopen(pipefd[0], "r");
    char line[8192];
    time_t checked = 0;
    struct printer_state st = { -1, -1, 0, { 0 } };
    char shown[sizeof st.panel];
    const char *empty_shown = NULL;

    shown[0] = '\0';

    while (in && fgets(line, sizeof line, in)) {
        int jamline = !strncmp(line, "STATE:", 6) && strstr(line, JAM_REASON);
        time_t now = time(NULL);

        /*
         * One poll answers both questions.  It runs when a jam state needs
         * deciding and nothing is cached, and otherwise on the same five
         * second cadence the jam check always used, so the panel text keeps
         * up on a job that never reports a jam at all.  STATE lines repeat
         * far faster than that, which is what the cache is for.
         */
        if ((jamline && st.jam < 0) || now - checked > 5) {
            probe_printer(host, community, &st);
            checked = now;
            if (st.panel[0] && strcmp(st.panel, shown)) {
                snprintf(shown, sizeof shown, "%s", st.panel);
                fprintf(stderr, "INFO: %s\n", st.panel);
            }

            /*
             * Report the empty tray, which nothing else will.  The firmware
             * writes hrPrinterDetectedErrorState with its two octets the wrong
             * way round, so "out of paper" - noPaper in bit 1 and inputTrayEmpty
             * in bit 13, which is 40 04 - goes out as 04 40 and arrives as
             * jammed plus outputTrayMissing.  This backend then correctly drops
             * the jam, and the queue is left saying nothing is wrong while the
             * printer sits there with no paper in it.
             *
             * So say it here.  The tray level is the evidence: a tray with paper
             * answers -3, meaning it holds some but does not count sheets, and
             * only an empty one answers 0.  Every tray empty means printing has
             * actually stopped, which is an error; one empty tray among several
             * is a warning, because the job can still feed from another.
             */
            if (st.empty >= 0) {
                const char *want = !st.empty      ? NULL
                                 : st.all_empty   ? "media-empty-error"
                                                  : "media-empty-warning";
                if (want != empty_shown) {
                    if (empty_shown)
                        fprintf(stderr, "STATE: -%s\n", empty_shown);
                    if (want)
                        fprintf(stderr, "STATE: +%s\n", want);
                    empty_shown = want;
                }
            }
        }

        if (jamline) {
            if (st.jam == 0) {
                char before[8192];
                snprintf(before, sizeof before, "%s", line);
                if (filter_state(line)) {
                    char *nl = strchr(before, '\n'); if (nl) *nl = '\0';
                    fprintf(stderr, "DEBUG: clp620: suppressed \"%s\" - "
                            "prtAlertTable reports no jam%s\n", before,
                            st.empty == 1 ? ", and an input tray reads empty"
                                          : "");
                }
            } else if (st.jam < 0) {
                fputs("DEBUG: clp620: SNMP did not answer, passing the jam "
                      "state through unfiltered\n", stderr);
            }
        }
        fputs(line, stderr);
        fflush(stderr);
    }
    if (in) fclose(in);

    int status = 0;
    while (waitpid(Child, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 1;
    return 0;
}
