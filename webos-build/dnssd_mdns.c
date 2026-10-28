/*
 * Standalone mDNS service registration for RPiPlay on webOS
 * Replaces the dns_sd.h / Avahi dependency with raw multicast DNS
 *
 * Implements just enough of the Bonjour API that RPiPlay's dnssd.c interface uses:
 * - DNSServiceRegister / DNSServiceRefDeallocate
 * - TXTRecordCreate / SetValue / GetLength / GetBytesPtr / Deallocate
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#include "dnssdint.h"
#include "dnssd.h"
#include "global.h"
#include "compat.h"
#include "utils.h"

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define MAX_DEVICEID 18
#define MAX_SERVNAME 256
#define MAX_MDNS_PKT 1500
#define MAX_TXT_SIZE 512
#define ANNOUNCE_INTERVAL 20

/* --- TXT record implementation --- */

typedef struct {
    uint8_t data[MAX_TXT_SIZE];
    uint16_t len;
} txt_record_t;

/* --- mDNS service handle --- */

typedef struct _DNSServiceRef_t {
    int sock;
    pthread_t thread;
    volatile int running;
    char *name;
    char *regtype;
    uint16_t port;
    uint8_t *txt_data;
    uint16_t txt_len;
    struct sockaddr_in mdns_addr;
} *DNSServiceRef;

/* --- DNS packet helpers --- */

static int dns_write_name(uint8_t *buf, int pos, const char *name) {
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len > 63) len = 63;
        buf[pos++] = (uint8_t)len;
        memcpy(buf + pos, p, len);
        pos += len;
        p += len;
        if (dot) p++;
    }
    buf[pos++] = 0;
    return pos;
}

static int dns_write_u16(uint8_t *buf, int pos, uint16_t val) {
    buf[pos++] = (val >> 8) & 0xFF;
    buf[pos++] = val & 0xFF;
    return pos;
}

static int dns_write_u32(uint8_t *buf, int pos, uint32_t val) {
    buf[pos++] = (val >> 24) & 0xFF;
    buf[pos++] = (val >> 16) & 0xFF;
    buf[pos++] = (val >> 8) & 0xFF;
    buf[pos++] = val & 0xFF;
    return pos;
}

static uint32_t get_local_ip(void) {
    struct ifaddrs *ifa, *p;
    uint32_t addr = 0;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            uint32_t a = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
            if ((a >> 24) == 127) continue;
            addr = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
            break;
        }
        freeifaddrs(ifa);
    }
    return addr;
}

static char *get_local_hostname(void) {
    static char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "webos-tv");
    /* Ensure it ends with .local */
    if (!strstr(hostname, ".local")) {
        char *dot = strchr(hostname, '.');
        if (dot) *dot = '\0';
        strcat(hostname, ".local");
    }
    return hostname;
}

/*
 * Build an mDNS announcement packet for a service.
 * Contains:
 * - PTR record: _type._tcp.local -> instance._type._tcp.local
 * - SRV record: instance._type._tcp.local -> host:port
 * - TXT record: instance._type._tcp.local -> txt data
 * - A record: host.local -> IP address
 */
