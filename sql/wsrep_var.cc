/* Copyright 2008 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <mysqld.h>
#include <sql_class.h>
#include <sql_plugin.h>
#include <set_var.h>
#include <sql_acl.h>
//#include <sys_vars.h>
#include "wsrep_priv.h"
#include <my_dir.h>
#include <cstdio>
#include <cstdlib>

#define WSREP_START_POSITION_ZERO "00000000-0000-0000-0000-000000000000:-1"

// trx history position to start with
const  char* wsrep_start_position   = WSREP_START_POSITION_ZERO;
const  char* wsrep_provider         = WSREP_NONE;
const  char* wsrep_provider_options = (const char*)my_memdup("", 1, MYF(MY_WME));
const  char* wsrep_cluster_address  = NULL;
const  char* wsrep_cluster_name     = "my_wsrep_cluster";
const  char* wsrep_node_name        = glob_hostname;

int wsrep_init_vars()
{
  return 0;
}

bool wsrep_on_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL) {
    // FIXME: this variable probably should be changed only per session
    thd->variables.wsrep_on = global_system_variables.wsrep_on;
  }
  else {
  }

  if (thd->variables.wsrep_on)
    thd->variables.option_bits |= (OPTION_BIN_LOG);
  else
    thd->variables.option_bits &= ~(OPTION_BIN_LOG);
  return false;
}

void wsrep_causal_reads_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL) {
    thd->variables.wsrep_causal_reads = global_system_variables.wsrep_causal_reads;
  }
  else {
  }
}

static int wsrep_start_position_verify (const char* start_str)
{
  size_t        start_len;
  char*         endptr;
  wsrep_uuid_t  uuid;
  ssize_t       uuid_len;
  wsrep_seqno_t seqno;

  start_len = strlen (start_str);
  if (start_len < 34)
    return 1;

  uuid_len = wsrep_uuid_scan (start_str, start_len, &uuid);
  if (uuid_len < 0 || (start_len - uuid_len) < 2)
    return 1;

  if (start_str[uuid_len] != ':') // separator should follow UUID
    return 1;

  seqno = strtoll (&start_str[uuid_len + 1], &endptr, 10);
  if (*endptr == '\0') return 0; // remaining string was seqno

  return 1;
}

bool wsrep_start_position_check (sys_var *self, THD* thd, set_var* var)
{
  char   buff[FN_REFLEN];
  String str(buff, sizeof(buff), system_charset_info), *res;
  const char*   start_str = NULL;

  if (!(res = var->value->val_str(&str))) goto err;

  start_str = res->c_ptr();

  if (!start_str) goto err;

  if (!wsrep_start_position_verify(start_str)) return 0;

err:

  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name, 
           start_str ? start_str : "NULL");
  return 1;
}

void wsrep_set_local_position (const char* value)
{
  size_t value_len  = strlen (value);
  size_t uuid_len   = wsrep_uuid_scan (value, value_len, &local_uuid);

  local_seqno = strtoll (value + uuid_len + 1, NULL, 10);

  WSREP_INFO ("wsrep_start_position var submitted: '%s'", wsrep_start_position);
}

bool wsrep_start_position_update (sys_var *self, THD* thd, enum_var_type type)
{
  // since this value passed wsrep_start_position_check, don't check anything
  // here
  wsrep_set_local_position (wsrep_start_position);

  if (wsrep) {
    wsrep->sst_received (wsrep, &local_uuid, local_seqno, NULL, 0);
  }

  return 0;
}

void wsrep_start_position_init (const char* val)
{
  if (NULL == val || wsrep_start_position_verify (val))
  {
    WSREP_ERROR("Bad initial value for wsrep_start_position: %s", 
		(val ? val : ""));
    return;
  }

  wsrep_start_position = my_strdup(val, MYF(0)); 

  wsrep_set_local_position (val);
}

static int wsrep_provider_verify (const char* provider_str)
{
  MY_STAT   f_stat;
  char path[FN_REFLEN];

  if (!provider_str || strlen(provider_str)== 0)
    return 1;

  if (!strcmp(provider_str, WSREP_NONE))
    return 0;

  if (!unpack_filename(path, provider_str))
    return 1;

  /* check that provider file exists */
  bzero(&f_stat, sizeof(MY_STAT));
  if (!my_stat(path, &f_stat, MYF(0)))
  {
    return 1;
  }
  return 0;
}

