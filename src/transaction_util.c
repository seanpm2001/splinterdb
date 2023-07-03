#include "transaction_util.h"
#include "data_internal.h"
#include "platform_linux/poison.h"

static uint64 gc_count = 0;

void
transaction_internal_create(transaction_internal **new_internal)
{
   transaction_internal *txn_internal;
   txn_internal          = TYPED_ZALLOC(0, txn_internal);
   txn_internal->tn      = 0;
   txn_internal->ws_size = txn_internal->rs_size = 0;
   txn_internal->next                            = NULL;
   txn_internal->start_txn                       = NULL;
   txn_internal->ref_count                       = 0;

   *new_internal = txn_internal;
}

void
transaction_internal_create_by_copy(transaction_internal      **new_internal,
                                    const transaction_internal *other)
{
   transaction_internal *txn_internal;
   txn_internal            = TYPED_ZALLOC(0, txn_internal);
   txn_internal->tn        = other->tn;
   txn_internal->ws_size   = other->ws_size;
   txn_internal->rs_size   = other->rs_size;
   txn_internal->next      = NULL;
   txn_internal->start_txn = other->start_txn;
   txn_internal->ref_count = other->ref_count;

   for (uint64 i = 0; i < other->ws_size; ++i) {
      void *key =
         platform_aligned_zalloc(0, 64, slice_length(other->ws[i].key));
      memmove(
         key, slice_data(other->ws[i].key), slice_length(other->ws[i].key));
      txn_internal->ws[i].key =
         slice_create(slice_length(other->ws[i].key), key);

      void *msg =
         platform_aligned_zalloc(0, 64, message_length(other->ws[i].msg));
      memmove(
         msg, message_data(other->ws[i].msg), message_length(other->ws[i].msg));
      txn_internal->ws[i].msg =
         message_create(message_class(other->ws[i].msg),
                        slice_create(message_length(other->ws[i].msg), msg));
   }

   for (uint64 i = 0; i < other->rs_size; ++i) {
      void *key =
         platform_aligned_zalloc(0, 64, slice_length(other->rs[i].key));
      memmove(
         key, slice_data(other->rs[i].key), slice_length(other->rs[i].key));
      txn_internal->rs[i].key =
         slice_create(slice_length(other->rs[i].key), key);
   }

   *new_internal = txn_internal;
}

void
transaction_internal_destroy(transaction_internal **internal_to_delete)
{
   transaction_internal *txn_internal = *internal_to_delete;

   for (uint64 i = 0; i < txn_internal->ws_size; ++i) {
      void *to_delete = (void *)slice_data(txn_internal->ws[i].key);
      platform_free(0, to_delete);
      to_delete = (void *)message_data(txn_internal->ws[i].msg);
      platform_free(0, to_delete);
   }

   for (uint64 i = 0; i < txn_internal->rs_size; ++i) {
      void *to_delete = (void *)slice_data(txn_internal->rs[i].key);
      platform_free(0, to_delete);
   }

   platform_free(0, *internal_to_delete);
   *internal_to_delete = NULL;
}

void
transaction_table_init(transaction_table *transactions)
{
   transaction_internal_create(&transactions->head);
   transactions->tail = transactions->head;
}

void
transaction_table_init_from_table(transaction_table       *transactions,
                                  const transaction_table *other)
{
   transaction_table_init(transactions);
   transaction_table_copy(transactions, other);
}

void
transaction_table_deinit(transaction_table *transactions)
{
   transaction_internal *curr = transactions->head->next;
   while (curr) {
      transactions->head->next = curr->next;
      transaction_internal_destroy(&curr);
      curr = transactions->head->next;
   }
   transaction_internal_destroy(&transactions->head);
   transactions->tail = NULL;
}

void
transaction_table_insert(transaction_table    *transactions,
                         transaction_internal *txn)
{
   transactions->tail->next = txn;
   transactions->tail       = txn;
   transactions->tail->next = NULL;
}

void *
transaction_table_insert_by_copy(transaction_table          *transactions,
                                 const transaction_internal *original)
{
   transaction_internal *copy;
   transaction_internal_create_by_copy(&copy, original);
   transaction_table_insert(transactions, copy);

   return copy;
}
void
transaction_table_copy(transaction_table       *transactions,
                       const transaction_table *other)
{
   transaction_internal *other_curr = other->head->next;
   while (other_curr) {
      transaction_table_insert_by_copy(transactions, other_curr);
      other_curr = other_curr->next;
   }
}

