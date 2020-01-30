/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_INLINE_HPP
#define SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_INLINE_HPP

//#include "gc/g1/g1CollectedHeap.inline.hpp"
//#include "gc/g1/g1ConcurrentMark.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1ConcurrentMarkObjArrayProcessor.inline.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RegionMarkStatsCache.inline.hpp"
#include "gc/g1/g1RemSetTrackingPolicy.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "utilities/bitMap.inline.hpp"

// Semeru
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/g1SemeruConcurrentMark.hpp"
#include "gc/g1/g1ParScanThreadState.hpp"

// inline bool G1CMIsAliveClosure::do_object_b(oop obj) {
//   return !_semeru_h->is_obj_ill(obj);
// }

// Semeru
inline bool G1SemeruCMIsAliveClosure::do_object_b(oop obj) {
  return !_g1_semeru_h->is_obj_ill(obj);
}


// inline bool G1CMSubjectToDiscoveryClosure::do_object_b(oop obj) {
//   // Re-check whether the passed object is null. With ReferentBasedDiscovery the
//   // mutator may have changed the referent's value (i.e. cleared it) between the
//   // time the referent was determined to be potentially alive and calling this
//   // method.
//   if (obj == NULL) {
//     return false;
//   }
//   assert(_semeru_h->is_in_reserved(obj), "Trying to discover obj " PTR_FORMAT " not in heap", p2i(obj));
//   return _semeru_h->heap_region_containing(obj)->is_old_or_humongous_or_archive();
// }


// Semeru
inline bool G1SemeruCMSubjectToDiscoveryClosure::do_object_b(oop obj) {
  // Re-check whether the passed object is null. With ReferentBasedDiscovery the
  // mutator may have changed the referent's value (i.e. cleared it) between the
  // time the referent was determined to be potentially alive and calling this
  // method.
  if (obj == NULL) {
    return false;
  }
  assert(_g1_semeru_h->is_in_semeru_reserved(obj), "Trying to discover obj " PTR_FORMAT " not in heap", p2i(obj));
  return _g1_semeru_h->heap_region_containing(obj)->is_old_or_humongous_or_archive();
}


inline bool G1SemeruConcurrentMark::mark_in_next_bitmap(uint const worker_id, oop const obj) {
  HeapRegion* const hr = _semeru_h->heap_region_containing(obj);
  return mark_in_next_bitmap(worker_id, hr, obj);
}

/**
 * Tag : mark this object alive in the region's next_bitmap
 * 
 *  [?] Purpose? 
 *  => Used for Remark Phase. 
 *  Tell the region that there are new allocated objects since last Concurrent Full Marking ?
 * 
 */
inline bool G1SemeruConcurrentMark::mark_in_next_bitmap(uint const worker_id, HeapRegion* const hr, oop const obj) {
  assert(hr != NULL, "just checking");
  assert(hr->is_in_reserved(obj), "Attempting to mark object at " PTR_FORMAT " that is not contained in the given region %u", p2i(obj), hr->hrm_index());

  if (hr->obj_allocated_since_next_marking(obj)) {  // [?] if ture, this shoud be error ?
    return false;
  }

  // Some callers may have stale objects to mark above nTAMS after humongous reclaim.
  // Can't assert that this is a valid object at this point, since it might be in the process of being copied by another thread.
  assert(!hr->is_continues_humongous(), "Should not try to mark object " PTR_FORMAT " in Humongous continues region %u above nTAMS " PTR_FORMAT, p2i(obj), hr->hrm_index(), p2i(hr->next_top_at_mark_start()));

  HeapWord* const obj_addr = (HeapWord*)obj;

  bool success = _next_mark_bitmap->par_mark(obj_addr);   // [?] Mark obj alive in current 
  if (success) {
    add_to_liveness(worker_id, obj, obj->size());
  }
  return success;
}




#ifndef PRODUCT
template<typename Fn>
inline void G1SemeruCMMarkStack::iterate(Fn fn) const {
  assert_at_safepoint_on_vm_thread();

  size_t num_chunks = 0;

  TaskQueueEntryChunk* cur = _chunk_list;
  while (cur != NULL) {
    guarantee(num_chunks <= _chunks_in_chunk_list, "Found " SIZE_FORMAT " oop chunks which is more than there should be", num_chunks);

    for (size_t i = 0; i < EntriesPerChunk; ++i) {
      if (cur->data[i].is_null()) {
        break;
      }
      fn(cur->data[i]);
    }
    cur = cur->next;
    num_chunks++;
  }
}
#endif



