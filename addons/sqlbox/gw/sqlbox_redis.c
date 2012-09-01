#include "gwlib/gwlib.h"
#ifdef HAVE_REDIS
#include "gwlib/dbpool.h"
#include "hiredis.h"
#define sqlbox_redis_c
#include "sqlbox_redis.h"
#include "jansson.h"

#define sql_update redis_update
#define sql_select redis_select

static Octstr *sqlbox_logtable;
static Octstr *sqlbox_insert_table;
static Octstr *sqlbox_inflight_table;

/*
 * Our connection pool to redis.
 */

static DBPool *pool = NULL;

static void redis_update(const Octstr *sql, const Octstr *data)
{
    redisReply *reply;
    DBPoolConn *pc;

#if defined(SQLBOX_TRACE)
     debug("SQLBOX", 0, "sql: %s", octstr_get_cstr(sql));
     debug("SQLBOX", 0, "data: %s", octstr_get_cstr(data));
#endif

    pc = dbpool_conn_consume(pool);
    if (pc == NULL) {
        error(0, "REDIS: Database pool got no connection! DB update failed!");
        return;
    }

    if(data == NULL)
        reply = redisCommand(pc->conn, octstr_get_cstr(sql));
    else
        reply = redisCommand(pc->conn, octstr_get_cstr(sql), octstr_get_cstr(data));
    if (reply->type == REDIS_REPLY_ERROR)
        error(0, "REDIS: redisCommand() failed: %s", reply->str);

    freeReplyObject(reply);
    dbpool_conn_produce(pc);
}

static redisReply* redis_select(const Octstr *sql)
{
    redisReply *reply;
    DBPoolConn *pc;

#if defined(SQLBOX_TRACE)
    debug("SQLBOX", 0, "sql: %s", octstr_get_cstr(sql));
#endif

    pc = dbpool_conn_consume(pool);
    if (pc == NULL) {
        error(0, "REDIS: Database pool got no connection! DB update failed!");
        return NULL;
    }

    reply = redisCommand(pc->conn, octstr_get_cstr(sql));
    if (reply->type == REDIS_REPLY_ERROR)
        error(0, "REDIS: %s", reply->str);

    dbpool_conn_produce(pc);

    return reply;
}

static void *gw_malloc_json(size_t size)
{
    return gw_malloc(size);
}

static void gw_free_json(void *ptr)
{
    return gw_free(ptr);
}

void sqlbox_configure_redis(Cfg* cfg)
{
    CfgGroup *grp;
    Octstr *sql;

    if (!(grp = cfg_get_single_group(cfg, octstr_imm("sqlbox"))))
        panic(0, "SQLBOX: Redis: group 'sqlbox' is not specified!");

    sqlbox_logtable = cfg_get(grp, octstr_imm("sql-log-table"));
    if (sqlbox_logtable == NULL) {
        panic(0, "'sql-log-table' is not configured in the group 'sqlbox'.");
    }
    sqlbox_insert_table = cfg_get(grp, octstr_imm("sql-insert-table"));
    if (sqlbox_insert_table == NULL) {
        panic(0, "'sql-insert-table' is not configured in the group 'sqlbox'.");
    }
    sqlbox_inflight_table = cfg_get(grp, octstr_imm("sql-inflight-table"));

    json_set_alloc_funcs(gw_malloc_json, gw_free_json);

    /* no need to create tables on redis */
}

