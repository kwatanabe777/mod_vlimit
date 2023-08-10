/*
// -------------------------------------------------------------------
// mod_vlimit
//   Control the number of references from the same IP address to file access
//      or the number of references from the same file name to file access
//
//   By matsumoto_r (MATSUMOTO Ryosuke) Sep 2010 in Japan
//
// Date   2010/09/21
// Version  1.00
//
// change log
//  2010/09/21 0.10 SHM_DATA files count on shared memory add matsumoto_r
//  2010/09/21 0.20 vlimit_response()_end create matsumoto_r
//  2010/09/22 0.21 make_ip_slot_list() create matsumoto_r
//  2010/09/22 0.22 make_file_slot_list() create matsumoto_r
//  2010/09/22 0.90 vlimit_mutex add matsumoto_r
//  2010/12/24 1.00 ServerAlias routine add matsumoto_r
//
//  2023/07/26 1.00-odp1 fix HTTP1.0/no Host Header request issue (ignore) add kwatanabe@opendoor.co.jp
// -------------------------------------------------------------------
*/

#include <libgen.h>
#include <limits.h>
#include <unistd.h>
#include <unixd.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_log.h>
#include <http_main.h>
#include <http_protocol.h>
#include <http_request.h>

#include <apr_shm.h>
#include <apr_strings.h>
#include <apr_global_mutex.h>

#define MODULE_NAME "mod_vlimit"
#define MODULE_VERSION "1.00-odp1"
#define SET_VLIMITDEFAULT 0
#define SET_VLIMITIP 1
#define SET_VLIMITFILE 2
#define IP_MAX 15
#define MAX_FILENAME 256

/* change for environment */
#define MAX_CLIENTS 512
#define VLIMIT_LOG_FILE "/tmp/mod_vlimit.log"
#define VLIMIT_LOG_FLAG_FILE "/tmp/VLIMIT_LOG"
#define VLIMIT_DEBUG_FLAG_FILE "/tmp/VLIMIT_DEBUG"
#define VLIMIT_IP_STAT_FILE "/tmp/vlimit_ip_stat.list"
#define VLIMIT_IP_STAT_FLAG_FILE "/tmp/VLIMIT_IP_STAT"
#define VLIMIT_FILE_STAT_FILE "/tmp/vlimit_file_stat.list"
#define VLIMIT_FILE_STAT_FLAG_FILE "/tmp/VLIMIT_FILE_STAT"

#ifndef MAXSYMLINKS
#define MAXSYMLINKS 256
#endif

#if (AP_SERVER_MINORVERSION_NUMBER > 2)
#define __APACHE24__
#endif

#ifdef __APACHE24__
#define remote_ip client_ip
#define unixd_set_global_mutex_perms ap_unixd_set_global_mutex_perms
#endif

module AP_MODULE_DECLARE_DATA vlimit_module;

typedef struct {
  int type;        /* max number of connections per IP */
  int ip_limit;    /* max number of connections per IP */
  int file_limit;  /* max number of connections per IP */
  int conf_id;     /* directive id */
  char *full_path; /* option target file realpath */
} vlimit_config;

typedef struct ip_data {
  char address[IP_MAX];
  int counter;
} ip_stat;

typedef struct file_data {
  char filename[MAX_FILENAME];
  int counter;
} file_stat;

typedef struct shm_data_str {
  file_stat file_stat_shm[MAX_CLIENTS];
  ip_stat ip_stat_shm[MAX_CLIENTS];
} SHM_DATA;

// shared memory
apr_shm_t *shm;
SHM_DATA *shm_base = NULL;
apr_file_t *vlimit_log_fp = NULL;
static int conf_counter = 0;

// grobal mutex
apr_global_mutex_t *vlimit_mutex;

/* --------------------------------------- */
/* --- Debug in SYSLOG Logging Routine --- */
/* --------------------------------------- */
char *vlimit_debug_log_buf = NULL;
static int VLIMIT_DEBUG_SYSLOG(const char *key, const char *msg, apr_pool_t *p)
{
  char *vlimit_buf = NULL;

  if (access(VLIMIT_DEBUG_FLAG_FILE, F_OK) == 0) {
    vlimit_buf = (char *)apr_psprintf(p, MODULE_NAME ": %s%s", key, msg);

    openlog(NULL, LOG_PID, LOG_SYSLOG);
    syslog(LOG_DEBUG, vlimit_buf);
    closelog();

    return 0;
  }

  return -1;
}

/* ----------------------------------- */
/* --- Create Share Config Routine --- */
/* ----------------------------------- */
static vlimit_config *create_share_config(apr_pool_t *p)
{
  vlimit_config *cfg = (vlimit_config *)apr_pcalloc(p, sizeof(*cfg));

  cfg->type = SET_VLIMITDEFAULT;
  cfg->ip_limit = 0;
  cfg->file_limit = 0;
  cfg->full_path = NULL;
  cfg->conf_id = conf_counter++;

  return cfg;
}

