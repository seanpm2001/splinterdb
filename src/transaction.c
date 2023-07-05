#include "data_internal.h"
#include "transaction_private.h"
#include "transaction_util.h"
#include "platform_linux/poison.h"

static int
transactional_splinterdb_create_or_open(const splinterdb_config   *kvsb_cfg,
                                        transactional_splinterdb **txn_kvsb,
                                        bool open_existing)
{
   transactional_splinterdb_config *txn_splinterdb_cfg;
   txn_splinterdb_cfg = TYPED_ZALLOC(0, txn_splinterdb_cfg);
   memcpy(txn_splinterdb_cfg, kvsb_cfg, sizeof(txn_splinterdb_cfg->kvsb_cfg));

   transactional_splinterdb *_txn_kvsb;
   _txn_kvsb       = TYPED_ZALLOC(0, _txn_kvsb);
   _txn_kvsb->tcfg = txn_splinterdb_cfg;

   int rc = splinterdb_create_or_open(
      &txn_splinterdb_cfg->kvsb_cfg, &_txn_kvsb->kvsb, open_existing);
   bool fail_to_create_splinterdb = (rc != 0);
   if (fail_to_create_splinterdb) {
      platform_free(0, _txn_kvsb);
      platform_free(0, txn_splinterdb_cfg);
      return rc;
   }

   atomic_counter_init(&_txn_kvsb->ts_allocator);
   platform_mutex_init(&_txn_kvsb->lock, 0, 0);
   transaction_table_init(&_txn_kvsb->all_transactions);
#ifdef PARALLEL_VALIDATION
   transaction_table_init(&_txn_kvsb->active_transactions);
#endif

   *txn_kvsb = _txn_kvsb;

   return 0;
}

int
transactional_splinterdb_create(const splinterdb_config   *kvsb_cfg,
                                transactional_splinterdb **txn_kvsb)
{
   return transactional_splinterdb_create_or_open(kvsb_cfg, txn_kvsb, FALSE);
}

int
transactional_splinterdb_open(const splinterdb_config   *kvsb_cfg,
                              transactional_splinterdb **txn_kvsb)
{
   return transactional_splinterdb_create_or_open(kvsb_cfg, txn_kvsb, TRUE);
}

void
transactional_splinterdb_close(transactional_splinterdb **txn_kvsb)
{
   transactional_splinterdb *_txn_kvsb = *txn_kvsb;
   splinterdb_close(&_txn_kvsb->kvsb);

   platform_mutex_destroy(&_txn_kvsb->lock);
#ifdef PARALLEL_VALIDATION
   transaction_table_deinit(&_txn_kvsb->active_transactions);
#endif
   transaction_table_deinit(&_txn_kvsb->all_transactions);
   atomic_counter_deinit(&_txn_kvsb->ts_allocator);

   platform_free(0, _txn_kvsb->tcfg);
   platform_free(0, _txn_kvsb);

   *txn_kvsb = NULL;
}

void
transactional_splinterdb_register_thread(transactional_splinterdb *kvs)
{
   splinterdb_register_thread(kvs->kvsb);
}

void
transactional_splinterdb_deregister_thread(transactional_splinterdb *kvs)
{
   splinterdb_deregister_thread(kvs->kvsb);
}

int
transactional_splinterdb_begin(transactional_splinterdb *txn_kvsb,
                               transaction              *txn)
{
   // Initialize the given transaction
   transaction_internal *txn_internal;
   transaction_internal_create(&txn_internal);

   platform_mutex_lock(&txn_kvsb->lock);
   txn_internal->start_txn = txn_kvsb->all_transactions.tail;
   ++txn_internal->start_txn->ref_count;
   platform_mutex_unlock(&txn_kvsb->lock);

   txn->internal = txn_internal;

   return 0;
}

static void
write_into_splinterdb(transactional_splinterdb *txn_kvsb,
                      transaction_internal     *txn_internal)
{
   int rc = 0;

   // Write all elements in txn->ws
   for (int i = 0; i < txn_internal->ws_size; ++i) {
      switch (message_class(txn_internal->ws[i].msg)) {
         case MESSAGE_TYPE_INSERT:
            rc = splinterdb_insert(txn_kvsb->kvsb,
                                   txn_internal->ws[i].key,
                                   message_slice(txn_internal->ws[i].msg));
            platform_assert(rc == 0);
            break;
         case MESSAGE_TYPE_UPDATE:
            rc = splinterdb_update(txn_kvsb->kvsb,
                                   txn_internal->ws[i].key,
                                   message_slice(txn_internal->ws[i].msg));
            platform_assert(rc == 0);
            break;
         case MESSAGE_TYPE_DELETE:
            rc = splinterdb_delete(txn_kvsb->kvsb, txn_internal->ws[i].key);
            platform_assert(rc == 0);
            break;
         default:
            platform_assert(0, "invalid operation");
      }
   }
}

