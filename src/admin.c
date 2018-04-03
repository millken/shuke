//
// Created by yangyu on 2/18/17.
//

#include "shuke.h"
#include "utils.h"

#include <sys/resource.h>
#include <sys/utsname.h>

DEF_LOG_MODULE(RTE_LOGTYPE_USER1, "ADMIN");

#define LEN_BYTES 4
#define ADMIN_CONN_EXPIRE 3600

typedef struct _adminReply {
    struct _adminReply *next;
    int state;
    size_t wsize;    // total size in data
    size_t wcur;     // write cursor
    char len[LEN_BYTES];
    sds data;
} adminReply;

typedef struct _adminConn {
    int fd;
    int state;
    aeEventLoop *el;
    char data[MAX_UDP_SIZE];
    size_t packetSize;    // size of current dns query packet
    size_t nRead;
    char len[LEN_BYTES];

    struct list_head node;
    long lastActiveTs;

    adminReply *replyList;
} adminConn;

static void adminAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void adminReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void adminWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static int adminCron(struct aeEventLoop *el, long long id, void *clientData);

void adminConnDestroy(adminConn *c);

static void versionCommand(int argc, char *argv[], adminConn *c);
static void infoCommand(int argc, char *argv[], adminConn *c);
static void debugCommand(int argc, char *argv[], adminConn *c);
static void zoneCommand(int argc, char *argv[], adminConn *c);
static void configCommand(int argc, char *argv[], adminConn *c);

typedef void adminCommandProc(int argc, char *argv[], adminConn *c);
typedef struct {
    char *name;
    adminCommandProc *proc;
}adminCommand;

static adminCommand adminCommandTable[] = {
    {(char *)"version", versionCommand},
    {(char *)"debug", debugCommand},
    {(char *)"info", infoCommand},
    {(char *)"zone", zoneCommand},
    {(char *)"config", configCommand}
};

static inline void adminConnMoveTail(adminConn *c) {
    c->lastActiveTs = sk.unixtime;
    list_del(&(c->node));
    list_add_tail(&(c->node), &(sk.head));
}

int initAdminServer(void) {
    char *host = sk.admin_host;
    int port = sk.admin_port;

    sk.commands = dictCreate(&dictTypeCaseStringCopyKey, NULL, sk.master_numa_id);
    int numcommands = sizeof(adminCommandTable)/sizeof(adminCommand);
    for (int j = 0; j < numcommands; j++) {
        adminCommand *c = adminCommandTable+j;
        if (dictAdd(sk.commands, c->name, c) != DICT_OK) {
            LOG_FATAL("can't add command %s to dict", c->name);
        }
    }
    INIT_LIST_HEAD(&(sk.head));

    if (host == NULL) {
        // bind all addresses
        sk.fd = anetTcpServer(sk.errstr, port, NULL, sk.tcp_backlog, 0);
    } else {
        if (strchr(host, ':') == NULL) {
            sk.fd = anetTcpServer(sk.errstr, port, host, sk.tcp_backlog, 0);
        } else {
            sk.fd = anetTcp6Server(sk.errstr, port, host, sk.tcp_backlog, 0);
        }
    }
    if (sk.fd == ANET_ERR) {
        return ERR_CODE;
    }
    anetNonBlock(NULL,sk.fd);
    if (aeCreateFileEvent(sk.el, sk.fd, AE_READABLE, adminAcceptHandler, NULL) == AE_ERR) {
        LOG_ERROR("Can't create file event for listen socket %d", sk.fd);
        return ERR_CODE;
    }
    if (aeCreateTimeEvent(sk.el, TIME_INTERVAL, adminCron, NULL, NULL) == AE_ERR) {
        return ERR_CODE;
    }
    return OK_CODE;
}

void releaseAdminServer(void) {
    dictRelease(sk.commands);
    struct list_head *pos, *temp;
    list_for_each_safe(pos, temp, &(sk.head)) {
        adminConn *c = list_entry(pos, adminConn, node);
        adminConnDestroy(c);
    }
}

static adminReply *adminReplyCreate(sds data) {
    size_t dataLen = sdslen(data);

    adminReply *rep = zcalloc(sizeof(*rep));
    rep->data = data;
    rep->state = CONN_WRITE_LEN;
    dump32be(dataLen, rep->len);
    rep->wsize = dataLen;
    return rep;
}