inline void G1SemeruConcurrentMark::mark_in_prev_bitmap(oop p) {
  assert(!_prev_mark_bitmap->is_marked((HeapWord*) p), "sanity");
 _prev_mark_bitmap->mark((HeapWord*) p);
}

bool G1SemeruConcurrentMark::is_marked_in_prev_bitmap(oop p) const {
  assert(p != NULL && oopDesc::is_oop(p), "expected an oop");
  return _prev_mark_bitmap->is_marked((HeapWord*)p);
}

bool G1SemeruConcurrentMark::is_marked_in_next_bitmap(oop p) const {
  assert(p != NULL && oopDesc::is_oop(p), "expected an oop");
  return _next_mark_bitmap->is_marked((HeapWord*)p);
}

inline bool G1SemeruConcurrentMark::do_yield_check() {
  if (SuspendibleThreadSet::should_yield()) {
    SuspendibleThreadSet::yield();
    return true;
  } else {
    return false;
  }
}



inline HeapWord* G1SemeruConcurrentMark::top_at_rebuild_start(uint region) const {
  assert(region < _semeru_h->max_regions(), "Tried to access TARS for region %u out of bounds", region);
  return _top_at_rebuild_starts[region];
}


/**
 * Tag : update the _top_at_rebuild_starts to top.
 * 
 * [x] When to do this update ? 
 *  According to lock, only invoked in pre-rebuild. Update all the Old Region's _top_before_rebuild_starts. 
 *  
 * [?] RemSet Rebuild is a concurrent procedure, the top may be changed after the scan for this region ?
 * 
 */
inline void G1SemeruConcurrentMark::update_top_at_rebuild_start(HeapRegion* r) {
  uint const region = r->hrm_index();
  assert(region < _semeru_h->max_regions(), "Tried to access TARS for region %u out of bounds", region);
  assert(_top_at_rebuild_starts[region] == NULL,
         "TARS for region %u has already been set to " PTR_FORMAT " should be NULL",
         region, p2i(_top_at_rebuild_starts[region]));
  G1RemSetTrackingPolicy* tracker = _semeru_h->g1_policy()->remset_tracker();
  if (tracker->needs_scan_for_rebuild(r)) {   // except for Young, Free and Closed-Achrive HeapRegions.
    _top_at_rebuild_starts[region] = r->top();
  } else {
    // Leave TARS at NULL.
  }
}


inline void G1SemeruConcurrentMark::add_to_liveness(uint worker_id, oop const obj, size_t size) {
  task(worker_id)->update_liveness(obj, size);
}









// It scans an object and visits its children.
// 1) pop items from the G1SemeruCMTask->_semeru_task_queue one by one
// 2) Apply G1SemeruCMOopClosure to scan each object's field .
// 3) Mark the reached object alive and Enqueue newly marked objects into  G1SemeruCMTask->_semeru_task_queue.
inline void G1SemeruCMTask::scan_task_entry(G1SemeruTaskQueueEntry task_entry) { 
  process_grey_task_entry<true>(task_entry); 
}


inline void G1SemeruCMTask::push(G1SemeruTaskQueueEntry task_entry) {
  assert(task_entry.is_array_slice() || _semeru_h->is_in_g1_reserved(task_entry.obj()), "invariant");
  assert(task_entry.is_array_slice() || !_semeru_h->is_on_master_free_list(
              _semeru_h->heap_region_containing(task_entry.obj())), "invariant");
  assert(task_entry.is_array_slice() || !_semeru_h->is_obj_ill(task_entry.obj()), "invariant");  // FIXME!!!
  assert(task_entry.is_array_slice() || _next_mark_bitmap->is_marked((HeapWord*)task_entry.obj()), "invariant");

  if (!_semeru_task_queue->push(task_entry)) {
    // The local task queue looks full. We need to push some entries
    // to the global stack.
    // If the inserted task_entry exceed the G1SemeruCMTask->_semeru_task_queue,
    // transfer some data into G1SemeruConcurrentMark->_semeru_task_queue
    move_entries_to_global_stack();

    // this should succeed since, even if we overflow the global
    // stack, we should have definitely removed some entries from the
    // local queue. So, there must be space on it.
    bool success = _semeru_task_queue->push(task_entry);
    assert(success, "invariant");
  }
}