int
transactional_splinterdb_commit(transactional_splinterdb *txn_kvsb,
                                transaction              *txn)
{
   transaction_internal *txn_internal = txn->internal;
   platform_assert(txn_internal != NULL);

#ifdef PARALLEL_VALIDATION
   platform_mutex_lock(&txn_kvsb->lock);

   transaction_table_init_from_table(&txn_internal->finish_active_transactions,
                                     &txn_kvsb->active_transactions);
   void *txn_copy = transaction_table_insert_by_copy(
      &txn_kvsb->active_transactions, txn_internal);

   platform_mutex_unlock(&txn_kvsb->lock);

   bool valid =
      transaction_check_for_conflict(&txn_kvsb->all_transactions,
                                     txn_internal,
                                     txn_kvsb->tcfg->kvsb_cfg.data_cfg);
   if (valid) {
      valid = transaction_check_for_conflict_with_active_transactions(
         txn_internal, txn_kvsb->tcfg->kvsb_cfg.data_cfg);
   }

   transaction_table_deinit(&txn_internal->finish_active_transactions);

   if (valid) {
      // The new transaction finally becomes visible globally
      write_into_splinterdb(txn_kvsb, txn_internal);

      platform_mutex_lock(&txn_kvsb->lock);

      txn_internal->tn = atomic_counter_get_next(&txn_kvsb->ts_allocator);
      transaction_table_insert(&txn_kvsb->all_transactions, txn_internal);
      transaction_table_delete(&txn_kvsb->active_transactions, txn_copy, TRUE);

      // FIXME: use atomic operation
      --txn_internal->start_txn->ref_count;

      transaction_gc(txn_kvsb);

      platform_mutex_unlock(&txn_kvsb->lock);
   } else {
      platform_mutex_lock(&txn_kvsb->lock);

      transaction_table_delete(&txn_kvsb->active_transactions, txn_copy, TRUE);

      // FIXME: use atomic operation
      --txn_internal->start_txn->ref_count;

      platform_mutex_unlock(&txn_kvsb->lock);

      transaction_internal_destroy((transaction_internal **)&txn->internal);

      return -1;
   }

   return 0;
#else
   platform_mutex_lock(&txn_kvsb->lock);

   bool valid =
      transaction_check_for_conflict(&txn_kvsb->all_transactions,
                                     txn_internal,
                                     txn_kvsb->tcfg->kvsb_cfg.data_cfg);

   if (valid) {
      // The new transaction finally becomes visible globally
      write_into_splinterdb(txn_kvsb, txn_internal);

      txn_internal->tn = atomic_counter_get_next(&txn_kvsb->ts_allocator);
      transaction_table_insert(&txn_kvsb->all_transactions, txn_internal);
   }

   // FIXME: use atomic operation
   --txn_internal->start_txn->ref_count;

   transaction_gc(txn_kvsb);

   // transaction_table_delete(&txn_kvsb->active_transactions, txn_internal);

   platform_mutex_unlock(&txn_kvsb->lock);

   if (!valid) {
      transaction_internal_destroy((transaction_internal **)&txn->internal);
      return -1;
   }

   // TODO: Garbage collection

   return 0;
#endif
}

int
transactional_splinterdb_abort(transactional_splinterdb *txn_kvsb,
                               transaction              *txn)
{
   platform_assert(txn->internal != NULL);

   transaction_internal_destroy((transaction_internal **)&txn->internal);

   return 0;
}

static void
insert_into_write_set(transaction_internal *txn_internal,
                      slice                 user_key,
                      message_type          op,
                      slice                 value,
                      const data_config    *cfg)
{
   key ukey = key_create_from_slice(user_key);

   // check if there is the same key in its write set
   for (uint64 i = 0; i < txn_internal->ws_size; ++i) {
      key wkey = key_create_from_slice(txn_internal->ws[i].key);
      if (data_key_compare(cfg, ukey, wkey) == 0) {
         if (op == MESSAGE_TYPE_INSERT) {
            void *old_msg_data = (void *)message_data(txn_internal->ws[i].msg);
            platform_free(0, old_msg_data);

            char *new_msg_data;
            new_msg_data =
               TYPED_ARRAY_ZALLOC(0, new_msg_data, slice_length(value));
            memcpy(new_msg_data, slice_data(value), slice_length(value));

            txn_internal->ws[i].msg = message_create(
               op, slice_create(slice_length(value), new_msg_data));
         } else if (op == MESSAGE_TYPE_DELETE) {
            txn_internal->ws[i].msg = DELETE_MESSAGE;
         } else if (op == MESSAGE_TYPE_UPDATE) {
            merge_accumulator new_msg;
            merge_accumulator_init_from_message(
               &new_msg, 0, message_create(op, value));

            data_merge_tuples(cfg, ukey, txn_internal->ws[i].msg, &new_msg);

            void *old_msg_data = (void *)message_data(txn_internal->ws[i].msg);
            platform_free(0, old_msg_data);

            char *new_msg_data;
            new_msg_data = TYPED_ARRAY_ZALLOC(
               0, new_msg_data, merge_accumulator_length(&new_msg));
            memcpy(new_msg_data,
                   merge_accumulator_data(&new_msg),
                   merge_accumulator_length(&new_msg));

            txn_internal->ws[i].msg = message_create(
               op,
               slice_create(merge_accumulator_length(&new_msg), new_msg_data));

            merge_accumulator_deinit(&new_msg);
         }

         return;
      }
   }


   char *key_buf;
   key_buf = TYPED_ARRAY_ZALLOC(0, key_buf, slice_length(user_key));
   memcpy(key_buf, slice_data(user_key), slice_length(user_key));
   txn_internal->ws[txn_internal->ws_size].key =
      slice_create(slice_length(user_key), key_buf);

   if (op == MESSAGE_TYPE_DELETE) {
      txn_internal->ws[txn_internal->ws_size].msg = DELETE_MESSAGE;
   } else {
      char *value_buf;
      value_buf = TYPED_ARRAY_ZALLOC(0, value_buf, slice_length(value));
      memcpy(value_buf, slice_data(value), slice_length(value));
      txn_internal->ws[txn_internal->ws_size].msg =
         message_create(op, slice_create(slice_length(value), value_buf));
   }

   ++txn_internal->ws_size;
}