static void adminReplyDestroy(adminReply *rep) {
    sdsfree(rep->data);
    zfree(rep);
}

static adminConn* adminConnCreate(int fd) {
    adminConn *c = zcalloc(sizeof(*c));
    c->fd = fd;
    c->state = CONN_READ_LEN;
    c->el = sk.el;
    c->lastActiveTs = sk.unixtime;
    list_add_tail(&(c->node), &(sk.head));

    return c;
}

static void adminConnAppendW(adminConn *c, adminReply *rep) {
    if (c->replyList == NULL) {
        LOG_DEBUG("admin add write event");
        aeCreateFileEvent(c->el, c->fd, AE_WRITABLE, adminWriteHandler, c);
    }
    rep->next = c->replyList;
    c->replyList = rep;
}

void adminConnDestroy(adminConn *c) {
    list_del(&(c->node));
    aeDeleteFileEvent(c->el, c->fd, AE_READABLE|AE_WRITABLE);
    close(c->fd);
    while(c->replyList) {
        adminReply *rep = c->replyList;
        c->replyList = rep->next;
        adminReplyDestroy(rep);
    }
    zfree(c);
}

static void dispatchCommand(adminConn *c) {
    char *ptr = c->data;
    char *argv[10];
    int argc = 10;
    sds s = NULL;

    if (tokenize(ptr, argv, &argc, " \t") < 0) {
        s = sdsnewprintf("command contains too many tokens or syntax error.");
        goto error;
    }
    if (argc < 1) {
        s = sdsnew("need a command.");
        goto error;
    }
    for (int i = 0; i < argc; ++i) {
        argv[i] = strip(argv[i], "\"");
    }

    strtoupper(argv[0]);
    LOG_INFO("receive %s command.", argv[0]);
    adminCommand *cmd = dictFetchValue(sk.commands, argv[0]);
    if (cmd == NULL) goto error;
    cmd->proc(argc, argv, c);
    return;

error:
    if (s == NULL) {
        s = sdsnewprintf("invalid command %s.", argv[0]);
    }
    LOG_DEBUG("Buf: %s, cmd: %s", s, argv[0]);
    adminReply *rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
    return;
}

static int adminCron(struct aeEventLoop *el, long long id, void *clientData) {
    // LOG_DEBUG("admin time event");
    ((void) el); ((void) id); ((void) clientData);

    struct list_head *pos, *temp;
    list_for_each_safe(pos, temp, &(sk.head)) {
        adminConn *c = list_entry(pos, adminConn, node);
        if (sk.unixtime - c->lastActiveTs < ADMIN_CONN_EXPIRE) break;

        LOG_INFO("admin connection is idle more than %ds, close it.", ADMIN_CONN_EXPIRE);
        adminConnDestroy(c);
    }
    return TIME_INTERVAL;
}

static void adminReadHandler(struct aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void) el); ((void) mask);
    ssize_t n = 0;
    int remainRead;
    adminConn *conn = (adminConn *) (privdata);
    assert(conn->fd == fd);
    // move this connection to tail.
    adminConnMoveTail(conn);

    while(1) {
        switch (conn->state) {
        case CONN_READ_LEN:
            remainRead = LEN_BYTES - conn->nRead;
            n = read(conn->fd, conn->len + conn->nRead, remainRead);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                LOG_WARN("tcp read: %s", strerror(errno));
                adminConnDestroy(conn);
                return;
            }
            if (n == 0) {   // the peer close the socket prematurely.
                if (conn->nRead > 0) {
                    LOG_WARN("the connection peer closed socket prematurely.");
                }
                adminConnDestroy(conn);
                return;
            }
            conn->nRead += n;
            if (conn->nRead < LEN_BYTES) return;

            assert(conn->packetSize == 0);
            conn->packetSize = load32be(conn->len);
            conn->nRead = 0;
            conn->state = CONN_READ_N;
            break;
        case CONN_READ_N:
            remainRead = (int)(conn->packetSize - conn->nRead);
            if (remainRead > 0) {
                n = read(conn->fd, conn->data + conn->nRead, remainRead);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return;
                    }
                    LOG_WARN("tcp read: %s", strerror(errno));
                    adminConnDestroy(conn);
                    return;
                } else if (n == 0) {   // the peer close the socket prematurely.
                    LOG_WARN("the connection peer closed socket prematurely.");
                    adminConnDestroy(conn);
                    return;
                }
            }
            conn->nRead += n;
            if (conn->nRead < conn->packetSize) return;

            conn->data[conn->nRead] = 0;
            dispatchCommand(conn);

            // reset connection read state
            conn->nRead = 0;
            conn->packetSize = 0;
            conn->state = CONN_READ_LEN;
            break;
        }
    }
}