inline bool G1SemeruCMTask::is_below_finger(oop obj, HeapWord* global_finger) const {
  // If obj is above the global finger, then the mark bitmap scan
  // will find it later, and no push is needed.  Similarly, if we have
  // a current region and obj is between the local finger and the
  // end of the current region, then no push is needed.  The tradeoff
  // of checking both vs only checking the global finger is that the
  // local check will be more accurate and so result in fewer pushes,
  // but may also be a little slower.
  HeapWord* objAddr = (HeapWord*)obj;
  if (_finger != NULL) {
    // We have a current region.

    // Finger and region values are all NULL or all non-NULL.  We
    // use _finger to check since we immediately use its value.
    assert(_curr_region != NULL, "invariant");
    assert(_region_limit != NULL, "invariant");
    assert(_region_limit <= global_finger, "invariant");

    // True if obj is less than the local finger, or is between
    // the region limit and the global finger.
    if (objAddr < _finger) {
      return true;
    } else if (objAddr < _region_limit) {
      return false;
    } // Else check global finger.
  }
  // Check global finger.
  return objAddr < global_finger;
}

// template<bool scan>
// inline void G1SemeruCMTask::process_grey_task_entry(G1SemeruTaskQueueEntry task_entry) {
//   assert(scan || (task_entry.is_oop() && task_entry.obj()->is_typeArray()), "Skipping scan of grey non-typeArray");
//   assert(task_entry.is_array_slice() || _next_mark_bitmap->is_marked((HeapWord*)task_entry.obj()),
//          "Any stolen object should be a slice or marked");

//   if (scan) {
//     if (task_entry.is_array_slice()) {    // alrady sliced
//       _words_scanned += _objArray_processor.process_slice(task_entry.slice());
//     } else {
//       oop obj = task_entry.obj();
//       if (G1CMObjArrayProcessor::should_be_sliced(obj)) {   // an entire object array, should be sliced
//         _words_scanned += _objArray_processor.process_obj(obj);
//       } else {
//         _words_scanned += obj->oop_iterate_size(_cm_oop_closure);;  // a normal object instance, scan its fields.
//       }
//     }
//   }
  
//   // For semeru memory server, we don't need to count such a scavenge limitations ??
//    check_limits();
// }


/**
 * Use G1SemeruCMOopClosure to handle each fields.
 *  Like a BFS order.  
 * 
 */
template<bool scan>
inline void G1SemeruCMTask::process_grey_task_entry(G1SemeruTaskQueueEntry task_entry) {
  assert(scan || (task_entry.is_oop() && task_entry.obj()->is_typeArray()), "Skipping scan of grey non-typeArray");
  assert(task_entry.is_array_slice() || _next_mark_bitmap->is_marked((HeapWord*)task_entry.obj()),
         "Any stolen object should be a slice or marked");

  if (scan) {
    if (task_entry.is_array_slice()) {    // alrady sliced
     // _words_scanned += _objArray_processor.process_slice(task_entry.slice());

      //debug
      assert(false, "%s, object array slice tracing  Not finished yet \n", __func__);

    } else {
      oop obj = task_entry.obj();
      if (G1CMObjArrayProcessor::should_be_sliced(obj)) {   // an entire object array, should be sliced
       // _words_scanned += _objArray_processor.process_obj(obj);

        //debug
        assert(false, "%s, object array tracing  Not finished yet \n", __func__);
        

      } else {
        _words_scanned += obj->oop_iterate_size(_semeru_cm_oop_closure);  // a normal object instance, scan its fields.
      }
    }
  } // end of scan.
  
  // For semeru memory server, we don't need to count such a scavenge limitations ??
  // check_limits();
}


inline size_t G1SemeruCMTask::scan_objArray(objArrayOop obj, MemRegion mr) {
  obj->oop_iterate(_semeru_cm_oop_closure, mr);
  return mr.word_size();
}


inline void G1SemeruCMTask::update_liveness(oop const obj, const size_t obj_size) {
  _mark_stats_cache.add_live_words(_semeru_h->addr_to_region((HeapWord*)obj), obj_size);
}


inline void G1SemeruCMTask::abort_marking_if_regular_check_fail() {
  if (!regular_clock_call()) {
    set_has_aborted();
  }
}

