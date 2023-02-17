#define _GNU_SOURCE
#include "dns.h"
#include "net.h"
#include "log.h"
#include "opt.h"
#include "misc.h"
#include <stddef.h>
#include <string.h>
#include <netinet/in.h>

/* "\3www\6google\3com\0" => "www.google.com" */
static bool decode_name(char *restrict out, const char *restrict src, ssize_t len) {
    /* root domain ? */
    if (len <= DNS_NAME_ENC_MINLEN) {
        out[0] = '.';
        out[1] = '\0';
        return true;
    }

    /* ignore last byte: src="\3www\6google\3com" */
    /* ignore first byte: out="www\6google\3com\0" */
    memcpy(out, src + 1, --len);

    /* foreach label (len:1byte | label) */
    for (int first = 1; len >= 2;) {
        if (first) first = 0; else *out++ = '.';
        uchar label_len = *(const uchar *)src++; --len;
        unlikely_if (label_len < 1) {
            LOGE("label length is too short: %u", label_len);
            return false;
        }
        unlikely_if (label_len > DNS_DNAME_LABEL_MAXLEN) {
            LOGE("label length is too long: %u", label_len);
            return false;
        }
        unlikely_if (label_len > len) {
            LOGE("label length is greater than remaining length: %u > %zd", label_len, len);
            return false;
        }
        src += label_len;
        len -= label_len;
        out += label_len;
    }

    unlikely_if (len != 0) {
        LOGE("name format error, remaining length: %zd", len);
        return false;
    }

    return true;
}

/* check dns packet */
static bool check_packet(bool is_query,
    const void *restrict packet_buf, ssize_t packet_len,
    char *restrict name_buf, size_t *restrict p_namelen)
{
    /* check packet length */
    unlikely_if (packet_len < (ssize_t)DNS_PACKET_MINSIZE) {
        LOGE("dns packet is too short: %zd", packet_len);
        return false;
    }
    unlikely_if (packet_len > DNS_PACKET_MAXSIZE) {
        LOGE("dns packet is too long: %zd", packet_len);
        return false;
    }

    /* check header */
    const dns_header_t *header = packet_buf;
    unlikely_if (header->qr != (is_query ? DNS_QR_QUERY : DNS_QR_REPLY)) {
        LOGE("this is a %s packet, but header->qr != %d", is_query ? "query" : "reply", is_query ? DNS_QR_QUERY : DNS_QR_REPLY);
        return false;
    }
    unlikely_if (header->opcode != DNS_OPCODE_QUERY) {
        LOGE("this is not a standard query, opcode: %u", (unsigned)header->opcode);
        return false;
    }
    unlikely_if (ntohs(header->question_count) != 1) {
        LOGE("there should be one and only one question section: %u", (unsigned)ntohs(header->question_count));
        return false;
    }

    /* move to question section (dname + dns_query_t) */
    packet_buf += sizeof(dns_header_t);
    packet_len -= sizeof(dns_header_t);

    /* search the queried domain name */
    /* encoded name: "\3www\6google\3com\0" */
    const void *p = memchr(packet_buf, 0, (size_t)packet_len);
    unlikely_if (!p) {
        LOGE("format error: domain name end byte not found");
        return false;
    }

    /* check name length */
    const size_t namelen = p + 1 - packet_buf;
    unlikely_if (namelen < DNS_NAME_ENC_MINLEN) {
        LOGE("encoded domain name is too short: %zu", namelen);
        return false;
    }
    unlikely_if (namelen > DNS_NAME_ENC_MAXLEN) {
        LOGE("encoded domain name is too long: %zu", namelen);
        return false;
    }

    /* decode to ASCII format */
    if (name_buf) {
        unlikely_if (!decode_name(name_buf, packet_buf, namelen))
            return false;
    }
    if (p_namelen)
        *p_namelen = namelen;

    /* move to dns_query_t pos */
    packet_buf += namelen;
    packet_len -= namelen;

    /* check remaining length */
    unlikely_if (packet_len < (ssize_t)sizeof(dns_query_t)) {
        LOGE("remaining length is less than sizeof(dns_query_t): %zd < %zu", packet_len, sizeof(dns_query_t));
        return false;
    }

    /* check query class */
    const dns_query_t *query_ptr = packet_buf;
    unlikely_if (ntohs(query_ptr->qclass) != DNS_CLASS_INTERNET) {
        LOGE("only supports standard internet query class: %u", (unsigned)ntohs(query_ptr->qclass));
        return false;
    }

    return true;
}