/* ------------------------------------ */
/* --- Create Server Config Routine --- */
/* ------------------------------------ */
/* Create per-server configuration structure. Used by the quick handler. */
static void *vlimit_create_server_config(apr_pool_t *p, server_rec *s)
{
  VLIMIT_DEBUG_SYSLOG("vlimit_create_server_config: ", "create server config.", p);
  return create_share_config(p);
}

/* --------------------------------- */
/* --- Create Dir Config Routine --- */
/* --------------------------------- */
/* Create per-directory configuration structure. Used by the normal handler. */
static void *vlimit_create_dir_config(apr_pool_t *p, char *path)
{
  VLIMIT_DEBUG_SYSLOG("vlimit_create_dir_config: ", "create dir config.", p);
  return create_share_config(p);
}

/* -------------- */
/* file stat data */
/* -------------- */
static int get_file_slot_id(SHM_DATA *limit_stat, request_rec *r)
{

  int i;

  for (i = 0; i < MAX_CLIENTS; i++) {
    if (strcmp(limit_stat->file_stat_shm[i].filename, basename(r->filename)) == 0) {
      return i;
    }
  }

  return -1;
}

static int get_file_empty_slot_id(SHM_DATA *limit_stat, request_rec *r)
{

  int i;

  for (i = 0; i < MAX_CLIENTS; i++) {
    if (limit_stat->file_stat_shm[i].filename[0] == '\0') {
      return i;
    }
  }

  // slot full
  return -1;
}

static int get_file_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_file_slot_id(limit_stat, r);

  if (id == -1) {
    id = get_file_empty_slot_id(limit_stat, r);
  }

  if (id >= 0) {
    return limit_stat->file_stat_shm[id].counter;
  }

  // slot full
  return -1;
}

static int inc_file_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;
  int old, new;

  id = get_file_slot_id(limit_stat, r);

  if (id == -1) {
    id = get_file_empty_slot_id(limit_stat, r);
    if (id != -1) {
      strcpy(limit_stat->file_stat_shm[id].filename, basename(r->filename));
    }
  }

  if (id >= 0) {
    while (1) {
      old = limit_stat->file_stat_shm[id].counter;
      new = old + 1;
      if (limit_stat->file_stat_shm[id].counter == old) {
        limit_stat->file_stat_shm[id].counter = new;
        break;
      }
    }
    return 0;
  }

  // slot full
  return -1;
}

static int dec_file_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;
  int old, new;

  id = get_file_slot_id(limit_stat, r);

  if (id >= 0) {
    while (1) {
      old = limit_stat->file_stat_shm[id].counter;
      new = old - 1;
      if (limit_stat->file_stat_shm[id].counter == old) {
        limit_stat->file_stat_shm[id].counter = new;
        break;
      }
    }
    return 0;
  }

  // unexpected error
  VLIMIT_DEBUG_SYSLOG("dec_file_counter: ", "unexpected error. file slot not found.", r->pool);
  return -1;
}

void unset_file_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_file_slot_id(limit_stat, r);

  if (limit_stat->file_stat_shm[id].counter == 0) {
    limit_stat->file_stat_shm[id].filename[0] = '\0';
  }
}

/* ------------ */
/* ip stat data */
/* ------------ */
static int get_ip_slot_id(SHM_DATA *limit_stat, request_rec *r)
{

  int i;

  for (i = 0; i < MAX_CLIENTS; i++) {
    if (strcmp(limit_stat->ip_stat_shm[i].address, r->connection->remote_ip) == 0) {
      return i;
    }
  }

  return -1;
}

static int get_ip_empty_slot_id(SHM_DATA *limit_stat, request_rec *r)
{

  int i;

  for (i = 0; i < MAX_CLIENTS; i++) {
    if (limit_stat->ip_stat_shm[i].address[0] == '\0') {
      return i;
    }
  }

  // slot full
  return -1;
}

static int get_ip_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_ip_slot_id(limit_stat, r);

  if (id == -1) {
    id = get_ip_empty_slot_id(limit_stat, r);
  }

  if (id >= 0) {
    return limit_stat->ip_stat_shm[id].counter;
  }

  // slot full
  return -1;
}

static int inc_ip_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_ip_slot_id(limit_stat, r);

  if (id == -1) {
    id = get_ip_empty_slot_id(limit_stat, r);
    if (id != -1) {
      strcpy(limit_stat->ip_stat_shm[id].address, r->connection->remote_ip);
    }
  }

  if (id >= 0) {
    limit_stat->ip_stat_shm[id].counter++;
    return 0;
  }

  // slot full
  return -1;
}