bool wsrep_provider_check (sys_var *self, THD* thd, set_var* var)
{
  char   buff[FN_REFLEN];
  String str(buff, sizeof(buff), system_charset_info), *res;
  const char*   provider_str = NULL;

  if (!(res = var->value->val_str(&str))) goto err;

  provider_str = res->c_ptr();

  if (!provider_str) goto err;

  if (!wsrep_provider_verify(provider_str)) return 0;

err:

  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name, 
           provider_str ? provider_str : "NULL");
  return 1;
}

bool wsrep_provider_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool rcode= false;

  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  wsrep_stop_replication(thd);
  wsrep_deinit();

  char* tmp= strdup(wsrep_provider); // wsrep_init() rewrites provider 
                                     //when fails
  if (wsrep_init())
  {
    my_error(ER_CANT_OPEN_LIBRARY, MYF(0), tmp);
    rcode = true;
  }
  free(tmp);

  // we sure don't want to use old address with new provider
  wsrep_cluster_address_init(NULL);
  wsrep_provider_options_init(NULL);

  thd->variables.wsrep_on= wsrep_on_saved;

  return rcode;
}

void wsrep_provider_init (const char* value)
{
  if (NULL == value || wsrep_provider_verify (value))
  {
    WSREP_ERROR("Bad initial value for wsrep_provider: %s",
                (value ? value : ""));
    return;
  }
  wsrep_provider = my_strdup(value, MYF(0));
}

bool wsrep_provider_options_check(sys_var *self, THD* thd, set_var* var)
{
  return 0;
}

bool wsrep_provider_options_update(sys_var *self, THD* thd, enum_var_type type)
{
  wsrep_status_t ret= wsrep->options_set(wsrep, wsrep_provider_options);
  if (ret != WSREP_OK)
  {
    WSREP_ERROR("Set options returned %d", ret);
    return 1;
  }
  else
  {
    char* opts= wsrep->options_get(wsrep);

    if (opts)
    {
      if (wsrep_provider_options) my_free((void *)wsrep_provider_options);
      wsrep_provider_options = (char*)my_memdup(opts, strlen(opts) + 1, 
						MYF(MY_WME));
    }
    else
    {
      WSREP_ERROR("Failed to get provider options");
      return true;
    }
  }
  return false;
}

void wsrep_provider_options_init(const char* value)
{
  if (wsrep_provider_options && wsrep_provider_options != value) 
    my_free((void *)wsrep_provider_options);
  wsrep_provider_options = (value) ? my_strdup(value, MYF(0)) : NULL;
}

static int wsrep_cluster_address_verify (const char* cluster_address_str)
{
  /* There is no predefined address format, it depends on provider. */
  return 0;
}

bool wsrep_cluster_address_check (sys_var *self, THD* thd, set_var* var)
{
  char   buff[FN_REFLEN];
  String str(buff, sizeof(buff), system_charset_info), *res;
  const char*   cluster_address_str = NULL;

  if (!(res = var->value->val_str(&str))) goto err;

  cluster_address_str = res->c_ptr();

  if (!cluster_address_str) goto err;

  if (!wsrep_cluster_address_verify(cluster_address_str)) return 0;

 err:

  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name, 
             cluster_address_str ? cluster_address_str : "NULL");
  return 1    ;
}

bool wsrep_cluster_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  wsrep_stop_replication(thd);

  if (wsrep_start_replication())
  {
    wsrep_create_rollbacker();
    wsrep_create_appliers();
  }
  thd->variables.wsrep_on= wsrep_on_saved;

  return false;
}