static int build_announcement(uint8_t *pkt, DNSServiceRef ref) {
    int pos = 0;
    char instance_name[512];
    char service_domain[256];
    char *hostname = get_local_hostname();
    uint32_t local_ip = get_local_ip();
    uint32_t ttl = 120;

    snprintf(instance_name, sizeof(instance_name), "%s.%s.local", ref->name, ref->regtype);
    snprintf(service_domain, sizeof(service_domain), "%s.local", ref->regtype);

    /* DNS header: response, authoritative, no questions, 4 answers */
    pos = dns_write_u16(pkt, pos, 0x0000); /* ID */
    pos = dns_write_u16(pkt, pos, 0x8400); /* Flags: response, authoritative */
    pos = dns_write_u16(pkt, pos, 0);      /* Questions */
    pos = dns_write_u16(pkt, pos, 4);      /* Answers */
    pos = dns_write_u16(pkt, pos, 0);      /* Authority */
    pos = dns_write_u16(pkt, pos, 0);      /* Additional */

    /* PTR record: _service._tcp.local -> instance._service._tcp.local */
    pos = dns_write_name(pkt, pos, service_domain);
    pos = dns_write_u16(pkt, pos, 12);  /* TYPE: PTR */
    pos = dns_write_u16(pkt, pos, 1);   /* CLASS: IN */
    pos = dns_write_u32(pkt, pos, ttl);
    int ptr_rdlen_pos = pos;
    pos = dns_write_u16(pkt, pos, 0);   /* RDLENGTH placeholder */
    int ptr_rdata_start = pos;
    pos = dns_write_name(pkt, pos, instance_name);
    int ptr_rdlen = pos - ptr_rdata_start;
    dns_write_u16(pkt, ptr_rdlen_pos, ptr_rdlen);

    /* SRV record: instance._service._tcp.local -> host:port */
    pos = dns_write_name(pkt, pos, instance_name);
    pos = dns_write_u16(pkt, pos, 33);  /* TYPE: SRV */
    pos = dns_write_u16(pkt, pos, 0x8001); /* CLASS: IN, cache-flush */
    pos = dns_write_u32(pkt, pos, ttl);
    int srv_rdlen_pos = pos;
    pos = dns_write_u16(pkt, pos, 0);   /* RDLENGTH placeholder */
    int srv_rdata_start = pos;
    pos = dns_write_u16(pkt, pos, 0);   /* Priority */
    pos = dns_write_u16(pkt, pos, 0);   /* Weight */
    pos = dns_write_u16(pkt, pos, ref->port);
    pos = dns_write_name(pkt, pos, hostname);
    int srv_rdlen = pos - srv_rdata_start;
    dns_write_u16(pkt, srv_rdlen_pos, srv_rdlen);

    /* TXT record */
    pos = dns_write_name(pkt, pos, instance_name);
    pos = dns_write_u16(pkt, pos, 16);  /* TYPE: TXT */
    pos = dns_write_u16(pkt, pos, 0x8001); /* CLASS: IN, cache-flush */
    pos = dns_write_u32(pkt, pos, ttl);
    pos = dns_write_u16(pkt, pos, ref->txt_len);
    if (ref->txt_data && ref->txt_len > 0) {
        memcpy(pkt + pos, ref->txt_data, ref->txt_len);
        pos += ref->txt_len;
    }

    /* A record: host.local -> IP */
    pos = dns_write_name(pkt, pos, hostname);
    pos = dns_write_u16(pkt, pos, 1);   /* TYPE: A */
    pos = dns_write_u16(pkt, pos, 0x8001); /* CLASS: IN, cache-flush */
    pos = dns_write_u32(pkt, pos, ttl);
    pos = dns_write_u16(pkt, pos, 4);   /* RDLENGTH */
    memcpy(pkt + pos, &local_ip, 4);
    pos += 4;

    return pos;
}

static void send_announcement(DNSServiceRef ref) {
    uint8_t pkt[MAX_MDNS_PKT];
    int len = build_announcement(pkt, ref);
    if (len > 0) {
        sendto(ref->sock, pkt, len, 0,
               (struct sockaddr *)&ref->mdns_addr, sizeof(ref->mdns_addr));
    }
}

/* Check if an mDNS query is asking about our service */
static int is_query_for_us(const uint8_t *pkt, int len, DNSServiceRef ref) {
    if (len < 12) return 0;
    uint16_t flags = (pkt[2] << 8) | pkt[3];
    if (flags & 0x8000) return 0; /* Not a query */
    uint16_t qdcount = (pkt[4] << 8) | pkt[5];
    if (qdcount == 0) return 0;
    /* Simple heuristic: check if the packet contains our service type string */
    char service_type[128];
    snprintf(service_type, sizeof(service_type), "%s", ref->regtype);
    /* Convert dots to length-prefix for matching */
    for (int i = 12; i < len - 4; i++) {
        if (pkt[i] == '_') return 1; /* Any service query, respond */
    }
    return 0;
}

static void *mdns_thread(void *arg) {
    DNSServiceRef ref = (DNSServiceRef)arg;
    uint8_t buf[MAX_MDNS_PKT];
    struct timeval tv;
    fd_set fds;

    /* Initial announcement burst: 3 times, 1 second apart */
    for (int i = 0; i < 3 && ref->running; i++) {
        send_announcement(ref);
        sleep(1);
    }

    while (ref->running) {
        FD_ZERO(&fds);
        FD_SET(ref->sock, &fds);
        tv.tv_sec = ANNOUNCE_INTERVAL;
        tv.tv_usec = 0;

        int ret = select(ref->sock + 1, &fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(ref->sock, &fds)) {
            int n = recvfrom(ref->sock, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0 && is_query_for_us(buf, n, ref)) {
                usleep(50000 + (rand() % 100000)); /* Small jitter */
                send_announcement(ref);
            }
        } else if (ret == 0) {
            /* Timeout: periodic re-announcement */
            send_announcement(ref);
        }
    }
    return NULL;
}