static int dec_ip_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_ip_slot_id(limit_stat, r);

  if (id >= 0) {
    limit_stat->ip_stat_shm[id].counter--;
    return 0;
  }

  // unexpected error
  VLIMIT_DEBUG_SYSLOG("dec_ip_counter: ", "unexpected error. ip slot not found.", r->pool);
  return -1;
}

void unset_ip_counter(SHM_DATA *limit_stat, request_rec *r)
{

  int id;

  id = get_ip_slot_id(limit_stat, r);

  if (limit_stat->ip_stat_shm[id].counter == 0) {
    limit_stat->ip_stat_shm[id].address[0] = '\0';
  }
}

static int make_ip_slot_list(SHM_DATA *limit_stat, request_rec *r)
{

  int i;
  int len;
  time_t t;
  char *log_time;
  char *vlimit_log_buf;

  if (access(VLIMIT_IP_STAT_FLAG_FILE, F_OK) == 0 && access(VLIMIT_IP_STAT_FILE, F_OK) != 0) {
    time(&t);
    log_time = (char *)ctime(&t);
    len = strlen(log_time);
    log_time[len - 1] = '\0';

    apr_file_t *vlimit_make_ip_slot_fp = NULL;

    if (apr_file_open(&vlimit_make_ip_slot_fp, VLIMIT_IP_STAT_FILE, APR_WRITE | APR_APPEND | APR_CREATE, APR_OS_DEFAULT,
                      r->pool) != APR_SUCCESS) {
      return OK;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
      if (limit_stat->ip_stat_shm[i].counter > 0) {
        vlimit_log_buf = (char *)apr_psprintf(r->pool, "[%s] slot=[%d] ipaddress=[%s] counter=[%d]\n", log_time, i,
                                              limit_stat->ip_stat_shm[i].address, limit_stat->ip_stat_shm[i].counter);
        apr_file_puts(vlimit_log_buf, vlimit_make_ip_slot_fp);
      }
    }

    apr_file_flush(vlimit_make_ip_slot_fp);

    if (apr_file_close(vlimit_make_ip_slot_fp) != APR_SUCCESS) {
      return OK;
    }

    return 0;
  }

  return -1;
}

static int make_file_slot_list(SHM_DATA *limit_stat, request_rec *r)
{

  int i;
  int len;
  time_t t;
  char *log_time;
  char *vlimit_log_buf;

  if (access(VLIMIT_FILE_STAT_FLAG_FILE, F_OK) == 0 && access(VLIMIT_FILE_STAT_FILE, F_OK) != 0) {
    time(&t);
    log_time = (char *)ctime(&t);
    len = strlen(log_time);
    log_time[len - 1] = '\0';

    apr_file_t *vlimit_make_file_slot_fp = NULL;

    if (apr_file_open(&vlimit_make_file_slot_fp, VLIMIT_FILE_STAT_FILE, APR_WRITE | APR_APPEND | APR_CREATE,
                      APR_OS_DEFAULT, r->pool) != APR_SUCCESS) {
      return OK;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
      if (limit_stat->file_stat_shm[i].counter > 0) {
        vlimit_log_buf =
            (char *)apr_psprintf(r->pool, "[%s] slot=[%d] filename=[%s] counter=[%d]\n", log_time, i,
                                 limit_stat->file_stat_shm[i].filename, limit_stat->file_stat_shm[i].counter);
        apr_file_puts(vlimit_log_buf, vlimit_make_file_slot_fp);
      }
    }

    apr_file_flush(vlimit_make_file_slot_fp);

    if (apr_file_close(vlimit_make_file_slot_fp) != APR_SUCCESS) {
      return OK;
    }

    return 0;
  }

  return -1;
}

/* ------------------------------------------- */
/* --- Request Transaction Logging Routine --- */
/* ------------------------------------------- */
static int vlimit_logging(const char *msg, request_rec *r, vlimit_config *cfg, SHM_DATA *limit_stat)
{
  int len;
  time_t t;
  char *log_time;
  char *vlimit_log_buf;

  if (access(VLIMIT_LOG_FLAG_FILE, F_OK) == 0) {
    time(&t);
    log_time = (char *)ctime(&t);
    len = strlen(log_time);
    log_time[len - 1] = '\0';

    vlimit_log_buf = (char *)apr_psprintf(r->pool, "[%s] pid=[%d] name=[%s] client=[%s] %s ip_count: %d/%d "
                                                   "file_count: %d/%d file=[%s] \n",
                                          log_time, getpid(), apr_table_get(r->headers_in, "HOST"),
                                          r->connection->remote_ip, msg, get_ip_counter(limit_stat, r), cfg->ip_limit,
                                          get_file_counter(limit_stat, r), cfg->file_limit, r->filename);

    apr_file_puts(vlimit_log_buf, vlimit_log_fp);
    apr_file_flush(vlimit_log_fp);

    return 0;
  }

  return -1;
}

