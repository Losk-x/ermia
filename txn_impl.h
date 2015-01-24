#ifndef _NDB_TXN_IMPL_H_
#define _NDB_TXN_IMPL_H_

#include "txn.h"
#include "lockguard.h"

#include "object.h"
#include "dbcore/sm-common.h"

using namespace TXN;

/* Versions with more than 2.5 billion LSN delta from current are
   "old" and treated as read-mode. Readers do not apply SSN to these
   tuples, and writers (expected to be rare) must assume that the
   tuple has been read by a transaction that committed just before the
   writer. The upside is that readers pay vastly less than normal; the
   downside is this effectively means that any transaction that
   overwrites an old version cannot read under other committed
   overwrites: any meaningful sstamp would violate the exclusion
   window.
 */
#ifdef USE_PARALLEL_SSN
//static int64_t constexpr OLD_VERSION_THRESHOLD = 0xa0000000ll;
//static int64_t constexpr OLD_VERSION_THRESHOLD = 0x10000000ll;
static int64_t constexpr OLD_VERSION_THRESHOLD = 0xffffffffll;
//static int64_t constexpr OLD_VERSION_THRESHOLD = 0;
#endif

// base definitions

template <template <typename> class Protocol, typename Traits>
transaction<Protocol, Traits>::transaction(uint64_t flags, string_allocator_type &sa)
  : transaction_base(flags), xid(TXN::xid_alloc()), log(logger->new_tx_log()), sa(&sa)
{
  RA::epoch_enter();
#ifdef BTREE_LOCK_OWNERSHIP_CHECKING
  concurrent_btree::NodeLockRegionBegin();
#endif
#ifdef USE_PARALLEL_SSN
  ssn_register_tx(xid);
#endif
  xid_context *xc = xid_get_context(xid);
  //write_set.set_empty_key(NULL);
  RCU::rcu_enter();
  xc->begin = logger->cur_lsn();
  xc->end = INVALID_LSN;
  xc->state = TXN_EMBRYO;
}

