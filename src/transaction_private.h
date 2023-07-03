#ifndef TRANSACTION_PRIVATE_H
#define TRANSACTION_PRIVATE_H

#include "splinterdb/transaction.h"
#include "splinterdb/public_platform.h"
#include "splinterdb/data.h"
#include "splinterdb_internal.h"
#include "data_internal.h"
#include "platform.h"
#include "atomic_counter.h"
#include "hashmap.h"

#define PARALLEL_VALIDATION

#define TRANSACTION_RW_SET_MAX 64

typedef struct transaction_internal transaction_internal;

typedef struct transaction_rw_set_entry {
   slice   key;
   message msg;
} transaction_rw_set_entry;

typedef struct transaction_table {
   transaction_internal *head;
   transaction_internal *tail;
} transaction_table;

typedef struct transaction_internal {
   timestamp tn;

   transaction_rw_set_entry rs[TRANSACTION_RW_SET_MAX];
   transaction_rw_set_entry ws[TRANSACTION_RW_SET_MAX];

   uint64 rs_size;
   uint64 ws_size;

#ifdef PARALLEL_VALIDATION
   transaction_table finish_active_transactions;
#endif

   transaction_internal *next;

   transaction_internal *start_txn;

   int64 ref_count;
} transaction_internal;

typedef struct transactional_splinterdb_config {
   splinterdb_config kvsb_cfg;
} transactional_splinterdb_config;

typedef struct transactional_splinterdb {
   splinterdb                      *kvsb;
   transactional_splinterdb_config *tcfg;
   atomic_counter                   ts_allocator;
   platform_mutex                   lock;
   transaction_table                all_transactions;
#ifdef PARALLEL_VALIDATION
   transaction_table active_transactions;
#endif
} transactional_splinterdb;

#endif