/**
 * Tag : Mark the target object alive in CM->_next_bitmap.
 *  Only mark the object alive if it's below the finger. 
 *  Which means that we already scanned it but may bot mark it alive.
 * 
 *  This funtion is invoked in Remark Phase.
 *  1) Root scavenge, Java/VM thread variables.
 *  2) Java Thread's SATB queue.
 * 
 *  [?] Is there any objects is above the global_finger in Remark phase?
 *    => newly allocated objects ?
 * 
 */
// inline bool G1SemeruCMTask::make_reference_grey(oop obj) {
//   if (!_cm->mark_in_next_bitmap(_worker_id, obj)) {
//     return false;
//   }

//   // No OrderAccess:store_load() is needed. It is implicit in the
//   // CAS done in G1CMBitMap::parMark() call in the routine above.
//   HeapWord* global_finger = _cm->finger();

//   // We only need to push a newly grey object on the mark
//   // stack if it is in a section of memory the mark bitmap
//   // scan has already examined.  Mark bitmap scanning
//   // maintains progress "fingers" for determining that.
//   //
//   // Notice that the global finger might be moving forward
//   // concurrently. This is not a problem. In the worst case, we
//   // mark the object while it is above the global finger and, by
//   // the time we read the global finger, it has moved forward
//   // past this object. In this case, the object will probably
//   // be visited when a task is scanning the region and will also
//   // be pushed on the stack. So, some duplicate work, but no
//   // correctness problems.
//   if (is_below_finger(obj, global_finger)) {
//     G1SemeruTaskQueueEntry entry = G1SemeruTaskQueueEntry::from_oop(obj);
//     if (obj->is_typeArray()) {
//       // Immediately process arrays of primitive types, rather
//       // than pushing on the mark stack.  This keeps us from
//       // adding humongous objects to the mark stack that might
//       // be reclaimed before the entry is processed - see
//       // selection of candidates for eager reclaim of humongous
//       // objects.  The cost of the additional type test is
//       // mitigated by avoiding a trip through the mark stack,
//       // by only doing a bookkeeping update and avoiding the
//       // actual scan of the object - a typeArray contains no
//       // references, and the metadata is built-in.
//       process_grey_task_entry<false>(entry);
//     } else {
//       push(entry);
//     }
//   }
//   return true;
// }




/**
 * Semeru Memory Server - Mark the objet alive in alive_bitmap && push it into scan task_queue.
 * 
 * [?] What's the worker_id used for ? 
 * 
 * [?] If the target object isn't in current scanning Region, pointed by G1SemeruCMTask->_curr_region, skip it.
 * 
 */
inline bool G1SemeruCMTask::mark_in_alive_bitmap(uint const worker_id, oop const obj) {
  assert(_curr_region != NULL, "just checking");
  assert(_curr_region->is_in_reserved(obj), "Attempting to mark object at " PTR_FORMAT " that is not contained in the given region %u", 
                                                                                                  p2i(obj), _curr_region->hrm_index());

  // if ture, skip the marking for current oop.
  // also, this make sure the object is belone current Region's top.
  if (_curr_region->obj_allocated_since_next_marking(obj)) {
    return false;
  }

  // Some callers may have stale objects to mark above nTAMS after humongous reclaim.
  // Can't assert that this is a valid object at this point, since it might be in the process of being copied by another thread.
  assert(!_curr_region->is_continues_humongous(), "Should not try to mark object " PTR_FORMAT " in Humongous continues region %u above nTAMS " PTR_FORMAT, 
                                                                                    p2i(obj), _curr_region->hrm_index(), p2i(_curr_region->next_top_at_mark_start()));

  HeapWord* const obj_addr = (HeapWord*)obj;

  assert(_curr_region->alive_bitmap() == alive_bitmap(), "%s, Not marking at the corrent alive_bitmap \n", __func__);
  bool success = _alive_bitmap->par_mark(obj_addr);   // [?] Mark obj alive in current

  // Calculate the alive objects information. 
  // [?] Can we invoke freind class's function like  this ?
  if (success) {
    _semeru_cm->add_to_liveness(worker_id, obj, obj->size());
  }

  return success;
}







/**
 * Semeru Memory Server - Concurrently mark an object alive in HeapRegion->alive_bitmap
 *  
 *  [?] There may be multiple Semeru CM Threads marking the alive objects here ?
 *    => We need to confirm the MT safe.
 *       For a bitmap, this can be easily handled.
 * 
 * [?] Do we need the global_finger ??
 *    => Both CM and Remark use this function, so the marking can exceed the TAMS ?  
 *    => All the marking phases for a Region is incremental. We will not restart the CM from scratch ? 
 *        For G1 GC, prev_bitmap/next_bitmap are used for 2 different CM.
 * 
 */