template <template <typename> class Protocol, typename Traits>
transaction<Protocol, Traits>::~transaction()
{
  // transaction shouldn't fall out of scope w/o resolution
  // resolution means TXN_EMBRYO, TXN_CMMTD, and TXN_ABRTD
  INVARIANT(state() != TXN_ACTIVE && state() != TXN_COMMITTING);

#ifdef BTREE_LOCK_OWNERSHIP_CHECKING
  concurrent_btree::AssertAllNodeLocksReleased();
#endif
#ifdef USE_PARALLEL_SSN
  ssn_deregister_tx(xid);
#endif
  xid_free(xid);
  //write_set.clear();
  //read_set.clear();
  RA::epoch_exit();
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::signal_abort(abort_reason reason)
{
  abort_trap(reason);
#ifdef USE_PARALLEL_SSN
  if (reason == ABORT_REASON_SSN_EXCLUSION_FAILURE)
    TXN::tls_ssn_abort_count++;
#endif
  throw transaction_abort_exception(reason);
}
  
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::abort_impl()
{
  if (likely(state() != TXN_COMMITTING))
    volatile_write(xid_get_context(xid)->state, TXN_ABRTD);
  for (auto &w : write_set) {
    if (not w.second.btr)   // for repeated overwrites
        continue;
    dbtuple *tuple = w.second.new_tuple;
    ASSERT(tuple);
    ASSERT(XID::from_ptr(tuple->clsn) == xid);
    w.second.btr->unlink_tuple(w.second.oid, tuple);
  }

#ifdef USE_PARALLEL_SSN
  for (auto &r : read_set) {
    ASSERT(r.tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
    ASSERT(r.tuple == r.btr->fetch_committed_version_at(r.oid, xid, LSN::from_ptr(r.tuple->clsn)));
    // remove myself from reader list
    ssn_deregister_reader_tx(r.tuple);
  }
#endif
  RCU::rcu_enter();
  if (likely(state() != TXN_COMMITTING))
    log->pre_commit();
  log->discard();
  if (unlikely(state() == TXN_COMMITTING))
    volatile_write(xid_get_context(xid)->state, TXN_ABRTD);
  RCU::rcu_exit();
}

namespace {
  inline const char *
  transaction_state_to_cstr(txn_state state)
  {
    switch (state) {
    case TXN_EMBRYO: return "TXN_EMBRYO";
    case TXN_ACTIVE: return "TXN_ACTIVE";
    case TXN_ABRTD: return "TXN_ABRTD";
    case TXN_CMMTD: return "TXN_CMMTD";
    case TXN_COMMITTING: return "TXN_COMMITTING";
    }
    ALWAYS_ASSERT(false);
    return 0;
  }

  inline std::string
  transaction_flags_to_str(uint64_t flags)
  {
    bool first = true;
    std::ostringstream oss;
    if (flags & transaction_base::TXN_FLAG_LOW_LEVEL_SCAN) {
      oss << "TXN_FLAG_LOW_LEVEL_SCAN";
      first = false;
    }
    if (flags & transaction_base::TXN_FLAG_READ_ONLY) {
      if (first)
        oss << "TXN_FLAG_READ_ONLY";
      else
        oss << " | TXN_FLAG_READ_ONLY";
      first = false;
    }
    return oss.str();
  }
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::dump_debug_info() const
{
  std::cerr << "Transaction (obj=" << util::hexify(this) << ") -- state "
       << transaction_state_to_cstr(state()) << std::endl;
  std::cerr << "  Abort Reason: " << AbortReasonStr(reason) << std::endl;
  std::cerr << "  Flags: " << transaction_flags_to_str(flags) << std::endl;
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::commit()
{
#ifdef USE_PARALLEL_SSN
  ssn_parallel_si_commit();
#else
  si_commit();
#endif
}

#ifdef USE_PARALLEL_SSN
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::ssn_parallel_si_commit()
{
  xid_context* xc = xid_get_context(xid);

  PERF_DECL(
      static std::string probe0_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":total:")));
  ANON_REGION(probe0_name.c_str(), &transaction_base::g_txn_commit_probe0_cg);

  switch (state()) {
  case TXN_EMBRYO:
  case TXN_ACTIVE:
    volatile_write(xc->state, TXN_COMMITTING);
    break;
  case TXN_CMMTD:
  case TXN_COMMITTING:
  case TXN_ABRTD:
    ALWAYS_ASSERT(false);
  }

  INVARIANT(log);
  // get clsn, abort if failed
  RCU::rcu_enter();
  xc->end = log->pre_commit();
  LSN clsn = xc->end;
  auto cstamp = clsn.offset();
  if (xc->end == INVALID_LSN)
    signal_abort(ABORT_REASON_INTERNAL);

  // note that sstamp comes from reads, but the read optimization might
  // ignore looking at tuple's sstamp at all, so if tx sstamp is still
  // the initial value so far, we need to initialize it as cstamp. (so
  // that later we can fill tuple's sstamp as cstamp in case sstamp still
  // remained as the initial value.) Consider the extreme case where
  // old_version_threshold = 0: means no read set at all...
  if (xc->sstamp > cstamp)
    xc->sstamp = cstamp;

  // find out my largest predecessor (\eta) and smallest sucessor (\pi)
  // for reads, see if sb. has written the tuples - look at sucessor lsn
  // for writes, see if sb. has read the tuples - look at access lsn

  for (auto &w : write_set) {
    if (not w.second.btr)   // for repeated overwrites
        continue;
    dbtuple *tuple = w.second.new_tuple;

    // go to the precommitted or committed version I (am about to)
    // overwrite for the reader list
    dbtuple *overwritten_tuple = w.first;   // the key
    if (overwritten_tuple == tuple) // insert, see do_tree_put for the rules
      continue;

    int64_t age = 0;
    // note fetch_overwriter might return a tuple with clsn being an XID
    // (of a precommitted but still in post-commit transaction).
  try_get_age:
    fat_ptr overwritten_tuple_clsn = volatile_read(overwritten_tuple->clsn);
    if (overwritten_tuple_clsn.asi_type() == fat_ptr::ASI_XID) {
      // then that tx must have pre-committed, ie it has a valid xc->end
      // and need to go to the tx's context to find out its cstamp to
      // calculate age
      XID overwritten_xid = XID::from_ptr(overwritten_tuple_clsn);
      xid_context *overwritten_xc = xid_get_context(overwritten_xid);
      ASSERT(volatile_read(overwritten_xc->end).offset());
      if (overwritten_xc->owner != overwritten_xid)
        goto try_get_age;
      age = xc->begin.offset() - volatile_read(overwritten_xc->end).offset();
    }
    else {
      ASSERT(overwritten_tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
      age = xc->begin.offset() - overwritten_tuple->clsn.offset();
    }
    ASSERT(volatile_read(overwritten_tuple->sstamp) == 0);

    /* for old tuples, just assume xstamp = cstamp-1 otherwise, check
       reader list and such
     */
    if (age < OLD_VERSION_THRESHOLD) {
      // need access stamp , i.e., who read this version that I'm trying to overwrite?
      readers_list::bitmap_t readers = ssn_get_tuple_readers(overwritten_tuple);
      while (readers) {
        int i = __builtin_ctz(readers);
        ASSERT(i >= 0 and i < 24);
        readers &= (readers-1);
      get_reader:
        XID rxid = volatile_read(rlist.xids[i]);
        if (not rxid._val or rxid == xc->owner)
          continue; // ignore invalid entries and ignore my own reads
        xid_context *reader_xc = xid_get_context(rxid);
        if (not reader_xc)
          continue;
        // copy everything before doing anything
        auto reader_owner = volatile_read(reader_xc->owner);
        auto reader_end = volatile_read(reader_xc->end).offset();
        if (reader_owner != rxid)
            goto get_reader;
        // reader committed
        if (reader_end and reader_end < cstamp and wait_for_commit_result(reader_xc)) {
          if (xc->pstamp < reader_end)
            xc->pstamp = reader_end;
        }
      }
    }
    else {
        // pstamp can't be larger than this, no need to check
        xc->pstamp = cstamp - 1;
        break;
    }
  }
  ASSERT(xc->pstamp <= cstamp - 1);

  for (auto &r : read_set) {
    // skip writes (note we didn't remove the one in read set)
    if (write_set[r.tuple].btr)
      continue;
    // so tuple should be the committed version I read
    ASSERT(r.tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
    dbtuple *overwriter_tuple = (dbtuple *)r.btr->fetch_overwriter(r.oid,
                                                  LSN::from_ptr(r.tuple->clsn));
    if (not overwriter_tuple)
      continue;

  try_get_sucessor:
    // read tuple->slsn to a local variable before doing anything relying on it,
    // it might be changed any time...
    fat_ptr sucessor_clsn = volatile_read(overwriter_tuple->clsn);

    // overwriter in progress?
    if (sucessor_clsn.asi_type() == fat_ptr::ASI_XID) {
      XID successor_xid = XID::from_ptr(sucessor_clsn);
      xid_context *sucessor_xc = xid_get_context(successor_xid);
      if (not sucessor_xc)
        continue;
      auto successor_owner = volatile_read(sucessor_xc->owner);
      if (successor_owner == xc->owner)  // myself
          continue;

      // read everything before doing anything
      auto sucessor_end = volatile_read(sucessor_xc->end).offset();
      if (successor_owner != successor_xid)
        goto try_get_sucessor;

      // overwriter might haven't committed, be serialized after me, or before me
      if (not sucessor_end) // not even in precommit, don't bother
          ;
      else if (sucessor_end > cstamp)    // serialzed after me, (dependency trivially satisfied as I as the reader will (hopefully) commit first)
          ;
      else {
        if (wait_for_commit_result(sucessor_xc)) {    // either wait or give conservative estimation
          // now read the sucessor stamp (sucessor's clsn, as sucessor needs to fill its clsn to the overwritten tuple's slsn at post-commit)
          if (sucessor_end < xc->sstamp)
            xc->sstamp = sucessor_end;
        } // otherwise aborted, ignore
      }
    }
    else {
      // overwriter already fully committed/aborted or no overwriter at all
      ASSERT(sucessor_clsn.asi_type() == fat_ptr::ASI_LOG);
      uint64_t tuple_sstamp = volatile_read(r.tuple->sstamp);
      // tuple_sstamp = 0 means no one has overwritten this version so far
      if (tuple_sstamp and tuple_sstamp < xc->sstamp)
        xc->sstamp = tuple_sstamp;
    }
  }

  if (not ssn_check_exclusion(xc))
    signal_abort(ABORT_REASON_SSN_EXCLUSION_FAILURE);

  // ok, can really commit if we reach here
  log->commit(NULL);
  RCU::rcu_exit();

  // change state
  volatile_write(xc->state, TXN_CMMTD);

  // post-commit: stuff access stamps for reads; init new versions
  for (auto &w : write_set) {
    if (not w.second.btr)   // for repeated overwrites
        continue;
    dbtuple *tuple = w.second.new_tuple;
    dbtuple *next_tuple = w.first;
    if (tuple != next_tuple) {   // update, not insert
      ASSERT(volatile_read(next_tuple->clsn).asi_type());
      // FIXME: tuple's sstamp should never decrease?
      ASSERT(xc->sstamp and xc->sstamp != ~uint64_t{0});
      volatile_write(next_tuple->sstamp, xc->sstamp);
    }
    volatile_write(tuple->xstamp, cstamp);
    tuple->clsn = clsn.to_log_ptr();
    ASSERT(tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
  }

  for (auto &r : read_set) {
    if (write_set[r.tuple].btr)
      continue;
    ASSERT(r.tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
    uint64_t xlsn;
    do {
      xlsn = volatile_read(r.tuple->xstamp);
    }
    while (xlsn < cstamp and not __sync_bool_compare_and_swap(&r.tuple->xstamp, xlsn, cstamp));
    // remove myself from readers set, so others won't see "invalid XID" while enumerating readers
    ssn_deregister_reader_tx(r.tuple);
  }
}
#else
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::si_commit()
{
  xid_context* xc = xid_get_context(xid);

  PERF_DECL(
      static std::string probe0_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":total:")));
  ANON_REGION(probe0_name.c_str(), &transaction_base::g_txn_commit_probe0_cg);

  switch (state()) {
  case TXN_EMBRYO:
  case TXN_ACTIVE:
    xc->state = TXN_COMMITTING;
    break;
  case TXN_CMMTD:
  case TXN_COMMITTING: 
  case TXN_ABRTD:
    ALWAYS_ASSERT(false);
  }
  
  INVARIANT(log);
  // get clsn, abort if failed
  RCU::rcu_enter();
  xc->end = log->pre_commit();
  if (xc->end == INVALID_LSN)
      signal_abort(ABORT_REASON_INTERNAL);
  log->commit(NULL);
  RCU::rcu_exit();

  // change state
  volatile_write(xid_get_context(xid)->state, TXN_CMMTD);

  // post-commit cleanup: install clsn to tuples
  // (traverse write-tuple)
  // stuff clsn in tuples in write-set
  for (auto &w : write_set) {
    if (not w.second.btr)
      continue;
    dbtuple* tuple = w.second.new_tuple;
    tuple->clsn = xc->end.to_log_ptr();
    INVARIANT(tuple->clsn.asi_type() == fat_ptr::ASI_LOG);
  }
}
#endif

typedef object_vector<typename concurrent_btree::value_type> tuple_vector_type;
// FIXME: tzwang: note: we only try once in this function. If it
// failed (returns false) then the caller (supposedly do_tree_put)
// should fallback to normal update procedure.
template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::try_insert_new_tuple(
    concurrent_btree *btr,
    const std::string *key,
	object* value,
    dbtuple::tuple_writer_t writer)
{
  INVARIANT(key);
  char*p = (char*)value;
  dbtuple* tuple = reinterpret_cast<dbtuple*>(p + sizeof(object));
  tuple_vector_type* tuple_vector = btr->get_tuple_vector();
  oid_type oid = tuple_vector->alloc();
  fat_ptr new_head = fat_ptr::make( value, INVALID_SIZE_CODE, 0);
  if (not tuple_vector->put(oid, new_head))
    return false;

  typename concurrent_btree::insert_info_t insert_info;
  if (unlikely(!btr->insert_if_absent(
          varkey(*key), oid, tuple, &insert_info))) {
    VERBOSE(std::cerr << "insert_if_absent failed for key: " << util::hexify(key) << std::endl);
    ++transaction_base::g_evt_dbtuple_write_insert_failed;
    tuple_vector->unlink(oid, tuple);
    return false;
  }
  VERBOSE(std::cerr << "insert_if_absent suceeded for key: " << util::hexify(key) << std::endl
                    << "  new dbtuple is " << util::hexify(tuple) << std::endl);

  // insert to log
  // FIXME: tzwang: leave pdest as null and FID is always 1 now.
  INVARIANT(log);
  auto record_size = align_up((size_t)tuple->size);
  auto size_code = encode_size_aligned(record_size);
  log->log_insert(1,
                  oid,
                  fat_ptr::make(tuple, size_code),
                  DEFAULT_ALIGNMENT_BITS, NULL);
  // update write_set
  write_set[tuple] = write_record_t(tuple, btr, oid);
  return true;
}

template <template <typename> class Protocol, typename Traits>
template <typename ValueReader>
bool
transaction<Protocol, Traits>::do_tuple_read(
    concurrent_btree *btr_ptr, oid_type oid, dbtuple *tuple, ValueReader &value_reader)
{
  INVARIANT(tuple);
  ++evt_local_search_lookups;

  // do the actual tuple read
  dbtuple::ReadStatus stat;
  {
    PERF_DECL(static std::string probe0_name(std::string(__PRETTY_FUNCTION__) + std::string(":do_read:")));
    ANON_REGION(probe0_name.c_str(), &private_::txn_btree_search_probe0_cg);
    tuple->prefetch();
    stat = tuple->stable_read(value_reader, this->string_allocator());
    if (unlikely(stat == dbtuple::READ_FAILED)) {
      const transaction_base::abort_reason r = transaction_base::ABORT_REASON_UNSTABLE_READ;
      signal_abort(r);
    }
  }
  INVARIANT(stat == dbtuple::READ_EMPTY ||
            stat == dbtuple::READ_RECORD);
  if (stat == dbtuple::READ_EMPTY) {
    ++transaction_base::g_evt_read_logical_deleted_node_search;
    return false;
  }

#ifdef USE_PARALLEL_SSN
  // SSN stamps and check
  if (tuple->clsn.asi_type() == fat_ptr::ASI_LOG) {
    xid_context* xc = xid_get_context(xid);
    ASSERT(xc);
    auto v_clsn = tuple->clsn.offset();
    int64_t age = xc->begin.offset() - v_clsn;
    if (age < OLD_VERSION_THRESHOLD) {
      // \eta - largest predecessor. So if I read this tuple, I should commit
      // after the tuple's creator (trivial, as this is committed version, so
      // this tuple's clsn can only be a predecessor of me): so just update
      // my \eta if needed.
      if (xc->pstamp < v_clsn)
          xc->pstamp = v_clsn;

      // Now if this tuple was overwritten by somebody, this means if I read
      // it, that overwriter will have anti-dependency on me (I must be
      // serialized before the overwriter), and it already committed (as a
      // successor of mine), so I need to update my \pi for the SSN check.
      // This is the easier case of anti-dependency (the other case is T1
      // already read a (then latest) version, then T2 comes to overwrite it).
      auto tuple_sstamp = volatile_read(tuple->sstamp);
      if (not tuple_sstamp) {   // no overwrite so far
        if (ssn_register_reader_tx(tuple, xid))
            read_set.emplace_back(tuple, btr_ptr, oid);
      }
      else if (xc->sstamp > tuple_sstamp)
        xc->sstamp = tuple_sstamp; // \pi

#ifdef DO_EARLY_SSN_CHECKS
      if (not ssn_check_exclusion(xc))
        signal_abort(ABORT_REASON_SSN_EXCLUSION_FAILURE);
#endif
    }
  }
#endif
  return true;
}

#endif /* _NDB_TXN_IMPL_H_ */