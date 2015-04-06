#include "chandler.h"
#include "ulog.h"
#include "qap.h"

/* FIXME: add support for big-endian machines */
#define itop(X) (X)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

static const char *scr_socket = "/data/rcloud/run/Rscripts";

static int recvn(int s, char *buf, int len) {
    int i = 0;
    while (len) {
        int n = recv(s, buf + i, len, 0);
        if (n == 0) return i;
        if (n < 0) return n; /* FIXME: handle EINTR ? */
        i += n;
        len -= n;
    }
    return i;
}

static char *strapp(char *x, char *y) {
    if (y) {
        size_t l = strlen(y);
        memcpy(x, y, l);
        x += l;
    }
    return x;
}

typedef struct par_info {
    int type, attr_type;
    unsigned long len, attr_len;
    const char *attr, *payload, *next;
} par_info_t;

/* parse one parameter - check consistency of lengths involved,
   doesn't check alignment */
static int parse_par(par_info_t *pi, const char *buf, const char *eob) {
    const unsigned int *i = (const unsigned int*) buf;
    pi->type = 0;
    pi->len = 0;
    if (eob - buf < 4) return -1; /* incomplete (nothing is valid) */
    pi->type = PAR_TYPE(itop(*i));
    pi->len  = PAR_LEN(itop(*(i++)));
    pi->attr_type = 0;
    pi->attr_len = 0;
    pi->attr = 0;
    if (pi->type & XT_LARGE) {
        if (eob - buf < 8)
            return -1;
        pi->len |= ((unsigned long) *(i++)) << 24;
    }
    pi->payload = (const char*) i;
    pi->next = pi->payload + pi->len;
    ulog("PAR [%d, %d bytes] %d left", pi->type, (int) pi->len, (int) (eob - pi->payload));
    if ((eob - (const char*) i) < pi->len)
        return -2; /* truncated payload (type/len is valid) */
    
    /* parse the attribute and check length consistencies */
    if (pi->type & XT_HAS_ATTR) {
        if (pi->len < 4) return -3; /* inconsistent (attr doesn't fit) */
        pi->attr_type = PAR_TYPE(*i);
        pi->attr_len = PAR_LEN(*(i++));
        if (pi->attr_type & XT_HAS_ATTR)
            return -4; /* invalid recursive attribute */
        if (pi->attr_type & XT_LARGE) {
            if (pi->attr_len < 8) return -3;
            pi->attr_len |= ((unsigned long) *(i++)) << 24;
        }
        if (pi->len < (pi->attr_len + ((pi->attr_type & XT_LARGE) ? 8 : 4)))
            return -3;
        pi->attr = (const char*) i;
        pi->payload = pi->attr + pi->attr_len;
        pi->attr_type &= 0x3F; /* strip LARGE/HAS_ATTR */
    }
    pi->type &= 0x3F; /* strip LARGE/HAS_ATTR */
    return 0;
}

/* >= 0 (string length) if the parameter has string, -1 otherwise */
static long string_par_len(par_info_t *pi) {
    char *c;
    if (pi->type != XT_ARRAY_STR && pi->len < 1)
        return -1;
    c = memchr(pi->payload, 0, pi->len);
    return (c) ? (c - pi->payload) : -1;
}