static int check_virtualhost_name(request_rec *r)
{

  int i;
  const char *header_name;
  const char *alias_name;
  char *access_host;

  header_name = apr_table_get(r->headers_in, "HOST");
  if (header_name == NULL) {
    header_name = "NoHostHeader";
  }

  access_host = strtok((char *)header_name, ":");
  if (access_host == NULL) {
    access_host = (char *)header_name;
  }

  if (strcmp(access_host, r->server->server_hostname) == 0) {
    vlimit_debug_log_buf =
        apr_psprintf(r->pool, "Match: access_name=(%s) ServerName=(%s)", access_host, r->server->server_hostname);
    VLIMIT_DEBUG_SYSLOG("check_virtualhost_name: ", vlimit_debug_log_buf, r->pool);
    return 0;
  }

  if (r->server->names) {
    for (i = 0; i < r->server->names->nelts; i++) {
      alias_name = ((char **)r->server->names->elts)[i];
      vlimit_debug_log_buf = apr_psprintf(r->pool, "INFO: access_name=(%s) ServerAlias=(%s)", access_host, alias_name);
      VLIMIT_DEBUG_SYSLOG("check_virtualhost_name: ", vlimit_debug_log_buf, r->pool);
      if (strcmp(access_host, alias_name) == 0) {
        vlimit_debug_log_buf =
            apr_psprintf(r->pool, "Match: access_name=(%s) ServerAlias=(%s)", access_host, alias_name);
        VLIMIT_DEBUG_SYSLOG("check_virtualhost_name: ", vlimit_debug_log_buf, r->pool);
        return 0;
      }
    }
  }

  vlimit_debug_log_buf = apr_psprintf(r->pool, "Not Match: access_name=(%s)", access_host);
  VLIMIT_DEBUG_SYSLOG("check_virtualhost_name: ", vlimit_debug_log_buf, r->pool);

  return 1;
}

/* ------------------------------------------------- */
/* --- Check Connections from Clinets to Files  --- */
/* ------------------------------------------------- */
/* Generic function to check a request against a config. */
static int vlimit_check_limit(request_rec *r, vlimit_config *cfg)
{

  const char *header_name;
  const char *access_host;

  int ip_count = 0;
  int file_count = 0;
  int counter_stat = 0;

  if (!ap_is_initial_req(r)) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "SKIPPED: Initial Reqeusts.", r->pool);
    return DECLINED;
  }

  if (cfg->ip_limit <= 0 && cfg->file_limit <= 0) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "SKIPPED: cfg->ip_limit <= 0 && cfg->file_limit <= 0", r->pool);
    return DECLINED;
  }

  header_name = apr_table_get(r->headers_in, "HOST");
  if (header_name == NULL) {
    header_name = "NoHostHeader";
  }
  
  access_host = strtok((char *)header_name, ":");
  if (access_host == NULL) {
    access_host = (char *)header_name;
  }

  vlimit_debug_log_buf =
      apr_psprintf(r->pool, "client info: address=(%s) access_host=(%s)", r->connection->remote_ip, access_host);
  VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", vlimit_debug_log_buf, r->pool);

  SHM_DATA *limit_stat;
  limit_stat = shm_base + cfg->conf_id;

  if (make_ip_slot_list(limit_stat, r) != -1) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "make_ip_slot_list exec. create list(" VLIMIT_IP_STAT_FILE ").",
                        r->pool);
  }

  if (make_file_slot_list(limit_stat, r) != -1) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "make_file_slot_list exec. create list(" VLIMIT_FILE_STAT_FILE ").",
                        r->pool);
  }

  if (check_virtualhost_name(r)) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "access_host != server_hostname. return OK.", r->pool);
    return OK;
  }

  // vlimit_mutex lock
  VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "vlimit_mutex locked.", r->pool);
  if (apr_global_mutex_lock(vlimit_mutex) != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "vlimit_mutex lock failed.", r->pool);
    return OK;
  }

  if (cfg->file_limit > 0) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "type File: file_count++", r->pool);
    counter_stat = inc_file_counter(limit_stat, r);
    if (counter_stat == -1) {
      VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "file counter slot full. maxclients?", r->pool);
      return HTTP_SERVICE_UNAVAILABLE;
    }
    file_count = get_file_counter(limit_stat, r);
  }
  if (cfg->ip_limit > 0) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "type IP: ip_count++", r->pool);
    counter_stat = inc_ip_counter(limit_stat, r);
    if (counter_stat == -1) {
      VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "ip counter slot full. maxclients?", r->pool);
      return HTTP_SERVICE_UNAVAILABLE;
    }
    ip_count = get_ip_counter(limit_stat, r);
  }

  // vlimit_mutex unlock
  VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "vlimit_mutex unlocked.", r->pool);
  if (apr_global_mutex_unlock(vlimit_mutex) != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "vlimit_mutex unlock failed.", r->pool);
    return OK;
  }

  vlimit_debug_log_buf =
      apr_psprintf(r->pool, "conf_id: %d name: %s  uri: %s  ip_count: %d/%d file_count: %d/%d", cfg->conf_id,
                   r->server->server_hostname, r->filename, ip_count, cfg->ip_limit, file_count, cfg->file_limit);
  VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", vlimit_debug_log_buf, r->pool);

  if (cfg->ip_limit > 0 && ip_count > cfg->ip_limit) {
    vlimit_debug_log_buf = apr_psprintf(r->pool, "Rejected, too many connections from this host(%s) to the file(%s) by "
                                                 "VlimitIP[ip_limit=(%d) docroot=(%s)].",
                                        r->connection->remote_ip, access_host, cfg->ip_limit, cfg->full_path);
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", vlimit_debug_log_buf, r->pool);

    if (counter_stat != -2) {
      vlimit_logging("RESULT: 503 INC", r, cfg, limit_stat);
    }

    return HTTP_SERVICE_UNAVAILABLE;
  }
  if (cfg->file_limit > 0 && file_count > cfg->file_limit) {
    vlimit_debug_log_buf = apr_psprintf(r->pool, "Rejected, too many connections to the file(%s) by "
                                                 "VlimitFile[limit=(%d) docroot=(%s)].",
                                        access_host, cfg->file_limit, cfg->full_path);
    VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", vlimit_debug_log_buf, r->pool);

    if (counter_stat != -2) {
      vlimit_logging("RESULT: 503 INC", r, cfg, limit_stat);
    }

    return HTTP_SERVICE_UNAVAILABLE;
    // return HTTP_NOT_FOUND;
  }

  // all check passed, return response normally
  VLIMIT_DEBUG_SYSLOG("vlimit_check_limit: ", "OK: Passed all checks", r->pool);

  if (counter_stat != -2) {
    vlimit_logging("RESULT:  OK INC", r, cfg, limit_stat);
  }

  return OK;
}

