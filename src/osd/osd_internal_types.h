// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_OSD_INTERNAL_TYPES_H
#define CEPH_OSD_INTERNAL_TYPES_H

#include "osd_types.h"
#include "OpRequest.h"
#include "object_state.h"
#include "Watch.h" // for WatchRef

#include <execinfo.h>
#include <cxxabi.h>

/*
  * keep tabs on object modifications that are in flight.
  * we need to know the projected existence, size, snapset,
  * etc., because we don't send writes down to disk until after
  * replicas ack.
  */

struct SnapSetContext {
  hobject_t oid;
  SnapSet snapset;
  int ref;
  bool registered : 1;
  bool exists : 1;

  explicit SnapSetContext(const hobject_t& o) :
    oid(o), ref(0), registered(false), exists(true) { }
};

inline std::ostream& operator<<(std::ostream& out, const SnapSetContext& ssc)
{
  return out << "ssc(" << ssc.oid << " snapset: " << ssc.snapset
             << " ref: " << ssc.ref << " registered: "
             << ssc.registered << " exists: " << ssc.exists << ")";
}

struct ObjectContext;
class ObjectContextRef {
private:
  std::shared_ptr<ObjectContext> ptr_;
  
  // Forward declare - defined after ObjectContext
  std::string get_caller_info() const;
  void log_ref_change(const char* op, const void* this_addr) const;

public:
  ObjectContextRef() : ptr_(nullptr) {}
  
  // Allow construction from nullptr (for default parameters like = NULL)
  ObjectContextRef(std::nullptr_t) : ptr_(nullptr) {}
  
  ObjectContextRef(std::shared_ptr<ObjectContext> p) : ptr_(std::move(p)) {
    log_ref_change("construct", this);
  }
  
  ObjectContextRef(const ObjectContextRef& other) : ptr_(other.ptr_) {
    log_ref_change("copy_construct", this);
  }
  
  ObjectContextRef(ObjectContextRef&& other) noexcept : ptr_(std::move(other.ptr_)) {
    log_ref_change("move_construct", this);
  }
  
  ObjectContextRef& operator=(const ObjectContextRef& other) {
    if (this != &other) {
      ptr_ = other.ptr_;
      log_ref_change("copy_assign", this);
    }
    return *this;
  }
  
  ObjectContextRef& operator=(ObjectContextRef&& other) noexcept {
    if (this != &other) {
      ptr_ = std::move(other.ptr_);
      log_ref_change("move_assign", this);
    }
    return *this;
  }
  
  // Assignment from shared_ptr (for compatibility with SharedLRU get_next)
  ObjectContextRef& operator=(const std::shared_ptr<ObjectContext>& p) {
    ptr_ = p;
    log_ref_change("assign_from_shared_ptr", this);
    return *this;
  }
  
  ObjectContextRef& operator=(std::shared_ptr<ObjectContext>&& p) {
    ptr_ = std::move(p);
    log_ref_change("move_assign_from_shared_ptr", this);
    return *this;
  }
  
  ~ObjectContextRef() {
    if (ptr_) {
      log_ref_change("destruct", this);
    }
  }
  
  ObjectContext* get() const { return ptr_.get(); }
  ObjectContext& operator*() const { return *ptr_; }
  ObjectContext* operator->() const { return ptr_.get(); }
  explicit operator bool() const { return ptr_ != nullptr; }
  long use_count() const { return ptr_.use_count(); }
  void reset() {
    log_ref_change("reset", this);
    ptr_.reset();
  }
  
  // Comparison operators
  bool operator==(const ObjectContextRef& other) const { return ptr_ == other.ptr_; }
  bool operator!=(const ObjectContextRef& other) const { return ptr_ != other.ptr_; }
  bool operator==(const std::shared_ptr<ObjectContext>& other) const { return ptr_ == other; }
  bool operator!=(const std::shared_ptr<ObjectContext>& other) const { return ptr_ != other; }
  bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }
  
  // Implicit conversion to shared_ptr for compatibility
  operator std::shared_ptr<ObjectContext>() const { return ptr_; }
  
  // Get the underlying shared_ptr
  const std::shared_ptr<ObjectContext>& get_ptr() const { return ptr_; }
};

struct ObjectContext {
  ObjectState obs;

  SnapSetContext *ssc;  // may be null