static void
insert_into_read_set(transaction_internal *txn_internal, slice key)
{
   char *key_buf;
   key_buf = TYPED_ARRAY_ZALLOC(0, key_buf, slice_length(key));
   memcpy(key_buf, slice_data(key), slice_length(key));
   txn_internal->rs[txn_internal->rs_size].key =
      slice_create(slice_length(key), key_buf);

   ++txn_internal->rs_size;
}

int
transactional_splinterdb_insert(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                slice                     value)
{
   // Call non-transactional insertion for YCSB loading..
   if (txn == NULL) {
      return splinterdb_insert(txn_kvsb->kvsb, user_key, value);
   }

   transaction_internal *txn_internal = txn->internal;
   platform_assert(txn_internal != NULL);

   insert_into_write_set(txn_internal,
                         user_key,
                         MESSAGE_TYPE_INSERT,
                         value,
                         txn_kvsb->tcfg->kvsb_cfg.data_cfg);

   return 0;
}

int
transactional_splinterdb_delete(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key)
{
   transaction_internal *txn_internal = txn->internal;
   platform_assert(txn_internal != NULL);

   insert_into_write_set(txn_internal,
                         user_key,
                         MESSAGE_TYPE_DELETE,
                         NULL_SLICE,
                         txn_kvsb->tcfg->kvsb_cfg.data_cfg);

   return 0;
}

int
transactional_splinterdb_update(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                slice                     delta)
{
   transaction_internal *txn_internal = txn->internal;
   platform_assert(txn_internal != NULL);

   insert_into_write_set(txn_internal,
                         user_key,
                         MESSAGE_TYPE_UPDATE,
                         delta,
                         txn_kvsb->tcfg->kvsb_cfg.data_cfg);

   return 0;
}

int
transactional_splinterdb_lookup(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                splinterdb_lookup_result *result)
{
   transaction_internal *txn_internal = txn->internal;
   platform_assert(txn_internal != NULL);

   key ukey = key_create_from_slice(user_key);

   // Support read a value within its write set, which may not be committed
   for (int i = 0; i < txn_internal->ws_size; ++i) {
      key wkey = key_create_from_slice(txn_internal->ws[i].key);
      if (data_key_compare(txn_kvsb->tcfg->kvsb_cfg.data_cfg, ukey, wkey) == 0)
      {
         _splinterdb_lookup_result *_result =
            (_splinterdb_lookup_result *)result;
         merge_accumulator_copy_message(&_result->value,
                                        txn_internal->ws[i].msg);

         insert_into_read_set(txn_internal, user_key);

         return 0;
      }
   }

   int rc = splinterdb_lookup(txn_kvsb->kvsb, user_key, result);

   if (splinterdb_lookup_found(result)) {
      insert_into_read_set(txn_internal, user_key);
   }

   return rc;
}

const splinterdb *
transactional_splinterdb_get_db(transactional_splinterdb *txn_kvsb)
{
   return txn_kvsb->kvsb;
}

void
transactional_splinterdb_lookup_result_init(
   transactional_splinterdb *txn_kvsb,   // IN
   splinterdb_lookup_result *result,     // IN/OUT
   uint64                    buffer_len, // IN
   char                     *buffer      // IN
)
{
   return splinterdb_lookup_result_init(
      txn_kvsb->kvsb, result, buffer_len, buffer);
}

void
transactional_splinterdb_set_isolation_level(
   transactional_splinterdb   *txn_kvsb,
   transaction_isolation_level isol_level)
{
   // TODO: implement isolation_level. Current: serializable
}