/* ------------------------------------------------- */
/* --- Analyze from the Path to RealPath Routine --- */
/* ------------------------------------------------- */
static char *realpath_for_vlimit(const char *path, char *resolved_path, int maxreslth, apr_pool_t *p)
{

  int readlinks = 0;
  int n;

  char *npath;
  char link_path[PATH_MAX + 1];
  char *buf;

  buf = apr_pcalloc(p, sizeof(char *));
  npath = resolved_path;

  if (*path != '/') {
    if (!getcwd(npath, maxreslth - 2)) {
      return NULL;
    }
    npath += strlen(npath);
    if (npath[-1] != '/') {
      *npath++ = '/';
    }
  } else {
    *npath++ = '/';
    path++;
  }

  while (*path != '\0') {

    if (*path == '/') {
      path++;
      continue;
    }

    if (*path == '.' && (path[1] == '\0' || path[1] == '/')) {
      path++;
      continue;
    }

    if (*path == '.' && path[1] == '.' && (path[2] == '\0' || path[2] == '/')) {
      path += 2;
      while (npath > resolved_path + 1 && (--npath)[-1] != '/')
        ;
      continue;
    }

    while (*path != '\0' && *path != '/') {
      if (npath - resolved_path > maxreslth - 2) {
        errno = ENAMETOOLONG;
        return NULL;
      }
      *npath++ = *path++;
    }

    if (readlinks++ > MAXSYMLINKS) {
      errno = ELOOP;
      return NULL;
    }

    /* symlink analyzed */
    *npath = '\0';
    n = readlink(resolved_path, link_path, PATH_MAX);
    if (n < 0) {
      /* EINVAL means the file exists but isn't a symlink. */
      if (errno != EINVAL) {
        return NULL;
      }
    } else {
      int m;
      char *newbuf;

      link_path[n] = '\0';
      if (*link_path == '/') {
        npath = resolved_path;
      } else {
        while (*(--npath) != '/')
          ;
      }

      m = strlen(path);
      newbuf = apr_pcalloc(p, m + n + 1);
      memcpy(newbuf, link_path, n);
      memcpy(newbuf + n, path, m + 1);
      path = buf = newbuf;
    }
    *npath++ = '/';
  }

  if (npath != resolved_path + 1 && npath[-1] == '/') {
    npath--;
  }

  *npath = '\0';

  return resolved_path;
}