int R_script_handler(http_request_t *req, http_result_t *res, const char *path) {
    int l = strlen(path), s;

    /* we only serve .R scripts */
    if (l < 2 || strcmp(path + l - 2, ".R")) return 0;

    ulog("INFO: serving R script '%s'", path);
    
    s = socket(AF_LOCAL, SOCK_STREAM, 0);

    { /* connect to script services QAP socket */
        struct sockaddr_un sau;
        struct phdr hdr;
        char *oci;
        int n;
        memset(&sau, 0, sizeof(sau));
        sau.sun_family = AF_LOCAL;
        strcpy(sau.sun_path, scr_socket);
        if (s == -1 || connect(s, (struct sockaddr*)&sau, sizeof(sau))) {
            ulog("ERROR: failed to connect to script socket '%s': %s", scr_socket, strerror(errno));
            res->err = strdup("cannot connect to R services");
            res->code = 500;
            return 1;
        }
        if ((n = recvn(s, (char*) &hdr, sizeof(hdr))) != sizeof(hdr)) {
            ulog("ERROR: cannot read ID string/header (n = %d, errno: %s)", n, strerror(errno));
            res->err = strdup("cannot read ID string/header from R services");
            res->code = 500;
            close(s);
            return 1;
        }
        hdr.cmd = itop(hdr.cmd);
        hdr.len = itop(hdr.len);

        if (hdr.cmd != CMD_OCinit) {
            ulog("ERROR: server did not respond with RsOC message - wrong protocol?");
            res->err = strdup("R services are not running in OCAP mode");
            res->code = 500;
            close(s);
            return 1;
        }
        if (hdr.res || hdr.len > 0x7fffff || hdr.len < 32) {
            ulog("ERROR: initial message doesn't have expected length (got %d bytes)", hdr.len);
            res->err = strdup("R services responded with invalid large message");
            res->code = 500;
            close(s);
            return 1;
        }
        
        oci = (char*) malloc(hdr.len + 128);
        if (!oci) {
            ulog("ERROR: out of memory when allocating buffer for RsOC message");
            res->err = strdup("out of memory");
            res->code = 500;
            close(s);
            return 1;
        }
        if ((n = recvn(s, oci, hdr.len)) != hdr.len) {
            free(oci);
            ulog("ERROR: read error in RsOC payload (n = %d, errno: %s)", n, strerror(errno));
            res->err = strdup("cannot read ID string/header from R services");
            res->code = 500;
            close(s);
            return 1;
        }

        {
            char qq[4096], *q = qq;
            int i;
            for (i = 0; i < hdr.len; i++) q += snprintf(q, 8, " %02x", (int) ((unsigned char*)oci)[i]);
            ulog(qq);
        }

        /* parse RsOC */
        {
            unsigned int *hp = (unsigned int*) oci;
            if (PAR_TYPE(itop(*hp)) == DT_SEXP) {
                hp++;
                if (PAR_TYPE(itop(*hp)) == (XT_ARRAY_STR | XT_HAS_ATTR)) {
                    unsigned int ocl = PAR_LEN(itop(*hp));
                    /* check length sanity */
                    if (ocl <= hdr.len - 8) {
                        /* simple packing: url, query, headers, body
                           all but body may not contain \0 so they are
                           separated by \0, body is the remainder */
                        unsigned long l = 0, tpl;
                        char *outp, *oc;
                        qap_hdr_t *oh;
                        unsigned int *oi;

                        if (req->url) l += strlen(req->url);
                        if (req->query) l += strlen(req->query);
                        if (req->headers) l += strlen(req->headers);
                        if (req->body_len) l += req->body_len;
                        l += 3; /* 3 separating \0s */
                        
                        /* FIXME: support large packets */
                        if (l > 0xffff80) {
                            free(oci);  
                            ulog("ERROR: large packages are curretnly unsupported (needed to store %lu bytes)", l);
                            res->err = strdup("sorry, large packets are currently unsupported");
                            res->code = 500;
                            close(s);
                            return 1;
                        }

                        tpl = l + ocl + 36; /* DT_SEXP; XT_LANG_NOTAG; OCAP; XT_RAW; raw-len + 16-byte hdr */
                        tpl = (tpl + 3) & (~3); /* align */
                        ulog("l = %lu, tpl = %lu", l, tpl);
                        outp = (char*) malloc(tpl);
                        if (!outp) {
                            ulog("ERROR: out of memory when allocating output buffer (%lu bytes)", tpl);
                            res->err = strdup("out of memory");
                            res->code = 500;
                            close(s);
                            return 1;
                        }
                        oh = (qap_hdr_t*) outp;
                        oi = (unsigned int*) (outp + sizeof(qap_hdr_t));
                        oh->cmd = itop(CMD_OCcall);
                        oh->len = itop((unsigned int) (tpl - sizeof(qap_hdr_t)));
                        oh->res = 0;
                        oh->msg_id = hdr.msg_id;
                        tpl -= sizeof(qap_hdr_t) + 4; /* hdr - DT_SXP header */
                        *(oi++) = itop(SET_PAR(DT_SEXP, tpl));
                        tpl -= 4;
                        *(oi++) = itop(SET_PAR(XT_LANG_NOTAG, tpl));
                        tpl -= 4;
                        memcpy(oi, hp, ocl + 4);
                        tpl -= ocl + 4;
                        oi += (ocl + 4) / 4; /* Note: we don't check alignment */
                        *(oi++) = itop(SET_PAR(XT_RAW, (l + 7) & 0xfffffffc));
                        *(oi++) = itop(l);
                        oc = (char*) oi;
                        ulog("l will be stored at %ld", (long int) (oc - outp));
                        oc = strapp(oc, req->url);
                        *(oc++) = 0;
                        oc = strapp(oc, req->query);
                        *(oc++) = 0;
                        oc = strapp(oc, req->headers);
                        *(oc++) = 0;
                        if (req->body_len) memcpy(oc, req->body, req->body_len);
                        ulog("INFO: sending %d bytes (n = %d)", itop(oh->len) + sizeof(qap_hdr_t),
                             send(s, outp, itop(oh->len) + sizeof(qap_hdr_t), 0));

#if 0
                        {
                            char qq[4096], *q = qq;
                            int i;
                            for (i = 0; i < ((tpl > 256) ? 256 : tpl); i++)
                                q += snprintf(q, 8, " %02x", (int) ((unsigned char*)outp)[i]);
                            ulog(qq);
                        }
#endif

                        free(outp);
                        /* ok, now we just wait for the response ... */

                        if ((n = recvn(s, (char*) &hdr, sizeof(hdr))) != sizeof(hdr)) {
                            free(oci);
                            ulog("ERROR: read error on OCcall response header (n = %d, errno: %s)", n, strerror(errno));
                            res->err = strdup("R aborted on the request");
                            res->code = 500;
                            close(s);
                            return 1;
                        }
                        hdr.cmd = itop(hdr.cmd);
                        hdr.len = itop(hdr.len);

                        ulog("INFO: OCcall response 0x%08x (%d bytes)", hdr.cmd, hdr.len);
                        
                        free(oci);
                        oci = (char*) malloc(hdr.len + 128);
                        if (!oci) {
                            ulog("ERROR: out of memory when allocating buffer for OCcall response (%u bytes)", hdr.len + 128);
                            res->err = strdup("out of memory");
                            res->code = 500;
                            close(s);
                            return 1;
                        }
                        if ((n = recvn(s, oci, hdr.len)) != hdr.len) {
                            free(oci);
                            ulog("ERROR: read error in OCCall response payload (n = %d, errno: %s)", n, strerror(errno));
                            res->err = strdup("incomplete response from R");
                            res->code = 500;
                            close(s);
                            return 1;
                        }

                        {
                            char qq[4096], *q = qq;
                            int i;
                            for (i = 0; i < hdr.len; i++) q += snprintf(q, 8, " %02x", (int) ((unsigned char*)oci)[i]);
                            ulog(qq);
                        }

                        /* expect: DT_SEXP -> XT_VECTOR -> XT_ARRAY_STR ... */
                        {
                            par_info_t pi;
                            const char *eoci = oci + hdr.len, *cp = oci;
                            int peo;
                            
                            if ((peo = parse_par(&pi, cp, eoci)) || pi.type != DT_SEXP || pi.len < 4) {
                                free(oci);
                                ulog("ERROR: invalid payload (at DT, parse error %d, type = %, length = %d)",
                                     peo, pi.type, pi.len);
                                res->err = strdup("invalid response payload from R");
                                res->code = 500;
                                close(s);
                                return 1;
                            }
                            cp = pi.payload;
                            if ((peo = parse_par(&pi, cp, eoci))) {
                                free(oci);
                                ulog("ERROR: invalid payload (at SEXP, parse error %d)",peo);
                                res->err = strdup("invalid response payload from R");
                                res->code = 500;
                                close(s);
                                return 1;
                            }
                            cp = pi.payload;
                            eoci = cp + pi.len;
                            res->payload = 0;
                            if (pi.type == XT_VECTOR) {
                                /* FIXME: add support for named vectors that serve files */
                                long strl;
                                if (!(peo = parse_par(&pi, cp, eoci)) &&
                                    (strl = string_par_len(&pi)) >= 0) {
                                    res->payload = strdup(pi.payload);
                                    res->payload_len = strl;
                                    ulog("INFO: text body (%ld bytes)", strl);
                                    cp = pi.next;
                                    if (cp < eoci && !(peo = parse_par(&pi, cp, eoci)) &&
                                        (strl = string_par_len(&pi)) >= 0) {
                                        res->content_type = strdup(pi.payload);
                                        ulog("INFO: content-type: '%s'", res->content_type);
                                        cp = pi.next;
                                        if (cp < eoci && !(peo = parse_par(&pi, cp, eoci)) &&
                                            (strl = string_par_len(&pi)) >= 0) {
                                            res->headers = strdup(pi.payload);
                                            ulog("INFO: headers: '%s'", res->headers);
                                        } /* headers */
                                    } /* content-type */
                                    free(oci);
                                    close(s);
                                    return 1;
                                } /* body */
                                free(oci);
                                ulog("ERROR: invalid payload (missing body in the result list, parse error %d)", peo);
                                res->err = strdup("missing body in response list from R");
                                res->code = 500;
                                close(s);
                                return 1;
                            } else if (pi.type == XT_ARRAY_STR && string_par_len(&pi) >= 0) {
                                res->err = strdup(pi.payload);
                                free(oci);
                                res->code = 500;
                                close(s);
                                return 1;
                            } else {
                                free(oci);
                                ulog("ERROR: invalid payload (expected list[16]/string[34], got %d with %d bytes)",
                                     pi.type, pi.len);
                                res->err = strdup("invalid response from R - neither a vector or character result");
                                res->code = 500;
                                close(s);
                                return 1;
                            }
                        }
                    }

                    close(s);
                }
            }
        }                
            
        close(s);
        free(oci);
    }

    res->err = strdup("not implemented yet");
    res->payload = 0;
    res->payload_len = 0;
    res->code = 500;

    return 1;
}