  Context *destructor_callback;
  
  CephContext *cct;  // for logging

public:

  // any entity in obs.oi.watchers MUST be in either watchers or unconnected_watchers.
  std::map<std::pair<uint64_t, entity_name_t>, WatchRef> watchers;

  // attr cache
  std::map<std::string, ceph::buffer::list, std::less<>> attr_cache;

  RWState rwstate;
  std::list<OpRequestRef> waiters;  ///< ops waiting on state change
  bool get_read(OpRequestRef& op, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (rwstate.get_read_lock()) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " READ_LOCK acquired by " << caller
                       << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name() << ")" << dendl;
      }
      return true;
    } // else
      // Now we really need to bump up the ref-counter.
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " READ_LOCK blocked for " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ")" << dendl;
    }
    waiters.emplace_back(op);
    rwstate.inc_waiters();
    return false;
  }
  bool get_write(OpRequestRef& op, bool greedy=false, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (rwstate.get_write_lock(greedy)) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " WRITE_LOCK acquired by " << caller
                       << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                       << ", greedy=" << greedy << ")" << dendl;
      }
      return true;
    } // else
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " WRITE_LOCK blocked for " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ", greedy=" << greedy << ")" << dendl;
    }
    if (op) {
      waiters.emplace_back(op);
      rwstate.inc_waiters();
    }
    return false;
  }
  bool get_excl(OpRequestRef& op, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (rwstate.get_excl_lock()) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " EXCL_LOCK acquired by " << caller
                       << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name() << ")" << dendl;
      }
      return true;
    } // else
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " EXCL_LOCK blocked for " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ")" << dendl;
    }
    if (op) {
      waiters.emplace_back(op);
      rwstate.inc_waiters();
    }
    return false;
  }
  void wake(std::list<OpRequestRef> *requeue) {
    rwstate.release_waiters();
    requeue->splice(requeue->end(), waiters);
  }
  void put_read(std::list<OpRequestRef> *requeue, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " READ_LOCK released by " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ")" << dendl;
    }
    if (rwstate.put_read()) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " READ_LOCK fully released, waking waiters (waiters="
                       << rwstate.waiters << ")" << dendl;
      }
      wake(requeue);
    }
  }
  void put_write(std::list<OpRequestRef> *requeue, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " WRITE_LOCK released by " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ")" << dendl;
    }
    if (rwstate.put_write()) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " WRITE_LOCK fully released, waking waiters (waiters="
                       << rwstate.waiters << ")" << dendl;
      }
      wake(requeue);
    }
  }
  void put_excl(std::list<OpRequestRef> *requeue, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    if (hoid && caller && cct) {
      lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " EXCL_LOCK released by " << caller
                     << " (count=" << rwstate.count << ", state=" << rwstate.get_state_name()
                     << ", waiters=" << rwstate.waiters << ")" << dendl;
    }
    if (rwstate.put_excl()) {
      if (hoid && caller && cct) {
        lgeneric_subdout(cct, osd, 20) << "LOCK_TRACE: " << *hoid << " EXCL_LOCK fully released, waking waiters (waiters="
                       << rwstate.waiters << ")" << dendl;
      }
      wake(requeue);
    }
  }
  bool empty() const { return rwstate.empty(); }

  bool get_lock_type(OpRequestRef& op, RWState::State type, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    switch (type) {
    case RWState::RWWRITE:
      return get_write(op, false, hoid, caller);
    case RWState::RWREAD:
      return get_read(op, hoid, caller);
    case RWState::RWEXCL:
      return get_excl(op, hoid, caller);
    default:
      ceph_abort_msg("invalid lock type");
      return true;
    }
  }
  bool get_write_greedy(OpRequestRef& op, const hobject_t* hoid = nullptr, const char* caller = nullptr) {
    return get_write(op, true, hoid, caller);
  }
  bool get_snaptrimmer_write(bool mark_if_unsuccessful) {
    return rwstate.get_snaptrimmer_write(mark_if_unsuccessful);
  }
  bool get_recovery_read() {
    return rwstate.get_recovery_read();
  }
  bool try_get_read_lock() {
    return rwstate.get_read_lock();
  }
  void drop_recovery_read(std::list<OpRequestRef> *ls) {
    ceph_assert(rwstate.recovery_read_marker);
    put_read(ls);
    rwstate.recovery_read_marker = false;
  }
  void put_lock_type(
    RWState::State type,
    std::list<OpRequestRef> *to_wake,
    bool *requeue_recovery,
    bool *requeue_snaptrimmer,
    const hobject_t* hoid = nullptr,
    const char* caller = nullptr) {
    switch (type) {
    case RWState::RWWRITE:
      put_write(to_wake, hoid, caller);
      break;
    case RWState::RWREAD:
      put_read(to_wake, hoid, caller);
      break;
    case RWState::RWEXCL:
      put_excl(to_wake, hoid, caller);
      break;
    default:
      ceph_abort_msg("invalid lock type");
    }
    if (rwstate.empty() && rwstate.recovery_read_marker) {
      rwstate.recovery_read_marker = false;
      *requeue_recovery = true;
    }
    if (rwstate.empty() && rwstate.snaptrimmer_write_marker) {
      rwstate.snaptrimmer_write_marker = false;
      *requeue_snaptrimmer = true;
    }
  }
  bool is_request_pending() {
    return !rwstate.empty();
  }

  ObjectContext(CephContext *cct_ = nullptr)
    : ssc(NULL),
      destructor_callback(0),
      cct(cct_),
      blocked(false), requeue_scrub_on_unblock(false) {}

  ~ObjectContext() {
    ceph_assert(rwstate.empty());
    if (destructor_callback)
      destructor_callback->complete(0);
  }

  void start_block() {
    ceph_assert(!blocked);
    blocked = true;
  }
  void stop_block() {
    ceph_assert(blocked);
    blocked = false;
  }
  bool is_blocked() const {
    return blocked;
  }

  /// in-progress copyfrom ops for this object
  bool blocked;
  bool requeue_scrub_on_unblock;    // true if we need to requeue scrub on unblock
};
// ObjectContextRef method implementations (defined after ObjectContext)
inline std::string ObjectContextRef::get_caller_info() const {
  void* callstack[4];
  int frames = backtrace(callstack, 4);
  
  if (frames > 2) {
    char** symbols = backtrace_symbols(callstack, frames);
    if (symbols) {
      // Frame 0 = get_caller_info
      // Frame 1 = log_ref_change
      // Frame 2 = constructor/destructor/etc
      // Frame 3 = THE ACTUAL CALLER WE WANT
      std::string caller = symbols[3];
      free(symbols);
      
      // Extract function name from symbol
      size_t start = caller.find('(');
      size_t end = caller.find('+', start);
      if (start != std::string::npos && end != std::string::npos) {
        std::string mangled = caller.substr(start + 1, end - start - 1);
        
        // Demangle C++ name
        int status;
        char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
        if (status == 0 && demangled) {
          std::string result(demangled);
          free(demangled);
          return result;
        }
      }
      return caller;
    }
  }
  return "unknown";
}