/* ----------------------------------------- */
/* --- Access Checker for Per Dir Config --- */
/* ----------------------------------------- */
static int vlimit_handler(request_rec *r)
{
  /* get configuration information */
  vlimit_config *cfg = (vlimit_config *)ap_get_module_config(r->per_dir_config, &vlimit_module);

  int result;
  char *real_path_dir = (char *)apr_pcalloc(r->pool, sizeof(char *) * PATH_MAX + 1);

  vlimit_debug_log_buf = apr_psprintf(r->pool, "cfg->ip_limit=(%d) cfg->file_limit=(%d) cfg->full_path=(%s)",
                                      cfg->ip_limit, cfg->file_limit, cfg->full_path);
  VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);

  /* full_path check */
  if (cfg->full_path != NULL) {
    if (access(r->filename, F_OK) != 0) {
      real_path_dir = apr_pstrdup(r->pool, r->filename);
    } else if (realpath_for_vlimit(r->filename, real_path_dir, PATH_MAX, r->pool) == NULL) {
      vlimit_debug_log_buf = apr_psprintf(r->pool, "realpath_for_vlimit was failed. path=(%s)", r->filename);
      VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);
      return DECLINED;
    }

    if (strcmp(cfg->full_path, real_path_dir) != 0) {

      vlimit_debug_log_buf = apr_psprintf(r->pool, "full_path not match cfg->full_path=(%s) <=> real_path_dir=(%s)",
                                          cfg->full_path, real_path_dir);
      VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);
      VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", "full_path not match end...", r->pool);

      return DECLINED;
    }

    vlimit_debug_log_buf = apr_psprintf(r->pool, "full_path match cfg->full_path=(%s) <=> real_path_dir=(%s)",
                                        cfg->full_path, real_path_dir);
    VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);
  } else {
    vlimit_debug_log_buf = apr_psprintf(r->pool, "full_path not found. cfg->full_path=(%s)", cfg->full_path);
    VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);
  }

  VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", "Entering normal handler", r->pool);
  result = vlimit_check_limit(r, cfg);
  VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", "Exiting normal handler", r->pool);

  return result;
}

/* -------------------------------------------- */
/* --- Access Checker for Per Server Config --- */
/* -------------------------------------------- */
/* For server configration */
static int vlimit_quick_handler(request_rec *r, int lookup)
{
  vlimit_config *cfg = (vlimit_config *)ap_get_module_config(r->server->module_config, &vlimit_module);

  int result;
  char *real_path_dir = (char *)apr_pcalloc(r->pool, sizeof(char *) * PATH_MAX + 1);

  /* full_path check */
  if (cfg->full_path != NULL) {

    if (access(r->filename, F_OK) != 0) {
      real_path_dir = apr_pstrdup(r->pool, r->filename);
    } else if (realpath_for_vlimit(r->filename, real_path_dir, PATH_MAX, r->pool) == NULL) {
      vlimit_debug_log_buf = apr_psprintf(r->pool, "realpath_for_vlimit was failed. path=(%s)", r->filename);
      VLIMIT_DEBUG_SYSLOG("vlimit_handler: ", vlimit_debug_log_buf, r->pool);
      return DECLINED;
    }

    if (strcmp(cfg->full_path, real_path_dir) != 0) {

      vlimit_debug_log_buf = apr_psprintf(r->pool, "full_path not match cfg->full_path=(%s) <=> real_path_dir=(%s)",
                                          cfg->full_path, real_path_dir);
      VLIMIT_DEBUG_SYSLOG("vlimit_quick_handler: ", vlimit_debug_log_buf, r->pool);
      VLIMIT_DEBUG_SYSLOG("vlimit_quick_handler: ", "mod_vlimit: vlimit_quick_handler: full_path not match end...",
                          r->pool);

      return DECLINED;
    }

    vlimit_debug_log_buf = apr_psprintf(r->pool, "full_path match cfg->full_path=(%s) <=> real_path_dir=(%s)",
                                        cfg->full_path, real_path_dir);
    VLIMIT_DEBUG_SYSLOG("vlimit_quick_handler: ", vlimit_debug_log_buf, r->pool);
  }

  VLIMIT_DEBUG_SYSLOG("vlimit_quick_handler: ", "mod_vlimit: Entering quick handler", r->pool);
  result = vlimit_check_limit(r, cfg);
  VLIMIT_DEBUG_SYSLOG("vlimit_quick_handler: ", "mod_vlimit: Entering quick handler", r->pool);

  return result;
}

/* ------------------------------------ */
/* --- Command_rec for VlimitIP--- */
/* ------------------------------------ */
/* Parse the VlimitIP directive */
static const char *set_vlimitip(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg_opt1)
{
  vlimit_config *cfg = (vlimit_config *)mconfig;
  vlimit_config *scfg = (vlimit_config *)ap_get_module_config(parms->server->module_config, &vlimit_module);

  signed long int limit = strtol(arg1, (char **)NULL, 10);

  /* No reasonable person would want more than 2^16. Better would be
     to use LONG_MAX but that causes portability problems on win32 */
  if ((limit > 65535) || (limit < 0)) {
    return "Integer overflow or invalid number";
  }

  if (parms->path != NULL) {
    /* Per-directory context */
    cfg->type = SET_VLIMITIP;
    cfg->ip_limit = limit;
    cfg->full_path = apr_pstrdup(parms->pool, arg_opt1);
  } else {
    /* Per-server context */
    scfg->type = SET_VLIMITIP;
    scfg->ip_limit = limit;
    scfg->full_path = apr_pstrdup(parms->pool, arg_opt1);
  }

  return NULL;
}