void
transaction_table_delete(transaction_table    *transactions,
                         transaction_internal *txn,
                         bool                  should_free)
{
   transaction_internal *curr = transactions->head->next;
   transaction_internal *prev = transactions->head;

   while (curr) {
      if (curr == txn) {
         prev->next = curr->next;
         if (prev->next == NULL) {
            transactions->tail = prev;
         }

         if (should_free) {
            transaction_internal_destroy(&txn);
         }

         return;
      }

      prev = curr;
      curr = curr->next;
   }
}


// For debug purpose
void
transaction_internal_print(transaction_internal *txn_internal)
{
   platform_error_log("[%p] ---\n", txn_internal);
   platform_error_log("tn: %lu\n", txn_internal->tn);
   platform_error_log("ws_size: %lu\n", txn_internal->ws_size);
   platform_error_log("rs_size: %lu\n", txn_internal->rs_size);
   platform_error_log("start_txn: %p\n", txn_internal->start_txn);
   platform_error_log("ref_count: %lu\n", txn_internal->ref_count);
   platform_error_log("next: %p\n", txn_internal->next);
}

// For debug purpose
void
transaction_table_dump(transaction_table *transactions)
{
   transaction_internal *curr = transactions->head->next;
   while (curr) {
      transaction_internal_print(curr);
      curr = curr->next;
   }
}

bool
transaction_check_for_conflict(transaction_table    *transactions,
                               transaction_internal *txn,
                               const data_config    *cfg)
{
   transaction_internal *txn_committed = txn->start_txn->next;
   while (txn_committed) {
      for (uint64 i = 0; i < txn->rs_size; ++i) {
         for (uint64 j = 0; j < txn_committed->ws_size; ++j) {
            key rkey = key_create_from_slice(txn->rs[i].key);
            key wkey = key_create_from_slice(txn_committed->ws[j].key);
            if (data_key_compare(cfg, rkey, wkey) == 0) {
               return FALSE;
            }
         }
      }
      txn_committed = txn_committed->next;
   }

   return TRUE;
}

#ifdef PARALLEL_VALIDATION
// TODO: rename to the general
bool
transaction_check_for_conflict_with_active_transactions(
   transaction_internal *txn,
   const data_config    *cfg)
{
   transaction_internal *txn_i = txn->finish_active_transactions.head->next;
   while (txn_i) {
      for (uint64 i = 0; i < txn_i->ws_size; ++i) {
         key wkey = key_create_from_slice(txn_i->ws[i].key);
         for (uint64 j = 0; j < txn->rs_size; ++j) {
            key txn_rkey = key_create_from_slice(txn->rs[j].key);
            if (data_key_compare(cfg, wkey, txn_rkey) == 0) {
               return FALSE;
            }
         }

         for (uint64 j = 0; j < txn->ws_size; ++j) {
            key txn_wkey = key_create_from_slice(txn->ws[j].key);
            if (data_key_compare(cfg, wkey, txn_wkey) == 0) {
               return FALSE;
            }
         }
      }

      txn_i = txn_i->next;
   }

   return TRUE;
}

#endif

static void
transaction_table_delete_after(transaction_table    *transactions,
                               transaction_internal *txn)
{
   transaction_internal *curr = txn->next;
   if (curr == NULL) {
      return;
   }

   txn->next = curr->next;
   if (curr == transactions->tail) {
      transactions->tail = txn;
   }
   transaction_internal_destroy(&curr);
}

void
transaction_gc(transactional_splinterdb *txn_kvsb)
{
   if (txn_kvsb->all_transactions.head->ref_count > 0) {
      return;
   }

   transaction_internal *head = txn_kvsb->all_transactions.head;
   transaction_internal *curr = head->next;
   while (curr) {
      platform_assert(curr->ref_count >= 0);

      // transaction_table_dump(&txn_kvsb->all_transactions);

      if (curr->ref_count == 0) {
         transaction_table_delete_after(&txn_kvsb->all_transactions, head);

         ++gc_count;
      } else {
         break;
      }

      curr = head->next;
   }

   // platform_default_log("current gc_count: %lu\n", gc_count);
}