inline void ObjectContextRef::log_ref_change(const char* op, const void* this_addr) const {
  if (ptr_ && ptr_->cct) {
    // std::string caller = get_caller_info();
    lgeneric_subdout(ptr_->cct, osd, 0) 
      << "MATTY OBC_REF: " << op
      << " hobject=" << ptr_->obs.oi.soid
      << " obc_ptr=" << (void*)ptr_.get()
      << " refcnt=" << ptr_.use_count()
      << " wrapper_addr=" << this_addr
      // << " caller=" << caller
      << dendl;
  }
}


inline std::ostream& operator<<(std::ostream& out, const ObjectState& obs)
{
  out << obs.oi.soid;
  if (!obs.exists)
    out << "(dne)";
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const ObjectContext& obc)
{
  return out << "obc(" << obc.obs << " " << obc.rwstate << ")";
}

class ObcLockManager {
  struct ObjectLockState {
    ObjectContextRef obc;
    RWState::State type;
    ObjectLockState(
      ObjectContextRef obc,
      RWState::State type)
      : obc(std::move(obc)), type(type) {}
  };
  std::map<hobject_t, ObjectLockState> locks;
public:
  ObcLockManager() = default;
  ObcLockManager(ObcLockManager &&) = default;
  ObcLockManager(const ObcLockManager &) = delete;
  ObcLockManager &operator=(ObcLockManager &&) = default;
  bool empty() const {
    return locks.empty();
  }
  bool get_lock_type(
    RWState::State type,
    const hobject_t &hoid,
    ObjectContextRef& obc,
    OpRequestRef& op,
    const char* caller = __builtin_FUNCTION()) {
    ceph_assert(locks.find(hoid) == locks.end());
    if (obc->get_lock_type(op, type, &hoid, caller)) {
      locks.insert(std::make_pair(hoid, ObjectLockState(obc, type)));
      return true;
    } else {
      return false;
    }
  }
  /// Get write lock, ignore starvation
  bool take_write_lock(
    const hobject_t &hoid,
    ObjectContextRef obc,
    const char* caller = __builtin_FUNCTION()) {
    ceph_assert(locks.find(hoid) == locks.end());
    if (obc->rwstate.take_write_lock()) {
      if (obc->cct) {
        lgeneric_subdout(obc->cct, osd, 20) << "LOCK_TRACE: " << hoid << " WRITE_LOCK taken (ignore starvation) by " << caller
                            << " (count=" << obc->rwstate.count << ", state=" << obc->rwstate.get_state_name() << ")" << dendl;
      }
      locks.insert(
 std::make_pair(
   hoid, ObjectLockState(obc, RWState::RWWRITE)));
      return true;
    } else {
      return false;
    }
  }
  /// Get write lock for snap trim
  bool get_snaptrimmer_write(
    const hobject_t &hoid,
    ObjectContextRef obc,
    bool mark_if_unsuccessful,
    const char* caller = __builtin_FUNCTION()) {
    ceph_assert(locks.find(hoid) == locks.end());
    if (obc->get_snaptrimmer_write(mark_if_unsuccessful)) {
      if (obc->cct) {
        lgeneric_subdout(obc->cct, osd, 20) << "LOCK_TRACE: " << hoid << " WRITE_LOCK acquired (snaptrimmer) by " << caller
                            << " (count=" << obc->rwstate.count << ", state=" << obc->rwstate.get_state_name() << ")" << dendl;
      }
      locks.insert(
 std::make_pair(
   hoid, ObjectLockState(obc, RWState::RWWRITE)));
      return true;
    } else {
      return false;
    }
  }
  /// Get write lock greedy
  bool get_write_greedy(
    const hobject_t &hoid,
    ObjectContextRef obc,
    OpRequestRef op,
    const char* caller = __builtin_FUNCTION()) {
    ceph_assert(locks.find(hoid) == locks.end());
    if (obc->get_write_greedy(op, &hoid, caller)) {
      locks.insert(
 std::make_pair(
   hoid, ObjectLockState(obc, RWState::RWWRITE)));
      return true;
    } else {
      return false;
    }
  }