#define octstr_null_create(x) ((x != NULL) ? octstr_create(x) : octstr_create(""))
#define atol_null(x) ((x != NULL) ? atol(x) : -1)
Msg *redis_fetch_msg()
{
    Msg *msg = NULL;
    Octstr *sql, *delet, *subst, *jsonstr;
    redisReply *res = NULL;
    char *resjson;
    json_t *root;
    json_t *jsonmsg;
    json_error_t *error = NULL;

    if (sqlbox_inflight_table != NULL)
        sql = octstr_format(SQLBOX_REDIS_QUEUE_POP_WITH_INFLIGHT, sqlbox_insert_table, sqlbox_inflight_table);
    else
        sql = octstr_format(SQLBOX_REDIS_QUEUE_POP, sqlbox_insert_table);
    res = redis_select(sql);

    if (res->type == REDIS_REPLY_ARRAY) {
        resjson = res->element[1]->str; /* In-flight usage (BRPOPLPUSH) returns this */
    }
    else if (res->type == REDIS_REPLY_STRING) { /* Non in-flight usage (BRPOP) returns this */
        resjson = res->str;
    }
    else if (res->type == REDIS_REPLY_NIL) { /* No messages queued - loop */
        freeReplyObject(res);
        return NULL;
     }
    else if (res->type == REDIS_REPLY_ERROR) {
        warning(0, "REDIS command %s failed with error %s", octstr_get_cstr(sql), res->str);
        freeReplyObject(res);
        return NULL;
    }
    else
    {
        warning(0, "REDIS command %s return unknown status", octstr_get_cstr(sql));
        freeReplyObject(res);
        return NULL;
    }

    root = json_loads(resjson, 0, error);
    if (!root) {
        warning(0, "sqlbox: Invalid JSON in message retrieved from Redis. Skipping message.");
        freeReplyObject(res);
        return NULL;
    }

    jsonmsg = json_object_get(root, "msg");
    if (!json_is_object(jsonmsg)) {
        warning(0, "sqlbox: JSON does not include 'msg' root element. Skipping message.");
        json_decref(root);
        freeReplyObject(res);
        return NULL;
    }

    /* save fields in this row as msg struct */
    msg = msg_create(sms);
    msg->sms.sender     = octstr_null_create(json_string_value(json_object_get(jsonmsg,"sender")));
    msg->sms.receiver   = octstr_null_create(json_string_value(json_object_get(jsonmsg,"receiver")));
    msg->sms.udhdata    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"udhdata")));
    msg->sms.msgdata    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"msgdata")));
    msg->sms.time       = atol_null(json_string_value(json_object_get(jsonmsg,"time")));
    msg->sms.smsc_id    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"smsc_id")));
    msg->sms.service    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"service")));
    msg->sms.account    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"account")));
    msg->sms.sms_type   = atol_null(json_string_value(json_object_get(jsonmsg,"sms_type")));
    msg->sms.mclass     = atol_null(json_string_value(json_object_get(jsonmsg,"mclass")));
    msg->sms.mwi        = atol_null(json_string_value(json_object_get(jsonmsg,"mwi")));
    msg->sms.coding     = atol_null(json_string_value(json_object_get(jsonmsg,"coding")));
    msg->sms.compress   = atol_null(json_string_value(json_object_get(jsonmsg,"compress")));
    msg->sms.validity   = atol_null(json_string_value(json_object_get(jsonmsg,"validity")));
    msg->sms.deferred   = atol_null(json_string_value(json_object_get(jsonmsg,"deferred")));
    msg->sms.dlr_mask   = atol_null(json_string_value(json_object_get(jsonmsg,"dlr_mask")));
    msg->sms.dlr_url    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"dlr_url")));
    msg->sms.pid        = atol_null(json_string_value(json_object_get(jsonmsg,"pid")));
    msg->sms.alt_dcs    = atol_null(json_string_value(json_object_get(jsonmsg,"alt_dcs")));
    msg->sms.rpi        = atol_null(json_string_value(json_object_get(jsonmsg,"rpi")));
    msg->sms.charset    = octstr_null_create(json_string_value(json_object_get(jsonmsg,"charset")));
    msg->sms.binfo      = octstr_null_create(json_string_value(json_object_get(jsonmsg,"binfo")));
    msg->sms.priority   = atol_null(json_string_value(json_object_get(jsonmsg,"priority")));
    msg->sms.meta_data  = octstr_null_create(json_string_value(json_object_get(jsonmsg,"meta_data")));
    if (json_string_value(json_object_get(jsonmsg,"boxc_id")) == NULL) {
        msg->sms.boxc_id= octstr_duplicate(sqlbox_id);
    }
    else {
        msg->sms.boxc_id= octstr_null_create(json_string_value(json_object_get(jsonmsg,"boxc_id")));
    }
    /* delete from inflight table. This shoudl really be done after the message has been queued to bearerbox */
    if (sqlbox_inflight_table != NULL) {
        subst = octstr_create("%s");
        delet = octstr_format(SQLBOX_REDIS_DELETE, sqlbox_inflight_table, subst);
        jsonstr = octstr_create(resjson);
        sql_update(delet, jsonstr);
        octstr_destroy(delet);
        octstr_destroy(jsonstr);
    }

    json_decref(root);
    freeReplyObject(res);
    octstr_destroy(sql);
    return msg;
}