/* --------------------------------------- */
/* --- Command_rec for VlimitFile--- */
/* --------------------------------------- */
/* Parse the VlimitFile directive */
static const char *set_vlimitfile(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg_opt1)
{
  vlimit_config *cfg = (vlimit_config *)mconfig;
  vlimit_config *scfg = (vlimit_config *)ap_get_module_config(parms->server->module_config, &vlimit_module);

  signed long int limit = strtol(arg1, (char **)NULL, 10);

  /* No reasonable person would want more than 2^16. Better would be
     to use LONG_MAX but that causes portability problems on win32 */
  if ((limit > 65535) || (limit < 0)) {
    return "Integer overflow or invalid number";
  }

  if (parms->path != NULL) {
    /* Per-directory context */
    cfg->type = SET_VLIMITFILE;
    cfg->file_limit = limit;
    cfg->full_path = apr_pstrdup(parms->pool, arg_opt1);
  } else {
    /* Per-server context */
    scfg->type = SET_VLIMITFILE;
    scfg->file_limit = limit;
    scfg->full_path = apr_pstrdup(parms->pool, arg_opt1);
  }

  return NULL;
}

/* ------------------------ */
/* --- Command_rec Array--- */
/* ------------------------ */
static command_rec vlimit_cmds[] = {
    AP_INIT_TAKE12("VlimitIP", set_vlimitip, NULL, OR_LIMIT | RSRC_CONF,
                   "maximum connections per IP address to DocumentRoot"),
    AP_INIT_TAKE12("VlimitFile", set_vlimitfile, NULL, OR_LIMIT | RSRC_CONF,
                   "maximum connections per File to DocumentRoot"),
    {NULL},
};

/* ------------------------------------------- */
/* --- Init Routine or ap_hook_post_config --- */
/* ------------------------------------------- */
/* Set up startup-time initialization */
static int vlimit_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
  VLIMIT_DEBUG_SYSLOG("vlimit_init: ", MODULE_NAME " " MODULE_VERSION " started.", p);

  if (apr_file_open(&vlimit_log_fp, VLIMIT_LOG_FILE, APR_WRITE | APR_APPEND | APR_CREATE, APR_OS_DEFAULT, p) !=
      APR_SUCCESS) {
    return OK;
  }

  apr_status_t status;
  apr_size_t retsize;
  apr_size_t shm_size;
  int t;

  SHM_DATA *shm_data = NULL;

  shm_size = (apr_size_t)(sizeof(shm_data) + sizeof(shm_data->file_stat_shm) + sizeof(shm_data->ip_stat_shm)) *
             (conf_counter + 1);

  // Create global mutex
  status = apr_global_mutex_create(&vlimit_mutex, NULL, APR_LOCK_DEFAULT, p);
  if (status != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Error creating global mutex.", p);
    return status;
  }
#ifdef AP_NEED_SET_MUTEX_PERMS
  status = unixd_set_global_mutex_perms(vlimit_mutex);
  if (status != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Error xrent could not set permissions on global mutex.", p);
    return status;
  }
#endif

  /* If there was a memory block already assigned.. destroy it */
  if (shm) {
    status = apr_shm_destroy(shm);
    if (status != APR_SUCCESS) {
      VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Couldn't destroy old memory block", p);
      return status;
    } else {
      VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Old Shared memory block, destroyed.", p);
    }
  }

  /* Create shared memory block */
  status = apr_shm_create(&shm, shm_size, NULL, p);
  if (status != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Error creating shm block", p);
    return status;
  }

  /* Check size of shared memory block */
  retsize = apr_shm_size_get(shm);
  if (retsize != shm_size) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init: ", "Error allocating shared memory block", p);
    return status;
  }
  /* Init shm block */
  shm_base = apr_shm_baseaddr_get(shm);
  if (shm_base == NULL) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init", "Error creating status block.", p);
    return status;
  }
  memset(shm_base, 0, retsize);

  vlimit_debug_log_buf = apr_psprintf(
      p, "Memory Allocated %d bytes (each conf takes %d bytes) MaxClient:%d", (int)retsize,
      (int)(sizeof(shm_data) + sizeof(shm_data->file_stat_shm) + sizeof(shm_data->ip_stat_shm)), MAX_CLIENTS);
  VLIMIT_DEBUG_SYSLOG("vlimit_init: ", vlimit_debug_log_buf, p);

  if (retsize < (sizeof(shm_data) * conf_counter)) {
    VLIMIT_DEBUG_SYSLOG("vlimit_init ", "Not enough memory allocated!! Giving up", p);
    return HTTP_INTERNAL_SERVER_ERROR;
  }

  int i;

  for (t = 0; t <= conf_counter; t++) {
    shm_data = shm_base + t;
    for (i = 0; i < MAX_CLIENTS; i++) {
      shm_data->file_stat_shm[i].filename[0] = '\0';
      shm_data->ip_stat_shm[i].address[0] = '\0';
      shm_data->file_stat_shm[i].counter = 0;
      shm_data->ip_stat_shm[i].counter = 0;
    }
  }

  vlimit_debug_log_buf =
      apr_psprintf(p, "%s Version %s - Initialized [%d Conf]", MODULE_NAME, MODULE_VERSION, conf_counter);
  VLIMIT_DEBUG_SYSLOG("vlimit_init: ", vlimit_debug_log_buf, p);

  return OK;
}

