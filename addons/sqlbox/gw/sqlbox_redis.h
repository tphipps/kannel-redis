#include "gwlib/gwlib.h"

#if defined(HAVE_REDIS)

#define SQLBOX_REDIS_QUEUE_POP "BRPOP %S 10"
#define SQLBOX_REDIS_QUEUE_POP_WITH_INFLIGHT "BRPOPLPUSH %S %S 10"
#define SQLBOX_REDIS_QUEUE_PUSH "LPUSH %S %S"

#define SQLBOX_REDIS_GETID "INCR %S"
#define SQLBOX_REDIS_DELETE "LREM %S 1 %S"

#endif /* HAVE_REDIS */

#ifdef HAVE_REDIS
#include "gw/msg.h"
#include "sqlbox_sql.h"
void sql_save_msg(Msg *msg, Octstr *momt);
Msg *mysql_fetch_msg();
void sql_shutdown();
struct server_type *sqlbox_init_redis(Cfg *cfg);
#ifndef sqlbox_redis_c
extern
#endif
Octstr *sqlbox_id;
#endif
