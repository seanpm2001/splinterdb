// Copyright 2022 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef _TRANSACTION_UTIL_H_
#define _TRANSACTION_UTIL_H_

#include "transaction_private.h"

void
transaction_internal_create(transaction_internal **new_internal);
void
transaction_internal_destroy(transaction_internal **internal_to_delete);

void
transaction_table_init(transaction_table *transactions);

void
transaction_table_init_from_table(transaction_table       *transactions,
                                  const transaction_table *other);

void
transaction_table_deinit(transaction_table *transactions);

void
transaction_table_insert(transaction_table    *transactions,
                         transaction_internal *txn);

void *
transaction_table_insert_by_copy(transaction_table          *transactions,
                                 const transaction_internal *original);

void
transaction_table_copy(transaction_table       *transactions,
                       const transaction_table *other);

void
transaction_table_delete(transaction_table    *transactions,
                         transaction_internal *txn,
                         bool                  should_free);

bool
transaction_check_for_conflict(transaction_table    *transactions,
                               transaction_internal *txn,
                               const data_config    *cfg);

#ifdef PARALLEL_VALIDATION
bool
transaction_check_for_conflict_with_active_transactions(
   transaction_internal *txn,
   const data_config    *cfg);
#endif

void
transaction_gc(transactional_splinterdb *txn_kvsb);

#endif // _TRANSACTION_UTIL_H_