static void vlimit_child_init(apr_pool_t *p, server_rec *server)
{
  if (apr_global_mutex_child_init(&vlimit_mutex, NULL, p)) {
    VLIMIT_DEBUG_SYSLOG("vlimit_child_init: ", "global mutex attached.", p);
  }
  if (apr_shm_attach(&shm, NULL, p) == APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_child_init: ", "global shared memory attached.", p);
  } else {
    VLIMIT_DEBUG_SYSLOG("vlimit_child_init: ", "global shared memory can't be attached.", p);
  }
}

static int vlimit_response_end(request_rec *r)
{

  int counter_stat = -2;

  VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "start", r->pool);

  vlimit_config *cfg = (vlimit_config *)ap_get_module_config(r->per_dir_config, &vlimit_module);

  if (check_virtualhost_name(r)) {
    VLIMIT_DEBUG_SYSLOG(__func__, ": access_host != server_hostname. return OK.", r->pool);
    VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "end", r->pool);
    return OK;
  }

  SHM_DATA *limit_stat;
  limit_stat = shm_base + cfg->conf_id;

  // vlimit_mutex lock
  VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "vlimit_mutex locked.", r->pool);
  if (apr_global_mutex_lock(vlimit_mutex) != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "vlimit_mutex lock failed.", r->pool);
    return OK;
  }

  if (cfg->file_limit > 0) {
    VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "type FILE: file_count--", r->pool);
    if (get_file_counter(limit_stat, r) > 0) {
      counter_stat = dec_file_counter(limit_stat, r);
    }
    if (get_file_counter(limit_stat, r) == 0) {
      unset_file_counter(limit_stat, r);
    }
  }
  if (cfg->ip_limit > 0) {

    VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "type IP: ip_count--", r->pool);
    if (get_ip_counter(limit_stat, r) > 0) {
      counter_stat = dec_ip_counter(limit_stat, r);
    }
    if (get_ip_counter(limit_stat, r) == 0) {
      unset_ip_counter(limit_stat, r);
    }
  }

  // when decrement counter, write log
  if (counter_stat != -2) {
    vlimit_logging("RESULT: END DEC", r, cfg, limit_stat);
  }

  // vlimit_mutex unlock
  VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "vlimit_mutex unlocked.", r->pool);
  if (apr_global_mutex_unlock(vlimit_mutex) != APR_SUCCESS) {
    VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "vlimit_mutex unlock failed.", r->pool);
    return OK;
  }

  vlimit_debug_log_buf =
      apr_psprintf(r->pool, "conf_id: %d name: %s  uri: %s ip_count: %d/%d file_count: %d/%d", cfg->conf_id,
                   r->server->server_hostname, r->filename, get_ip_counter(limit_stat, r), cfg->ip_limit,
                   get_file_counter(limit_stat, r), cfg->file_limit);
  VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", vlimit_debug_log_buf, r->pool);
  VLIMIT_DEBUG_SYSLOG("vlimit_response_end: ", "end", r->pool);
  return OK;
}

/* ---------------------- */
/* --- Register_hooks --- */
/* ---------------------- */
static void vlimit_register_hooks(apr_pool_t *p)
{
  // static const char * const after_me[] = { "mod_cache.c", NULL };
  ap_hook_post_config(vlimit_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(vlimit_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  // ap_hook_quick_handler(vlimit_quick_handler, NULL, after_me, APR_HOOK_FIRST);
  ap_hook_fixups(vlimit_handler, NULL, NULL, APR_HOOK_LAST);
  ap_hook_log_transaction(vlimit_response_end, NULL, NULL, APR_HOOK_FIRST);
}

/* ------------------------------ */
/* --- Module Functions Array --- */
/* ------------------------------ */
module AP_MODULE_DECLARE_DATA vlimit_module = {STANDARD20_MODULE_STUFF,
                                               vlimit_create_dir_config,    /* create per-dir config structures   */
                                               NULL,                        /* merge  per-dir  config structures  */
                                               vlimit_create_server_config, /* create per-server config structures  */
                                               NULL,                        /* merge  per-server config structures  */
                                               vlimit_cmds,                 /* table of config file commands    */
                                               vlimit_register_hooks};