static Octstr *get_numeric_value_or_return_null(long int num)
{
    if (num == -1) {
        return octstr_create("NULL");
    }
    return octstr_format("%ld", num);
}

static Octstr *get_string_value_or_return_null(Octstr *str)
{
    if (str == NULL) {
        return octstr_create("NULL");
    }
    if (octstr_compare(str, octstr_imm("")) == 0) {
        return octstr_create("NULL");
    }
    octstr_replace(str, octstr_imm("\\"), octstr_imm("\\\\"));
    octstr_replace(str, octstr_imm("\'"), octstr_imm("\\\'"));
    return octstr_format("%S", str);
}

#define st_num(x) (stuffer[stuffcount++] = get_numeric_value_or_return_null(x))
#define st_str(x) (stuffer[stuffcount++] = get_string_value_or_return_null(x))

void redis_save_msg(Msg *msg, Octstr *momt /*, Octstr smsbox_id */)
{
    Octstr *sql, *jsonstr, *subst;
    Octstr *stuffer[30];
    json_t *msgjson, *root;
    char *json;
    int stuffcount = 0;
    
    msgjson = json_object();
   
    json_object_set_new(msgjson, "momt", json_string(octstr_get_cstr(st_str(momt))));
    json_object_set_new(msgjson, "sender", json_string(octstr_get_cstr(st_str(msg->sms.sender))));
    json_object_set_new(msgjson, "receiver", json_string(octstr_get_cstr(st_str(msg->sms.receiver))));
    json_object_set_new(msgjson, "foreign_id", json_string(octstr_get_cstr(st_str(msg->sms.foreign_id))));
    json_object_set_new(msgjson, "udhdata", json_string(octstr_get_cstr(st_str(msg->sms.udhdata))));
    json_object_set_new(msgjson, "msgdata", json_string(octstr_get_cstr(st_str(msg->sms.msgdata))));
    json_object_set_new(msgjson, "time", json_string(octstr_get_cstr(st_num(msg->sms.time))));
    json_object_set_new(msgjson, "smsc_id", json_string(octstr_get_cstr(st_str(msg->sms.smsc_id))));
    json_object_set_new(msgjson, "service", json_string(octstr_get_cstr(st_str(msg->sms.service))));
    json_object_set_new(msgjson, "account", json_string(octstr_get_cstr(st_str(msg->sms.account))));
    json_object_set_new(msgjson, "sms_type", json_string(octstr_get_cstr(st_num(msg->sms.sms_type))));
    json_object_set_new(msgjson, "mclass", json_string(octstr_get_cstr(st_num(msg->sms.mclass))));
    json_object_set_new(msgjson, "mwi", json_string(octstr_get_cstr(st_num(msg->sms.mwi))));
    json_object_set_new(msgjson, "coding", json_string(octstr_get_cstr(st_num(msg->sms.coding))));
    json_object_set_new(msgjson, "compress", json_string(octstr_get_cstr(st_num(msg->sms.compress))));
    json_object_set_new(msgjson, "validity", json_string(octstr_get_cstr(st_num(msg->sms.validity))));
    json_object_set_new(msgjson, "deferred", json_string(octstr_get_cstr(st_num(msg->sms.deferred))));
    json_object_set_new(msgjson, "dlr_mask", json_string(octstr_get_cstr(st_num(msg->sms.dlr_mask))));
    json_object_set_new(msgjson, "dlr_url", json_string(octstr_get_cstr(st_str(msg->sms.dlr_url))));
    json_object_set_new(msgjson, "pid", json_string(octstr_get_cstr(st_num(msg->sms.pid))));
    json_object_set_new(msgjson, "alt_dcs", json_string(octstr_get_cstr(st_num(msg->sms.alt_dcs))));
    json_object_set_new(msgjson, "rpi", json_string(octstr_get_cstr(st_num(msg->sms.rpi))));
    json_object_set_new(msgjson, "charset", json_string(octstr_get_cstr(st_str(msg->sms.charset))));
    json_object_set_new(msgjson, "boxc_id", json_string(octstr_get_cstr(st_str(msg->sms.boxc_id))));
    json_object_set_new(msgjson, "binfo", json_string(octstr_get_cstr(st_str(msg->sms.binfo))));
    json_object_set_new(msgjson, "priority", json_string(octstr_get_cstr(st_num(msg->sms.priority))));
    json_object_set_new(msgjson, "meta_data", json_string(octstr_get_cstr(st_str(msg->sms.meta_data))));

    root = json_object();
    json_object_set(root, "msg", msgjson);
    json = json_dumps(root, JSON_COMPACT);
    jsonstr = octstr_create(json);

    subst = octstr_create("%s");

    sql = octstr_format(SQLBOX_REDIS_QUEUE_PUSH, sqlbox_logtable, subst);
    sql_update(sql, jsonstr);
    octstr_destroy(sql);
    octstr_destroy(subst);
    octstr_destroy(jsonstr);

    while (stuffcount > 0) {
        octstr_destroy(stuffer[--stuffcount]);
    }

    json_decref(msgjson);
    json_decref(root);
    gw_free(json);
}