static int create_mdns_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* If bind fails (another mDNS responder running), try without bind */
        fprintf(stderr, "[mdns] bind to port %d failed: %s (continuing anyway)\n", MDNS_PORT, strerror(errno));
    }

    /* Join multicast group on all interfaces */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "[mdns] join multicast failed: %s\n", strerror(errno));
    }

    /* Set multicast TTL */
    unsigned char ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* Enable loopback so we can see our own packets for debugging */
    unsigned char loop = 0;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    return sock;
}

/* === Public API matching RPiPlay's dnssd.h interface === */

struct dnssd_s {
    txt_record_t raop_record;
    txt_record_t airplay_record;
    DNSServiceRef raop_service;
    DNSServiceRef airplay_service;
    char *name;
    int name_len;
    char *hw_addr;
    int hw_addr_len;
};

dnssd_t *
dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len, int *error)
{
    dnssd_t *dnssd;

    if (error) *error = DNSSD_ERROR_NOERROR;

    dnssd = calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    if (!dnssd->name) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);

    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, hw_addr_len);
    if (!dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);

    printf("[mdns] Initialized mDNS for \"%s\"\n", dnssd->name);
    return dnssd;
}

void
dnssd_destroy(dnssd_t *dnssd)
{
    if (dnssd) {
        free(dnssd->name);
        free(dnssd->hw_addr);
        free(dnssd);
    }
}

/* TXT record helpers */
static void txt_record_create(txt_record_t *rec) {
    rec->len = 0;
    memset(rec->data, 0, sizeof(rec->data));
}

static void txt_record_set_value(txt_record_t *rec, const char *key, uint8_t value_len, const void *value) {
    int entry_len = strlen(key) + 1 + value_len; /* key=value */
    if (rec->len + 1 + entry_len > MAX_TXT_SIZE) return;
    rec->data[rec->len++] = (uint8_t)entry_len;
    memcpy(rec->data + rec->len, key, strlen(key));
    rec->len += strlen(key);
    rec->data[rec->len++] = '=';
    memcpy(rec->data + rec->len, value, value_len);
    rec->len += value_len;
}

static DNSServiceRef service_register(const char *name, const char *regtype, uint16_t port,
                                       const uint8_t *txt_data, uint16_t txt_len) {
    struct _DNSServiceRef_t *ref = calloc(1, sizeof(struct _DNSServiceRef_t));
    if (!ref) return NULL;

    ref->name = strdup(name);
    ref->regtype = strdup(regtype);
    ref->port = port;
    ref->running = 1;

    if (txt_data && txt_len > 0) {
        ref->txt_data = malloc(txt_len);
        memcpy(ref->txt_data, txt_data, txt_len);
        ref->txt_len = txt_len;
    }

    ref->mdns_addr.sin_family = AF_INET;
    ref->mdns_addr.sin_port = htons(MDNS_PORT);
    ref->mdns_addr.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    ref->sock = create_mdns_socket();
    if (ref->sock < 0) {
        fprintf(stderr, "[mdns] Failed to create socket\n");
        free(ref->name);
        free(ref->regtype);
        free(ref->txt_data);
        free(ref);
        return NULL;
    }

    printf("[mdns] Registering %s as %s on port %d\n", name, regtype, port);

    pthread_create(&ref->thread, NULL, mdns_thread, ref);
    return ref;
}

static void service_deallocate(DNSServiceRef ref) {
    if (!ref) return;
    ref->running = 0;
    pthread_join(ref->thread, NULL);
    close(ref->sock);
    free(ref->name);
    free(ref->regtype);
    free(ref->txt_data);
    free(ref);
}

