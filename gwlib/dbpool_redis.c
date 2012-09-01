/* ==================================================================== 
 * The Kannel Software License, Version 1.0 
 * 
 * Copyright (c) 2001-2010 Kannel Group  
 * Copyright (c) 1998-2001 WapIT Ltd.   
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 * 
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in 
 *    the documentation and/or other materials provided with the 
 *    distribution. 
 * 
 * 3. The end-user documentation included with the redistribution, 
 *    if any, must include the following acknowledgment: 
 *       "This product includes software developed by the 
 *        Kannel Group (http://www.kannel.org/)." 
 *    Alternately, this acknowledgment may appear in the software itself, 
 *    if and wherever such third-party acknowledgments normally appear. 
 * 
 * 4. The names "Kannel" and "Kannel Group" must not be used to 
 *    endorse or promote products derived from this software without 
 *    prior written permission. For written permission, please  
 *    contact org@kannel.org. 
 * 
 * 5. Products derived from this software may not be called "Kannel", 
 *    nor may "Kannel" appear in their name, without prior written 
 *    permission of the Kannel Group. 
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED.  IN NO EVENT SHALL THE KANNEL GROUP OR ITS CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,  
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT  
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR  
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,  
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE  
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,  
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 * ==================================================================== 
 * 
 * This software consists of voluntary contributions made by many 
 * individuals on behalf of the Kannel Group.  For more information on  
 * the Kannel Group, please see <http://www.kannel.org/>. 
 * 
 * Portions of this software are based upon software originally written at  
 * WapIT Ltd., Helsinki, Finland for the Kannel project.  
 */ 

/*
 * dbpool_redis.c - implement REDIS operations for generic database connection pool
 *
 * Toby Phipps <toby.phipps@nexmedia.com.sg>
 *      2011 Initial version.
 */

#ifdef HAVE_REDIS
#include <hiredis.h>

static void *redis_open_conn(const DBConf *db_conf)
{
    redisContext *redis = NULL;
    RedisConf *conf = db_conf->redis; /* make compiler happy */
    redisReply *reply = NULL;
    char *replyTok;
    char *redisVersion;

    /* sanity check */
    if (conf == NULL)
        return NULL;

    /* pre-allocate */
    redis = gw_malloc(sizeof(redisContext));
    gw_assert(redis != NULL);

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    redis = redisConnectWithTimeout(octstr_get_cstr(conf->host),
                                   conf->port,
                                   timeout);
    if(redis->err) {
        error(0, "REDIS: can not connect to server!");
        error(0, "REDIS: %s", redis->errstr);
        goto failed;
    }

    info(0, "REDIS: Connected to server at %s:%ld.", octstr_get_cstr(conf->host),
                                                    conf->port);

    if(conf->password != NULL) {
       reply = redisCommand(redis,"AUTH %s",octstr_get_cstr(conf->password));
       if(strncmp("OK",reply->str,2) != 0) {
             error(0, "REDIS: Password authentication failed!");
             goto failed;
       }
       freeReplyObject(reply);
    }

    if (conf->idle_timeout_secs != -1) {
        reply = redisCommand(redis,"CONFIG SET TIMEOUT %ld",conf->idle_timeout_secs);
        if (strncmp("OK",reply->str,2) !=0 )
            warning(0,"REDIS: CONFIG SET TIMEOUT %ld failed - could not set timeout",conf->idle_timeout_secs);
        else
            info(0,"REDIS: Set idle timeout to %ld seconds",conf->idle_timeout_secs);
        freeReplyObject(reply);
    }

    if (conf->database != -1) {
        reply = redisCommand(redis,"SELECT %ld",conf->database);
        if (strncmp("OK",reply->str,2) !=0 )
            error(0,"REDIS: SELECT %ld failed - could not select database",conf->database);
        else
            info(0,"REDIS: Selected database %ld",conf->database);
        freeReplyObject(reply);
    }

    reply = redisCommand(redis,"INFO");
    if(reply->type != REDIS_REPLY_STRING) {
         error(0, "REDIS: INFO command to get version failed!");
         goto failed;
    }
    replyTok = strtok(reply->str, ":");
    while(replyTok!=NULL)
    {
      if(strcmp("redis_version",replyTok)==0) {
         replyTok = strtok(NULL, ":\r");
         break;
      }
      replyTok = strtok(NULL, ":\r");
    }

    if(replyTok == NULL) {
        error(0, "REDIS: Could not parse version from INFO output!");
        goto failed;
    }

    info(0, "REDIS: server version %s.", replyTok);

    freeReplyObject(reply);
    return redis;

failed:
    if (reply != NULL)
        freeReplyObject(reply);
    if (redis != NULL) {
        redisFree(redis);
    }
    return NULL;
}


static void redis_close_conn(void *conn)
{
    if (conn == NULL)
        return;

    redisFree((redisContext*) conn);
}


static int redis_check_conn(void *conn)
{
    redisReply *reply;

    if (conn == NULL)
        return -1;

    reply = redisCommand(conn,"PING");
    if (reply != NULL) {
        if (strcmp(reply->str,"PONG")==0) {
            freeReplyObject(reply);
            return 0;
        }
    }

    error(0, "REDIS: server connection check failed!");
    error(0, "REDIS: %s", ((redisContext*)conn)->errstr);
    if (reply != NULL)
        freeReplyObject(reply);
    return -1;
}