/*          \0 => root domain */
/*      \2cn\0 => normal domain */
/*     [ptr:2] => fully compress */
/* \2cn[ptr:2] => partial compress */
static bool skip_name(const void *restrict *restrict p_ptr, ssize_t *restrict p_len) {
    const void *restrict ptr = *p_ptr;
    ssize_t len = *p_len;

    while (len > 0) {
        uchar label_len = *(const uchar *)ptr;
        if (label_len == 0) {
            ++ptr;
            --len;
            break;
        } else if (label_len >= DNS_DNAME_COMPRESSION_MINVAL) {
            ptr += 2;
            len -= 2;
            break;
        } else unlikely_if (label_len > DNS_DNAME_LABEL_MAXLEN) {
            LOGE("label length is too long: %u", label_len);
            return false;
        } else { /* normal label */
            ptr += 1 + label_len;
            len -= 1 + label_len;
        }
    }

    unlikely_if (len < (ssize_t)sizeof(dns_record_t)) {
        LOGE("remaining length is less than sizeof(dns_record_t): %zd < %zu", len, sizeof(dns_record_t));
        return false;
    }

    *p_ptr = ptr;
    *p_len = len;
    return true;
}

/* check if the answer ip is in the chnroute ipset (check qtype before call) */
int dns_chnip_check(const void *restrict packet_buf, ssize_t packet_len, size_t namelen) {
    const dns_header_t *h = packet_buf;
    uint16_t answer_count = ntohs(h->answer_count);

    /* move to answer section */
    packet_buf += sizeof(dns_header_t) + namelen + sizeof(dns_query_t);
    packet_len -= sizeof(dns_header_t) + namelen + sizeof(dns_query_t);

    /* find the first A/AAAA record */
    for (uint16_t i = 0; i < answer_count; ++i) {
        unlikely_if (!skip_name(&packet_buf, &packet_len))
            return DNS_IPCHK_BAD_PACKET;

        const dns_record_t *record = packet_buf;
        unlikely_if (ntohs(record->rclass) != DNS_CLASS_INTERNET) {
            LOGE("only supports standard internet query class: %u", (unsigned)ntohs(record->rclass));
            return DNS_IPCHK_BAD_PACKET;
        }

        uint16_t rdatalen = ntohs(record->rdatalen);
        size_t recordlen = sizeof(dns_record_t) + rdatalen;
        unlikely_if (packet_len < (ssize_t)recordlen) {
            LOGE("remaining length is less than sizeof(record): %zd < %zu", packet_len, recordlen);
            return DNS_IPCHK_BAD_PACKET;
        }

        switch (ntohs(record->rtype)) {
            case DNS_RECORD_TYPE_A:
                unlikely_if (rdatalen != IPV4_BINADDR_LEN) {
                    LOGE("rdatalen is not equal to sizeof(ipv4): %u != %d", (unsigned)rdatalen, IPV4_BINADDR_LEN);
                    return DNS_IPCHK_BAD_PACKET;
                }
                return ipset_addr_is_exists(record->rdata, true) ? DNS_IPCHK_IS_CHNIP : DNS_IPCHK_NOT_CHNIP; /* in chnroute ? */
            case DNS_RECORD_TYPE_AAAA:
                unlikely_if (rdatalen != IPV6_BINADDR_LEN) {
                    LOGE("rdatalen is not equal to sizeof(ipv6): %u != %d", (unsigned)rdatalen, IPV6_BINADDR_LEN);
                    return DNS_IPCHK_BAD_PACKET;
                }
                return ipset_addr_is_exists(record->rdata, false) ? DNS_IPCHK_IS_CHNIP : DNS_IPCHK_NOT_CHNIP; /* in chnroute6 ? */
        }

        packet_buf += recordlen;
        packet_len -= recordlen;
    }

    /* not found A/AAAA record */
    return DNS_IPCHK_NOT_FOUND;
}

/* check dns query, `name_buf` used to get domain name, return true if valid */
bool dns_query_check(const void *restrict packet_buf, ssize_t packet_len, char *restrict name_buf, size_t *restrict p_namelen) {
    return check_packet(true, packet_buf, packet_len, name_buf, p_namelen);
}

/* check dns reply, `name_buf` used to get domain name, return true if accept */
bool dns_reply_check(const void *restrict packet_buf, ssize_t packet_len, char *restrict name_buf, size_t *restrict p_namelen) {
    return check_packet(false, packet_buf, packet_len, name_buf, p_namelen);
}