int
dnssd_register_raop(dnssd_t *dnssd, unsigned short port)
{
    char servname[MAX_SERVNAME];

    assert(dnssd);

    txt_record_create(&dnssd->raop_record);
    txt_record_set_value(&dnssd->raop_record, "ch", strlen(RAOP_CH), RAOP_CH);
    txt_record_set_value(&dnssd->raop_record, "cn", strlen(RAOP_CN), RAOP_CN);
    txt_record_set_value(&dnssd->raop_record, "da", strlen(RAOP_DA), RAOP_DA);
    txt_record_set_value(&dnssd->raop_record, "et", strlen(RAOP_ET), RAOP_ET);
    txt_record_set_value(&dnssd->raop_record, "vv", strlen(RAOP_VV), RAOP_VV);
    txt_record_set_value(&dnssd->raop_record, "ft", strlen(RAOP_FT), RAOP_FT);
    txt_record_set_value(&dnssd->raop_record, "am", strlen(GLOBAL_MODEL), GLOBAL_MODEL);
    txt_record_set_value(&dnssd->raop_record, "md", strlen(RAOP_MD), RAOP_MD);
    txt_record_set_value(&dnssd->raop_record, "rhd", strlen(RAOP_RHD), RAOP_RHD);
    txt_record_set_value(&dnssd->raop_record, "pw", strlen("false"), "false");
    txt_record_set_value(&dnssd->raop_record, "sr", strlen(RAOP_SR), RAOP_SR);
    txt_record_set_value(&dnssd->raop_record, "ss", strlen(RAOP_SS), RAOP_SS);
    txt_record_set_value(&dnssd->raop_record, "sv", strlen(RAOP_SV), RAOP_SV);
    txt_record_set_value(&dnssd->raop_record, "tp", strlen(RAOP_TP), RAOP_TP);
    txt_record_set_value(&dnssd->raop_record, "txtvers", strlen(RAOP_TXTVERS), RAOP_TXTVERS);
    txt_record_set_value(&dnssd->raop_record, "sf", strlen(RAOP_SF), RAOP_SF);
    txt_record_set_value(&dnssd->raop_record, "vs", strlen(RAOP_VS), RAOP_VS);
    txt_record_set_value(&dnssd->raop_record, "vn", strlen(RAOP_VN), RAOP_VN);
    txt_record_set_value(&dnssd->raop_record, "pk", strlen(RAOP_PK), RAOP_PK);

    if (utils_hwaddr_raop(servname, sizeof(servname), dnssd->hw_addr, dnssd->hw_addr_len) < 0) {
        return -1;
    }

    if (sizeof(servname) < strlen(servname) + 1 + dnssd->name_len + 1) {
        return -2;
    }

    strncat(servname, "@", sizeof(servname)-strlen(servname)-1);
    strncat(servname, dnssd->name, sizeof(servname)-strlen(servname)-1);

    dnssd->raop_service = service_register(servname, "_raop._tcp",
                                            port,
                                            dnssd->raop_record.data,
                                            dnssd->raop_record.len);
    return dnssd->raop_service ? 1 : -1;
}

int
dnssd_register_airplay(dnssd_t *dnssd, unsigned short port)
{
    char device_id[3 * MAX_HWADDR_LEN];

    assert(dnssd);

    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd->hw_addr, dnssd->hw_addr_len) < 0) {
        return -1;
    }

    txt_record_create(&dnssd->airplay_record);
    txt_record_set_value(&dnssd->airplay_record, "deviceid", strlen(device_id), device_id);
    txt_record_set_value(&dnssd->airplay_record, "features", strlen(AIRPLAY_FEATURES), AIRPLAY_FEATURES);
    txt_record_set_value(&dnssd->airplay_record, "flags", strlen(AIRPLAY_FLAGS), AIRPLAY_FLAGS);
    txt_record_set_value(&dnssd->airplay_record, "model", strlen(GLOBAL_MODEL), GLOBAL_MODEL);
    txt_record_set_value(&dnssd->airplay_record, "pk", strlen(AIRPLAY_PK), AIRPLAY_PK);
    txt_record_set_value(&dnssd->airplay_record, "pi", strlen(AIRPLAY_PI), AIRPLAY_PI);
    txt_record_set_value(&dnssd->airplay_record, "srcvers", strlen(AIRPLAY_SRCVERS), AIRPLAY_SRCVERS);
    txt_record_set_value(&dnssd->airplay_record, "vv", strlen(AIRPLAY_VV), AIRPLAY_VV);

    dnssd->airplay_service = service_register(dnssd->name, "_airplay._tcp",
                                               port,
                                               dnssd->airplay_record.data,
                                               dnssd->airplay_record.len);
    return dnssd->airplay_service ? 1 : -1;
}

const char *
dnssd_get_airplay_txt(dnssd_t *dnssd, int *length)
{
    *length = dnssd->airplay_record.len;
    return (const char *)dnssd->airplay_record.data;
}

const char *
dnssd_get_name(dnssd_t *dnssd, int *length)
{
    *length = dnssd->name_len;
    return dnssd->name;
}

const char *
dnssd_get_hw_addr(dnssd_t *dnssd, int *length)
{
    *length = dnssd->hw_addr_len;
    return dnssd->hw_addr;
}

void
dnssd_unregister_raop(dnssd_t *dnssd)
{
    assert(dnssd);
    if (dnssd->raop_service) {
        service_deallocate(dnssd->raop_service);
        dnssd->raop_service = NULL;
    }
}

void
dnssd_unregister_airplay(dnssd_t *dnssd)
{
    assert(dnssd);
    if (dnssd->airplay_service) {
        service_deallocate(dnssd->airplay_service);
        dnssd->airplay_service = NULL;
    }
}