void wsrep_cluster_address_init (const char* value)
{
  if (wsrep_cluster_address && wsrep_cluster_address != value) 
    my_free ((void*)wsrep_cluster_address);

  wsrep_cluster_address = (value) ? my_strdup(value, MYF(0)) :  NULL;
}

bool wsrep_cluster_name_check (sys_var *self, THD* thd, set_var* var)
{
  char   buff[FN_REFLEN];
  String str(buff, sizeof(buff), system_charset_info), *res;
  const char* cluster_name_str = NULL;

  if (!(res = var->value->val_str(&str))) goto err;

  cluster_name_str = res->c_ptr();

  if (!cluster_name_str) goto err;

  return 0;

 err:

  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name, 
             cluster_name_str ? cluster_name_str : "NULL");
  return 1;
}

bool wsrep_cluster_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

bool wsrep_node_name_check (sys_var *self, THD* thd, set_var* var)
{
  char   buff[FN_REFLEN];
  String str(buff, sizeof(buff), system_charset_info), *res;
  const char* node_name_str = NULL;

  if (!(res = var->value->val_str(&str))) goto err;

  node_name_str = res->c_ptr();

  if (!node_name_str) goto err;

  return 0;

 err:

  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name, 
             node_name_str ? node_name_str : "NULL");
  return 1;
}

bool wsrep_node_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

/*
 * Status variables stuff below
 */
static inline void
wsrep_assign_to_mysql (SHOW_VAR* mysql, wsrep_stats_var* wsrep)
{
  mysql->name = wsrep->name;
  switch (wsrep->type) {
  case WSREP_VAR_INT64:
    mysql->value = (char*) &wsrep->value._int64;
    mysql->type  = SHOW_LONGLONG;
    break;
  case WSREP_VAR_STRING:
    mysql->value = (char*) &wsrep->value._string;
    mysql->type  = SHOW_CHAR_PTR;
    break;
  case WSREP_VAR_DOUBLE:
    mysql->value = (char*) &wsrep->value._double;
    mysql->type  = SHOW_DOUBLE;
    break;
  }
}

static wsrep_stats_var*  wsrep_status_vars = NULL;

#if DYNAMIC
// somehow this mysql status thing works only with statically allocated arrays.
static SHOW_VAR*          mysql_status_vars = NULL;
static int                mysql_status_len  = -1;
#else
static SHOW_VAR           mysql_status_vars[100 + 1];
static const int          mysql_status_len  = 100;
#endif

static void export_wsrep_status_to_mysql()
{
  int wsrep_status_len, i;

  if (wsrep_status_vars) wsrep->stats_free (wsrep, wsrep_status_vars);

  wsrep_status_vars = wsrep->stats_get (wsrep);

  if (!wsrep_status_vars) {
    return;
  }

  for (wsrep_status_len = 0;
       wsrep_status_vars[wsrep_status_len].name != NULL;
       wsrep_status_len++);

#if DYNAMIC
  if (wsrep_status_len != mysql_status_len) {
    void* tmp = realloc (mysql_status_vars,
                         (wsrep_status_len + 1) * sizeof(SHOW_VAR));
    if (!tmp) {

      sql_print_error ("Out of memory for wsrep status variables."
                       "Number of variables: %d", wsrep_status_len);
      return;
    }

    mysql_status_len  = wsrep_status_len;
    mysql_status_vars = (SHOW_VAR*)tmp;
  }
  /* @TODO: fix this: */
#else
  if (mysql_status_len < wsrep_status_len) wsrep_status_len= mysql_status_len;
#endif

  for (i = 0; i < wsrep_status_len; i++)
    wsrep_assign_to_mysql (mysql_status_vars + i, wsrep_status_vars + i);

  mysql_status_vars[wsrep_status_len].name  = NullS;
  mysql_status_vars[wsrep_status_len].value = NullS;
  mysql_status_vars[wsrep_status_len].type  = SHOW_LONG;
}

int wsrep_show_status (THD *thd, SHOW_VAR *var, char *buff)
{
  export_wsrep_status_to_mysql();
  var->type= SHOW_ARRAY;
  var->value= (char *) &mysql_status_vars;
  return 0;
}