void redis_leave()
{
    dbpool_destroy(pool);
}

struct server_type *sqlbox_init_redis(Cfg* cfg)
{
    CfgGroup *grp;
    List *grplist;
    Octstr *redis_host, *redis_password, *redis_id;
    Octstr *p = NULL;
    long pool_size, redis_port = 0, redis_database = -1, redis_idle_timeout_secs = -1;
    DBConf *db_conf = NULL;
    struct server_type *res = NULL;

    /*
     * check for all mandatory directives that specify the key names 
     * for the redis storage
     */
    if (!(grp = cfg_get_single_group(cfg, octstr_imm("sqlbox"))))
        panic(0, "SQLBOX: Redis: group 'sqlbox' is not specified!");

    if (!(redis_id = cfg_get(grp, octstr_imm("id"))))
        panic(0, "SQLBOX: Redis: directive 'id' is not specified in the 'sqlbox' group!");

    /*
     * now grap the required information from the 'redis-connection' group
     * with the redis-id we just obtained
     *
     * we have to loop through all available Redis connection definitions
     * and search for the one we are looking for
     */

     grplist = cfg_get_multi_group(cfg, octstr_imm("redis-connection"));
     while (grplist && (grp = (CfgGroup *)gwlist_extract_first(grplist)) != NULL) {
         p = cfg_get(grp, octstr_imm("id"));
         if (p != NULL && octstr_compare(p, redis_id) == 0) {
             goto found;
         }
         if (p != NULL) octstr_destroy(p);
     }
     panic(0, "SQLBOX: Redis: connection settings for id '%s' are not specified!",
         octstr_get_cstr(redis_id));

found:
    octstr_destroy(p);
    gwlist_destroy(grplist, NULL);

    if (cfg_get_integer(&pool_size, grp, octstr_imm("max-connections")) == -1 || pool_size == 0)
        pool_size = 1;

    if (!(redis_host = cfg_get(grp, octstr_imm("host"))))
        panic(0, "SQLBOX: Redis: directive 'host' is not specified!");
    if (cfg_get_integer(&redis_port, grp, octstr_imm("port")) == -1)
        panic(0, "SQLBOX: Redis: directive 'port' is not specified!");
    redis_password = cfg_get(grp, octstr_imm("password"));
    cfg_get_integer(&redis_database, grp, octstr_imm("database"));
    cfg_get_integer(&redis_idle_timeout_secs, grp, octstr_imm("idle-timeout-secs"));

    /*
     * ok, ready to connect to Redis
     */
    db_conf = gw_malloc(sizeof(DBConf));
    gw_assert(db_conf != NULL);

    db_conf->redis = gw_malloc(sizeof(RedisConf));
    gw_assert(db_conf->redis != NULL);

    db_conf->redis->host = redis_host;
    db_conf->redis->port = redis_port;
    db_conf->redis->password = redis_password;
    db_conf->redis->database = redis_database;
    db_conf->redis->idle_timeout_secs = redis_idle_timeout_secs;

    pool = dbpool_create(DBPOOL_REDIS, db_conf, pool_size);
    gw_assert(pool != NULL);

    /*
     * XXX should a failing connect throw panic?!
     */
    if (dbpool_conn_count(pool) == 0)
        panic(0,"SQLBOX: Redis: database pool has no connections!");

    octstr_destroy(redis_id);

    res = gw_malloc(sizeof(struct server_type));
    gw_assert(res != NULL);

    res->type = octstr_create("Redis");
    res->sql_enter = sqlbox_configure_redis;
    res->sql_leave = redis_leave;
    res->sql_fetch_msg = redis_fetch_msg;
    res->sql_save_msg = redis_save_msg;
    return res;
}
#endif