static int redis_select(void *conn, Octstr *sql, List *binds, List **res)
{
    redisReply *reply;
    int ret;
    long i, binds_len;
    int got_reply = 0;
    List *row;

    /* bind params if any */
    binds_len = gwlist_len(binds);
    if (binds_len > 0) {
        for (i = 0; i < binds_len; i++) {
            Octstr *str = gwlist_get(binds, i);
            if(octstr_len(str) > 0)
                octstr_replace_first(sql,octstr_imm("?"),str);
            else
                octstr_replace_first(sql,octstr_imm("?"),octstr_imm("_NULL_"));
        }
    }

    debug("dbpool.redis",0,"%s",octstr_get_cstr(sql));
    /* execute statement */
    reply = redisCommand(conn, octstr_get_cstr(sql));
    if (reply->type == REDIS_REPLY_ERROR) {
        error(0, "REDIS: redisCommand() failed: `%s'", reply->str);
        freeReplyObject(reply);
        return -1;
    }

    Octstr *temp = NULL;

    switch(reply->type) {
        case REDIS_REPLY_NIL:
            debug("dbpool.redis",0,"Received REDIS_REPLY_NIL");
            break;

        case REDIS_REPLY_STATUS:
            debug("dbpool.redis",0,"Received REDIS_REPLY_STATUS");
        case REDIS_REPLY_STRING:
            debug("dbpool.redis",0,"Received REDIS_REPLY_STRING");
            *res = gwlist_create();
            row = gwlist_create();
            temp = octstr_create_from_data(reply->str, reply->len);
            gwlist_append(row, temp);
            got_reply = 1;
            //octstr_destroy(temp);
            break;

        case REDIS_REPLY_ARRAY:
            debug("dbpool.redis",0,"Received REDIS_REPLY_ARRAY");
            *res = gwlist_create();
            row = gwlist_create();
            for (i=0; i < reply->elements; i++) {
                if (reply->element[i]->type==REDIS_REPLY_NIL ||
                    reply->element[i]->str == NULL || 
                    reply->element[i]->len == 0) {
                    gwlist_produce(row, octstr_imm(""));
                    continue;
                }
                temp = octstr_create_from_data(reply->element[i]->str, reply->element[i]->len);
                gwlist_append(row, temp);
                //octstr_destroy(temp);
            }
            got_reply = 1;
            break;
        default:
            debug("dbpool.redis",0,"Received unknown Redis reply");
    }

    if(got_reply==1)
    {
        debug("dbpool.redis",0,"Received Redis reply");
        gwlist_produce(*res, row);
    }
    else
    {
        debug("dbpool.redis",0,"Received no Redis reply");
        freeReplyObject(reply);
        return -1;
    }

    freeReplyObject(reply);

    return 0;
}


static int redis_update(void *conn, Octstr *sql, List *binds)
{
    long i, binds_len;
    int ret;
    redisReply *reply;
    Octstr *bindval, *nullval;

    bindval = octstr_create("?");
    nullval = octstr_create("_NULL_");

    /* bind params if any */
    binds_len = gwlist_len(binds);
    if (binds_len > 0) {
        for (i = 0; i < binds_len; i++) {
            Octstr *str = gwlist_get(binds, i);
            if(octstr_len(str) > 0)
                octstr_replace_first(sql,bindval,str);
            else
                octstr_replace_first(sql,bindval,nullval);
        }
    }

    octstr_destroy(bindval);
    octstr_destroy(nullval);

    debug("dbpool.redis",0,"%s", octstr_get_cstr(sql));
    reply = redisCommand(conn, octstr_get_cstr(sql)); 

    if (reply->type == REDIS_REPLY_ERROR) {
        debug("dbpool.redis",0,"Received REDIS_REPLY_ERROR");
        error(0, "REDIS: redisCommand() failed. Error=%s", reply->str);
    }
    /* Some Redis commands (e.g. WATCH) return a boolean status */
    else if (reply->type == REDIS_REPLY_STATUS) {
        debug("dbpool.redis",0,"Received REDIS_REPLY_STATUS. Value=%s",reply->str);
        if (strcmp(reply->str,"OK") == 0) {
            freeReplyObject(reply);
            return 0;
        }
    }
    /* Other commands (e.g. DEL) return an integer indicating the number of keys affected */
    else if (reply->type == REDIS_REPLY_INTEGER) {
      debug("dbpool.redis",0,"Received REDIS_REPLY_INTEGER. Value=%qi",reply->integer);
      /*
       * Note: Redis returns a long long. Casting it to an int here could cause precision loss,
       * however as we're returning an update status, this should only ever be used to return
       * a count of keys deleted/updated, and this will almost invariably be 1.
       */
      ret = (int)reply->integer;
      freeReplyObject(reply);
      return ret;
    }
    /* The EXEC command returns an array of replies when executed successfully */
    else if (reply->type == REDIS_REPLY_ARRAY) {
      debug("dbpool.redis",0,"Received REDIS_REPLY_ARRAY");
      freeReplyObject(reply);
      return 0; /* For now, we only support EXEC commands with an array return and in that case, all is well */
    }
    /* Finally, the EXEC command can return a NULL if it fails (e.g. due to a WATCH triggering */
    else if (reply->type == REDIS_REPLY_NIL) { 
      debug("dbpool.redis",0,"Received REDIS_REPLY_NIL");
    }
    else {
      debug("dbpool.redis",0,"Received unknown Redis reply");
    }
      
    freeReplyObject(reply);
    return -1;
}


static void redis_conf_destroy(DBConf *db_conf)
{
    RedisConf *conf = db_conf->redis;

    octstr_destroy(conf->host);
    octstr_destroy(conf->password);

    gw_free(conf);
    gw_free(db_conf);
}


static struct db_ops redis_ops = {
    .open = redis_open_conn,
    .close = redis_close_conn,
    .check = redis_check_conn,
    .select = redis_select,
    .update = redis_update,
    .conf_destroy = redis_conf_destroy
};

#endif /* HAVE_REDIS */

