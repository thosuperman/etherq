/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 *
 * QNX OS 8.0 port: Copyright (C) 2026 EtherQ contributors (GPL-3.0-or-later).
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include "oshw.h"

uint16 oshw_htons(uint16 host)
{
   return htons(host);
}

uint16 oshw_ntohs(uint16 network)
{
   return ntohs(network);
}

/* List network interfaces known to io-sock (libsocket if_nameindex()). */
ec_adaptert *oshw_find_adapters(void)
{
   struct if_nameindex *ids;
   ec_adaptert *adapter;
   ec_adaptert *prev_adapter = NULL;
   ec_adaptert *ret_adapter = NULL;
   int i;

   ids = if_nameindex();
   if (ids == NULL)
   {
      return NULL;
   }
   for (i = 0; ids[i].if_index != 0; i++)
   {
      adapter = (ec_adaptert *)calloc(1, sizeof(ec_adaptert));
      if (adapter == NULL)
      {
         break;
      }
      if (prev_adapter)
      {
         prev_adapter->next = adapter;
      }
      else
      {
         ret_adapter = adapter;
      }
      if (ids[i].if_name)
      {
         strncpy(adapter->name, ids[i].if_name, EC_MAXLEN_ADAPTERNAME - 1);
         strncpy(adapter->desc, ids[i].if_name, EC_MAXLEN_ADAPTERNAME - 1);
      }
      prev_adapter = adapter;
   }
   if_freenameindex(ids);
   return ret_adapter;
}

void oshw_free_adapters(ec_adaptert *adapter)
{
   ec_adaptert *next;
   while (adapter != NULL)
   {
      next = adapter->next;
      free(adapter);
      adapter = next;
   }
}