  /// try get read lock
  bool try_get_read_lock(
    const hobject_t &hoid,
    ObjectContextRef obc,
    const char* caller = __builtin_FUNCTION()) {
    ceph_assert(locks.find(hoid) == locks.end());
    if (obc->try_get_read_lock()) {
      if (obc->cct) {
        lgeneric_subdout(obc->cct, osd, 20) << "LOCK_TRACE: " << hoid << " READ_LOCK tried and acquired by " << caller
                            << " (count=" << obc->rwstate.count << ", state=" << obc->rwstate.get_state_name() << ")" << dendl;
      }
      locks.insert(
 std::make_pair(
   hoid,
   ObjectLockState(obc, RWState::RWREAD)));
      return true;
    } else {
      return false;
    }
  }

  void put_locks(
    std::list<std::pair<ObjectContextRef, std::list<OpRequestRef> > > *to_requeue,
    bool *requeue_recovery,
    bool *requeue_snaptrimmer,
    const char* caller = __builtin_FUNCTION()) {
    for (auto& p: locks) {
      std::list<OpRequestRef> _to_requeue;
      p.second.obc->put_lock_type(
 p.second.type,
 &_to_requeue,
 requeue_recovery,
 requeue_snaptrimmer,
 &p.first,
 caller);
      if (to_requeue) {
        // We can safely std::move here as the whole `locks` is going
        // to die just after the loop.
 to_requeue->emplace_back(std::move(p.second.obc),
  		 std::move(_to_requeue));
      }
    }
    locks.clear();
  }
  ~ObcLockManager() {
    ceph_assert(locks.empty());
  }
};



#endif