static void adminWriteHandler(struct aeEventLoop *el, int fd,
                              void *privdata, int mask) {
    UNUSED(mask);
    ssize_t n;
    adminReply *rep;
    int remainWrite;
    adminConn *c = (adminConn *) (privdata);
    // move this connection to tail.
    adminConnMoveTail(c);

    while (c->replyList) {
        rep = c->replyList;
        switch(rep->state) {
        case CONN_WRITE_LEN:
            remainWrite = (int)(LEN_BYTES-rep->wcur);
            n = write(fd, rep->len+rep->wcur, remainWrite);
            if (n <= 0) {
                if ((n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                LOG_WARN("admin server can't write data to client: %s", strerror(errno));
                adminConnDestroy(c);
                return;
            }
            rep->wcur += n;
            if (rep->wcur < LEN_BYTES) return;

            rep->state = CONN_WRITE_N;
            rep->wcur = 0;
            break;
        case CONN_WRITE_N:
            n = write(fd, rep->data+rep->wcur, rep->wsize-rep->wcur);
            if (n <= 0) {
                if ((n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                LOG_WARN("admin server can't write data to client: %s", strerror(errno));
                adminConnDestroy(c);
                return;
            }
            rep->wcur += n;
            if (rep->wcur == rep->wsize) {
                c->replyList = rep->next;
                adminReplyDestroy(rep);
            }
            break;
        }
        n = write(fd, rep->data+rep->wcur, rep->wsize-rep->wcur);
        if (n <= 0) {
            if ((n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            LOG_WARN("admin server can't write data to client: %s", strerror(errno));
            adminConnDestroy(c);
            return;
        }
        rep->wcur += n;
        if (rep->wcur == rep->wsize) {
            c->replyList = rep->next;
            adminReplyDestroy(rep);
        }
    }
    LOG_DEBUG("admin delete write event");
    aeDeleteFileEvent(el, fd, AE_WRITABLE);
}

#define MAX_ACCEPTS_PER_CALL 1000
static void adminAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void) privdata);
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[IP_STR_LEN];
    UNUSED(mask);

    while(max--) {
        cfd = anetTcpAccept(sk.errstr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                LOG_WARN("Accepting client connection: %s", sk.errstr);
            return;
        }
        LOG_INFO("Admin server accepted %s:%d", cip, cport);
        anetNonBlock(NULL, cfd);
        anetEnableTcpNoDelay(NULL, cfd);
        if (sk.tcp_keepalive) {
            anetKeepAlive(NULL, fd, sk.tcp_keepalive);
        }
        adminConn *c = adminConnCreate(cfd);

        if (aeCreateFileEvent(el, cfd, AE_READABLE, adminReadHandler, c) == AE_ERR) {
            LOG_ERROR("admin server can't create file event for client. %s", strerror(errno));
            return;
        }
    }
}

static void versionCommand(int argc, char *argv[], adminConn *c) {
    ((void) argv);
    adminReply *rep;
    sds s;
    if (argc > 1) {
        s = sdsnewprintf("VERSION command needs 0 arguments but gives %d.", argc-1);
        goto end;
    }
    s = sdsnewprintf("%s", SHUKE_VERSION);
end:
    rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
}

static char* statArrayToStr(uint64_t arr[], int nele) {
    static char buf[1024];
    int max = 1024;
    char human[128];
    int n = 0;
    int offset = 0;
    for (int i = 0; i < nele; ++i) {
        if (offset >= max-1) break;
        numberToHuman(human, (unsigned long long)arr[i]);
        n = snprintf(buf+offset, (size_t)(max-offset), "%d: %s ", i, human);
        offset += n;
    }

    return buf;
}

static sds genInfoString(char *section) {
    time_t uptime = sk.unixtime - sk.starttime;
    struct rusage self_ru;
    sds s = sdsempty();
    int allsections = 0, defsections = 0;
    int sections = 0;
    if (section == NULL) section = "default";
    allsections = (strcasecmp(section, "all") == 0);
    defsections = (strcasecmp(section, "default") == 0);

    getrusage(RUSAGE_SELF, &self_ru);

    // server
    if (allsections || defsections || (strcasecmp(section, "server") == 0)) {
        static bool call_uname = true;
        static struct utsname name;
        if (call_uname) {
            uname(&name);
            call_uname = false;
        }

        if (sections++) s = sdscat(s, "\r\n");
        s = sdscatprintf(s,
                         "# Server\r\n"
                         "version:%s\r\n"
                         "os:%s %s %s\r\n"
                         "arch_bits:%d\r\n"
                         "multiplexing_api:%s\r\n"
                         "process_id:%ld\r\n"
                         "port:%d\r\n"
                         "uptime_in_seconds:%jd\r\n"
                         "uptime_in_days:%jd\r\n"
                         "config_file:%s\r\n",
                         SHUKE_VERSION,
                         name.sysname, name.release, name.machine,
                         sk.arch_bits,
                         aeGetApiName(),
                         (long)getpid(),
                         sk.port,
                         (intmax_t)uptime,
                         (intmax_t)(uptime/(3600*24)),
                         sk.configfile);
    }

    // memory
    if (allsections || defsections || (strcasecmp(section, "memory") == 0)) {
        if (sections++) s = sdscat(s, "\r\n");
        struct rte_malloc_socket_stats stat;
        for (int i = 0; i < sk.nr_numa_id; ++i) {
            int numa_id = sk.numa_ids[i];
            rte_malloc_get_socket_stats(numa_id, &stat);

            s = sdscatprintf(s,
                             "#Socket %d Memory\r\n"
                             "total_size:%zu\r\n"
                             "total_alloc_size:%zu\r\n"
                             "free_size:%zu\r\n"
                             "greatest_free_size:%zu\r\n"
                             "free_count:%u\r\n"
                             "alloc_count:%u\r\n",
                             numa_id,
                             stat.heap_totalsz_bytes,
                             stat.heap_allocsz_bytes,
                             stat.heap_freesz_bytes,
                             stat.greatest_free_size,
                             stat.free_count,
                             stat.alloc_count);
        }
    }

    // statistics
    if (allsections || defsections || (strcasecmp(section, "stats") == 0)) {
        if (sections++) s = sdscat(s, "\r\n");
        static int64_t prev_nr_req = 0;
        static int64_t prev_nr_dropped = 0;
        long long prev_ms = sk.last_collect_ms;

        collectStats();

        int64_t interval = (int64_t)(sk.last_collect_ms - prev_ms);
        if (unlikely(interval == 0)) interval = 1;

        int64_t nr_req = sk.nr_req;
        int64_t nr_dropped = sk.nr_dropped;
         s = sdscatprintf(s,
                          "# Stats\r\n"
                          "total_requests:%lld\r\n"
                          "dropped_requests:%lld\r\n"
                          "avg_qps:%llu\r\n"
                          "qps:%llu\r\n"
                          "dropped_qps:%llu\r\n"
                          "num_zones:%lu\r\n",
                          (long long)nr_req,
                          (long long)nr_dropped,
                          (long long unsigned)(nr_req/uptime),
                          (long long unsigned)((nr_req - prev_nr_req)/(interval/1000.0)),
                          (long long unsigned)((nr_dropped - prev_nr_dropped)/(interval/1000.0)),
                          ltreeGetNumZones(sk.lt));
        prev_nr_req = nr_req;
        prev_nr_dropped = nr_dropped;

        if (!sk.only_udp) {
            s = sdscat(s, "\r\n");
            s = sdscatprintf(s,
                             "# Tcp stats\r\n"
                             "num_tcp_conn: %llu\r\n"
                             "total_tcp_conn: %llu\r\n"
                             "rejected_tcp_conn: %llu\r\n",
                             (long long unsigned)sk.num_tcp_conn,
                             (long long unsigned)sk.total_tcp_conn,
                             (long long unsigned)sk.rejected_tcp_conn);
        }

        struct rte_eth_stats eth_stats;
        for (int i = 0; i < sk.nr_ports; i++) {
            uint8_t portid = (uint8_t )sk.port_ids[i];
            if (rte_eth_stats_get(portid, &eth_stats) != 0) {
                LOG_WARN("can't get stats of port %d.", portid);
                continue;
            }
            s = sdscat(s, "\r\n");
            s = sdscatprintf(s,
                             "# Port %d stats\r\n"
                             "addr:%s\r\n"
                             "input_packets:%llu\r\n"
                             "input_packets_per_sec:%llu\r\n"
                             "output_packets:%llu\r\n"
                             "output_packets_per_sec:%llu\r\n"
                             "input_bytes:%llu\r\n"
                             "input_bytes_per_sec:%llu\r\n"
                             "output_bytes:%llu\r\n"
                             "output_bytes_per_sec:%llu\r\n"
                             "input_missed:%llu\r\n"
                             "input_errors:%llu\r\n"
                             "output_errors:%llu\r\n"
                             "queue_input_packets:%s\r\n"
                             "queue_output_packets:%s\r\n"
                             "queue_errors:%s\r\n"
                             "\r\n",
                             portid,
                             sk.port_info[portid]->eth_addr_s,
                             (long long unsigned) eth_stats.ipackets,
                             (long long unsigned) eth_stats.ipackets/uptime,
                             (long long unsigned) eth_stats.opackets,
                             (long long unsigned) eth_stats.opackets/uptime,
                             (long long unsigned) eth_stats.ibytes,
                             (long long unsigned) eth_stats.ibytes/uptime,
                             (long long unsigned) eth_stats.obytes,
                             (long long unsigned) eth_stats.obytes/uptime,
                             (long long unsigned) eth_stats.imissed,
                             (long long unsigned) eth_stats.ierrors,
                             (long long unsigned) eth_stats.oerrors,
                             statArrayToStr(eth_stats.q_ipackets, RTE_ETHDEV_QUEUE_STAT_CNTRS),
                             statArrayToStr(eth_stats.q_opackets, RTE_ETHDEV_QUEUE_STAT_CNTRS),
                             statArrayToStr(eth_stats.q_errors, RTE_ETHDEV_QUEUE_STAT_CNTRS));
        }

        s = sdscat(s, "# Core received packets\r\n");

        for (int i = 0; i < sk.nr_lcore_ids; ++i) {
            char human[128];
            unsigned lcore_id = (unsigned )sk.lcore_ids[i];
            if (lcore_id == rte_get_master_lcore()) continue;
            lcore_conf_t *qconf = &sk.lcore_conf[lcore_id];
            numberToHuman(human, (unsigned long long)qconf->received_req);
            s = sdscatprintf(s, "%d: %s ", lcore_id, human);
        }
        s = sdscat(s, "\r\n");
    }

    // cpu usage
    if (allsections || defsections || (strcasecmp(section, "cpu") == 0)) {
        if (sections++) s = sdscat(s, "\r\n");
        s = sdscatprintf(s,
                         "# CPU\r\n"
                         "used_cpu_sys:%.2f\r\n"
                         "used_cpu_user:%.2f\r\n",
                         (float) self_ru.ru_stime.tv_sec + (float) self_ru.ru_stime.tv_usec / 1000000,
                         (float) self_ru.ru_utime.tv_sec + (float) self_ru.ru_utime.tv_usec / 1000000);
    }
    return s;
}

static void infoCommand(int argc, char *argv[], adminConn *c) {
    ((void) argv);
    char *section = argc == 2 ? argv[1] : "default";
    adminReply *rep;
    sds s = NULL;

    if (argc > 2) {
        s = sdsnewprintf(s, "INFO command needs 0 or 1 argument but gives %d", argc-1);
        goto end;
    }
    s = genInfoString(section);

end:
    rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
    return;
}

sds genDebugInfo() {
    char buf[4096];
    sds s = sdsempty();
    int ret = intArrayToStr(sk.lcore_ids, sk.nr_lcore_ids, ",", buf, 4096);
    if (ret < 0) {
        snprintf(buf, 4096, "lcore list is too long");
    }
    s = sdscatprintf(s,
                     "master_numa_id:   %d\r\n"
                     "lcore_ids:        %s\r\n"
                     "total_lcore_list: %s\r\n"
                     "\r\n",
                     sk.master_lcore_id,
                     buf,
                     sk.total_lcore_list
        );
    for (int i = 0; i < sk.nr_numa_id; ++i) {
        int numa_id = sk.numa_ids[i];
        numaNode_t *node = sk.nodes[numa_id];
        ret = intArrayToStr(node->lcore_ids, node->nr_lcore_ids, ",", buf, 4096);
        if (ret < 0) {
            snprintf(buf, 4096, "lcore list is too long");
        }
        s = sdscatprintf(s,
                         "# NUMA NODE:  %d\r\n"
                         "max_lcore_id: %d\r\n"
                         "min_lcore_id: %d\r\n"
                         "nr_lcore_id:  %d\r\n"
                         "lcore_list:   %s\r\n"
                         "\r\n",
                         node->numa_id,
                         node->max_lcore_id,
                         node->min_lcore_id,
                         node->nr_lcore_ids,
                         buf);
    }
    for (int i = 0; i < RTE_MAX_ETHPORTS; ++i) {
        port_info_t *pinfo = sk.port_info[i];
        if (!pinfo) continue;
        ret = intArrayToStr(pinfo->lcore_list, pinfo->nr_lcore, ",", buf, 4096);
        if (ret < 0) {
            snprintf(buf, 4096, "lcore list is too long");
        }
        s = sdscatprintf(s,
                         "# PORT:  %d\r\n"
                         "mac addr: %s\r\n"
                         "lcore_list: %s\r\n",
                         pinfo->port_id,
                         pinfo->eth_addr_s,
                         buf);
        s = sdscat(s, "\r\n");
    }
    return s;
}

static void debugCommand(int argc, char *argv[], adminConn *c) {
    ((void) argc); ((void) argv);
    adminReply *rep;
    sds s = NULL;

    if (argc != 2) {
        s = sdsnewprintf("DEBUG command needs 1 arguments, but gives ", argc-1);
        goto end;
    }
    if (strcasecmp(argv[1], "segfault") == 0) {
        *((char *) -1) = 'x';
    } else if (strcasecmp(argv[1], "oom") == 0) {
        void *ptr = zmalloc(ULONG_MAX);
        zfree(ptr);
        s = sdsnew("OK");
    } else if (strcasecmp(argv[1], "info") == 0) {
        s = genDebugInfo();
    } else {
        s = sdsnewprintf("unknown debug subcommand %s.", argv[1]);
    }

end:
    rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
    return;
}

static int setZoneFileInConf(char *errstr, char *dotOrigin, char *fname) {
    int err = OK_CODE;
    char *k = NULL, *v = NULL;
    k = strip(dotOrigin, "\"");
    v = strip(fname, "\"");
    if (isAbsDotDomain(k) == false) {
        snprintf(errstr, ERR_STR_LEN, "%s is not absolute domain name.", k);
        goto error;
    }
    v = toAbsPath(v, sk.zone_files_root);
    if (access(v, F_OK) == -1) {
        snprintf(errstr, ERR_STR_LEN, "%s doesn't exist.", v);
        goto error;
    }
    dictReplace(sk.zone_files_dict, k, v);
    goto ok;
error:
    err = ERR_CODE;
ok:
    // don't use zfree.
    free(v);
    return err;
}

static void configCommand(int argc, char *argv[], adminConn *c) {
    adminReply *rep;
    char errstr[ERR_STR_LEN];
    sds s = NULL;

    if (argc < 2) {
        s = sdsnewprintf("CONFIG command needs at least 1 argument, but got %d", argc-1);
        goto end;
    }
    if (strcasecmp(argv[1], "GET") == 0) {
        ;
    } else if (strcasecmp(argv[1], "GETALL") == 0) {
        s = configToStr();
        goto end;
    } else if (strcasecmp(argv[1], "SET") == 0) {
        ;
    } else if (strcasecmp(argv[1], "ZONEFILE") == 0) {
        if (strcasecmp(sk.data_store, "file") != 0) {
            s = sdsnewprintf("data_store if %s, so you can't manipulate zone files.", sk.data_store);
            goto end;
        }
        if (strcasecmp(argv[2], "SET") == 0) {
            if (argc - 3 != 2) {
                s = sdsnewprintf("need 2 argument for CONFIG ZONFILE SET.");
                goto end;
            }
            if (setZoneFileInConf(errstr, argv[3], argv[4]) != OK_CODE) {
                s = sdsnew(errstr);
            }
        } else if (strcasecmp(argv[2], "GET") == 0) {
            if (argc - 3 != 1) {
                s = sdsnewprintf("need 1 argument for CONFIG ZONFILE GET.");
                goto end;
            }
            char *fname = dictFetchValue(sk.zone_files_dict, argv[3]);
            s = sdsnew(fname);
            goto end;
        }
    } else {
        s = sdsnewprintf("unknown subcommand(%s) for CONFIG command.", argv[1]);
    }
end:
    if (s == NULL) s = sdsnew("OK");
    rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
}

static void zoneCommand(int argc, char *argv[], adminConn *c) {
    adminReply *rep;
    zone *z;
    sds s = NULL;
    char origin[MAX_DOMAIN_LEN+2];
    char dotOrigin[MAX_DOMAIN_LEN+2];

    if (argc < 2) {
        s = sdsnewprintf("ZONE command needs at least 1 arguments, but gives %d", argc-1);
        goto end;
    }
    if (strcasecmp(argv[1], "GET") == 0) {
        if (argc != 3) {
            s = sdsnewprintf("ZONE GET needs 1 argument, but gives %d.", argc-2);
            goto end;
        }
        strncpy(dotOrigin, argv[2], MAX_DOMAIN_LEN);
        if (isAbsDotDomain(dotOrigin) == false) {
            strcat(dotOrigin, ".");
        }
        dot2lenlabel(dotOrigin, origin);
        ltreeRLock(sk.lt);
        z = ltreeGetZoneExactRaw(sk.lt, origin);
        if (z == NULL) {
            s = sdsnewprintf("zone %s not found", dotOrigin);
            ltreeRUnlock(sk.lt);
            goto end;
        }
        s = zoneToStr(z);
        ltreeRUnlock(sk.lt);
    } else if (strcasecmp(argv[1], "GET_RRSET") == 0) {
        if (argc != 4) {
            s = sdsnewprintf("ZONE GET_RRSET needs 2 argument, but gives %d.", argc-2);
            goto end;
        }
        int type = strToDNSType(argv[3]);
        if (type < 0) {
            s = sdsnewprintf("unsupport dns type %s.", argv[3]);
            goto end;
        }
        strncpy(dotOrigin, argv[2], MAX_DOMAIN_LEN);
        if (isAbsDotDomain(dotOrigin) == false) {
            strcat(dotOrigin, ".");
        }
        dot2lenlabel(dotOrigin, origin);

        ltreeRLock(sk.lt);
        z = ltreeGetZoneRaw(sk.lt, origin);

        if (z == NULL) {
            ltreeRUnlock(sk.lt);
            s = sdsnewprintf("zone %s not found.", argv[2]);
            goto end;
        }
        RRSet *rs = zoneFetchTypeVal(z, origin, (uint16_t)type);
        if (rs == NULL) s = sdsnewprintf("RRSet <%s %s> not found.", argv[2], argv[3]);
        else s = RRSetToStr(rs);
        ltreeRUnlock(sk.lt);
    } else if (strcasecmp(argv[1], "GETALL") == 0) {
        if (argc != 2) {
            s = sdsnewprintf("ZONE GETALL needs no arguments, but gives %d.", argc-2);
            goto end;
        }
        s = ltreeToStr(sk.lt);
    } else if (strcasecmp(argv[1], "RELOAD") == 0) {
        if (argc < 3) {
            s = sdsnewprintf("ZONE RELOAD command needs at least 1 arguments but gives  %d.", argc-2);
            goto end;
        }
        if (sk.checkAsyncContext() == ERR_CODE) {
            s = sdsnewprintf("%s is not ready to reload zone.", sk.data_store);
            goto end;
        }
        for (int i = 2; i < argc; ++i) {
            strncpy(dotOrigin, argv[i], MAX_DOMAIN_LEN);
            if (!isAbsDotDomain(dotOrigin)) strcat(dotOrigin, ".");
            if (asyncReloadZoneRaw(dotOrigin) != OK_CODE) {
                s = sdsnewprintf("Error: %s", sk.errstr);
            }
        }
    } else if (strcasecmp(argv[1], "RELOADALL") == 0) {
        if (argc != 2) {
            s = sdsnewprintf("ZONE RELOADALL command needs 0 argument, but gives %d.", argc-2);
            goto end;
        }
        if (sk.checkAsyncContext() == ERR_CODE) {
            s = sdsnewprintf("%s is not ready to reload all zone.", sk.data_store);
            goto end;
        }
        triggerReloadAllZone();
    } else if (strcasecmp(argv[1], "GET_NUMZONES") == 0) {
        size_t n = ltreeGetNumZones(sk.lt);
        s = sdsnewprintf("%lu", n);
    } else {
        s = sdsnewprintf("unknown subcommand %s for ZONE.", argv[1]);
    }
end:
    if (s == NULL) s = sdsnew("OK");
    rep = adminReplyCreate(s);
    adminConnAppendW(c, rep);
}