inline bool G1SemeruCMTask::make_reference_alive(oop obj) {
  
  // At this time, each Region has its own alive_bitmap. Not like the global _next_bitmap which covers the whole heap.
  if( !mark_in_alive_bitmap(_worker_id, obj) ) {  // Mark object alive in alive_bitmap
    return false;
  }

  // No OrderAccess:store_load() is needed. It is implicit in the
  // CAS done in G1CMBitMap::parMark() call in the routine above.
  //HeapWord* global_finger = _cm->finger();

  // why do we need to wrap the obj as an TaskQueueEntry ??
  // unify the object array slice and object reference?
  G1SemeruTaskQueueEntry entry = G1SemeruTaskQueueEntry::from_oop(obj);  
  if (obj->is_typeArray()) {
      // Immediately process arrays of primitive types, rather
      // than pushing on the mark stack.  This keeps us from
      // adding humongous objects to the mark stack that might
      // be reclaimed before the entry is processed - see         // [?] The humonguous objects are earger to be recliamed ??
      // selection of candidates for eager reclaim of humongous
      // objects.  The cost of the additional type test is
      // mitigated by avoiding a trip through the mark stack,
      // by only doing a bookkeeping update and avoiding the
      // actual scan of the object - a typeArray contains no
      // references, and the metadata is built-in.
    process_grey_task_entry<false>(entry);  // [?] scan = false, only check the limit.
  } else {
    push(entry);    // Enqueue the object to scan its fields.
  }

  return true;
}








// template <class T>
// inline bool G1SemeruCMTask::deal_with_reference(T* p) {
//   increment_refs_reached();
//   oop const obj = RawAccess<MO_VOLATILE>::oop_load(p);
//   if (obj == NULL) {
//     return false;
//   }
//   return make_reference_grey(obj);
// }

/**
 * Semeru Memory Server - Concurrent Marking 
 * 
 * [?] Mark the objects alive in corresponding Region's alive_bitmap. 
 * 
 */
template <class T>
inline bool G1SemeruCMTask::deal_with_reference(T* p) {
  // increment_refs_reached();  // [?] Purpose for this counting ? count the incoming cross-region reference.

  oop const obj = RawAccess<MO_VOLATILE>::oop_load(p);   // [?] how to confirm this is a valid oop, not a evacuated Region ?
  if (obj == NULL) {
    return false;
  }
  
  // Check if this object is in current Region, if not, skip it.
  // Assume 1) Write Barrier has captured all the cross-region reference caused by mutator
  // 2) GC can update the cross-region referenced caused by alive object evacuation.
  if(_curr_region->is_in_reserved(obj) == false ){
    return false;
  }

  //return make_reference_grey(obj);
  // Mark the object alive and push it into the task_queue to scan its field.
  return make_reference_alive(obj);
}




//
// Target object queue related
//

inline void G1SemeruCMTask::trim_target_object_queue(TargetObjQueue* target_obj_queue) {
	StarTask ref;
	do {
		// Fully drain the queue.
		trim_target_object_queue_to_threshold(target_obj_queue ,0);
	} while (!target_obj_queue->is_empty());
}


inline void G1SemeruCMTask::trim_target_object_queue_to_threshold(TargetObjQueue* target_obj_queue, uint threshold) {
	StarTask ref;
	// Drain the overflow stack first, so other threads can potentially steal.
	while (target_obj_queue->pop_overflow(ref)) {
		if (!target_obj_queue->try_push_to_taskqueue(ref)) {
			dispatch_reference(ref);
		}
	}

	while (target_obj_queue->pop_local(ref, threshold)) {  // threshold = 64
		dispatch_reference(ref);
	}
}


inline void G1SemeruCMTask::dispatch_reference(StarTask ref) {
	
  //debug
  // [?] Write Semeru's own verify_task function.
  //assert(verify_task(ref), "sanity");

	if (ref.is_narrow()) {
		//deal_with_reference((narrowOop*)ref);

    assert(false, "%s, Not support narrow oop ye \n",__func__);

	} else {
		deal_with_reference((oop*)ref);
	}
}





#endif // SHARE_VM_GC_G1_SEMERU_G1CONCURRENTMARK_INLINE_HPP
