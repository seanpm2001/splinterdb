#include "experimental_mode.h"

#if EXPERIMENTAL_MODE_TICTOC_DISK
#   include "transaction_impl/transaction_tictoc_disk.h"
#else
#   if EXPERIMENTAL_MODE_ATOMIC_WORD
#      if EXPERIMENTAL_MODE_KEEP_ALL_KEYS
#         include "transaction_impl/transaction_tictoc_cache_nolock.h"
#      else
#         include "transaction_impl/transaction_fantasticc_nolock.h"
#      endif
#   else
#      if EXPERIMENTAL_MODE_KEEP_ALL_KEYS
#         include "transaction_impl/transaction_tictoc_cache.h"
#      else
#         include "transaction_impl/transaction_fantasticc.h"
#      endif
#   endif
#endif

const splinterdb *
transactional_splinterdb_get_db(transactional_splinterdb *txn_kvsb)
{
   return txn_kvsb->kvsb;
}
