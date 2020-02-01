/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RegionMarkStatsCache.inline.hpp"
#include "gc/g1/g1StringDedup.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/genOopClosures.inline.hpp"
#include "gc/shared/referencePolicy.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "include/jvm.h"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/prefetch.inline.hpp"
#include "services/memTracker.hpp"
#include "utilities/align.hpp"
#include "utilities/growableArray.hpp"

// Semeru
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/g1SemeruConcurrentMark.hpp"
#include "gc/g1/g1SemeruConcurrentMark.inline.hpp"



G1SemeruCMMarkStack::G1SemeruCMMarkStack() :
	_max_chunk_capacity(0),
	_base(NULL),
	_chunk_capacity(0) {
	set_empty();
}

bool G1SemeruCMMarkStack::resize(size_t new_capacity) {
	assert(is_empty(), "Only resize when stack is empty.");
	assert(new_capacity <= _max_chunk_capacity,
				 "Trying to resize stack to " SIZE_FORMAT " chunks when the maximum is " SIZE_FORMAT, new_capacity, _max_chunk_capacity);

	TaskQueueEntryChunk* new_base = MmapArrayAllocator<TaskQueueEntryChunk>::allocate_or_null(new_capacity, mtGC);

	if (new_base == NULL) {
		log_warning(gc)("Failed to reserve memory for new overflow mark stack with " SIZE_FORMAT " chunks and size " SIZE_FORMAT "B.", new_capacity, new_capacity * sizeof(TaskQueueEntryChunk));
		return false;
	}
	// Release old mapping.
	if (_base != NULL) {
		MmapArrayAllocator<TaskQueueEntryChunk>::free(_base, _chunk_capacity);
	}

	_base = new_base;
	_chunk_capacity = new_capacity;
	set_empty();

	return true;
}

size_t G1SemeruCMMarkStack::capacity_alignment() {
	return (size_t)lcm(os::vm_allocation_granularity(), sizeof(TaskQueueEntryChunk)) / sizeof(G1SemeruTaskQueueEntry);
}

bool G1SemeruCMMarkStack::initialize(size_t initial_capacity, size_t max_capacity) {
	guarantee(_max_chunk_capacity == 0, "G1SemeruCMMarkStack already initialized.");

	size_t const TaskEntryChunkSizeInVoidStar = sizeof(TaskQueueEntryChunk) / sizeof(G1SemeruTaskQueueEntry);

	_max_chunk_capacity = align_up(max_capacity, capacity_alignment()) / TaskEntryChunkSizeInVoidStar;
	size_t initial_chunk_capacity = align_up(initial_capacity, capacity_alignment()) / TaskEntryChunkSizeInVoidStar;

	guarantee(initial_chunk_capacity <= _max_chunk_capacity,
						"Maximum chunk capacity " SIZE_FORMAT " smaller than initial capacity " SIZE_FORMAT,
						_max_chunk_capacity,
						initial_chunk_capacity);

	log_debug(gc)("Initialize mark stack with " SIZE_FORMAT " chunks, maximum " SIZE_FORMAT,
								initial_chunk_capacity, _max_chunk_capacity);

	return resize(initial_chunk_capacity);
}

void G1SemeruCMMarkStack::expand() {
	if (_chunk_capacity == _max_chunk_capacity) {
		log_debug(gc)("Can not expand overflow mark stack further, already at maximum capacity of " SIZE_FORMAT " chunks.", _chunk_capacity);
		return;
	}
	size_t old_capacity = _chunk_capacity;
	// Double capacity if possible
	size_t new_capacity = MIN2(old_capacity * 2, _max_chunk_capacity);

	if (resize(new_capacity)) {
		log_debug(gc)("Expanded mark stack capacity from " SIZE_FORMAT " to " SIZE_FORMAT " chunks",
									old_capacity, new_capacity);
	} else {
		log_warning(gc)("Failed to expand mark stack capacity from " SIZE_FORMAT " to " SIZE_FORMAT " chunks",
										old_capacity, new_capacity);
	}
}

G1SemeruCMMarkStack::~G1SemeruCMMarkStack() {
	if (_base != NULL) {
		MmapArrayAllocator<TaskQueueEntryChunk>::free(_base, _chunk_capacity);
	}
}

void G1SemeruCMMarkStack::add_chunk_to_list(TaskQueueEntryChunk* volatile* list, TaskQueueEntryChunk* elem) {
	elem->next = *list;
	*list = elem;
}

void G1SemeruCMMarkStack::add_chunk_to_chunk_list(TaskQueueEntryChunk* elem) {
	MutexLockerEx x(MarkStackChunkList_lock, Mutex::_no_safepoint_check_flag);
	add_chunk_to_list(&_chunk_list, elem);
	_chunks_in_chunk_list++;
}

void G1SemeruCMMarkStack::add_chunk_to_free_list(TaskQueueEntryChunk* elem) {
	MutexLockerEx x(MarkStackFreeList_lock, Mutex::_no_safepoint_check_flag);
	add_chunk_to_list(&_free_list, elem);
}

G1SemeruCMMarkStack::TaskQueueEntryChunk* G1SemeruCMMarkStack::remove_chunk_from_list(TaskQueueEntryChunk* volatile* list) {
	TaskQueueEntryChunk* result = *list;
	if (result != NULL) {
		*list = (*list)->next;
	}
	return result;
}

G1SemeruCMMarkStack::TaskQueueEntryChunk* G1SemeruCMMarkStack::remove_chunk_from_chunk_list() {
	MutexLockerEx x(MarkStackChunkList_lock, Mutex::_no_safepoint_check_flag);
	TaskQueueEntryChunk* result = remove_chunk_from_list(&_chunk_list);
	if (result != NULL) {
		_chunks_in_chunk_list--;
	}
	return result;
}

G1SemeruCMMarkStack::TaskQueueEntryChunk* G1SemeruCMMarkStack::remove_chunk_from_free_list() {
	MutexLockerEx x(MarkStackFreeList_lock, Mutex::_no_safepoint_check_flag);
	return remove_chunk_from_list(&_free_list);
}

G1SemeruCMMarkStack::TaskQueueEntryChunk* G1SemeruCMMarkStack::allocate_new_chunk() {
	// This dirty read of _hwm is okay because we only ever increase the _hwm in parallel code.
	// Further this limits _hwm to a value of _chunk_capacity + #threads, avoiding
	// wraparound of _hwm.
	if (_hwm >= _chunk_capacity) {
		return NULL;
	}

	size_t cur_idx = Atomic::add(1u, &_hwm) - 1;
	if (cur_idx >= _chunk_capacity) {
		return NULL;
	}

	TaskQueueEntryChunk* result = ::new (&_base[cur_idx]) TaskQueueEntryChunk;
	result->next = NULL;
	return result;
}

bool G1SemeruCMMarkStack::par_push_chunk(G1SemeruTaskQueueEntry* ptr_arr) {
	// Get a new chunk.
	TaskQueueEntryChunk* new_chunk = remove_chunk_from_free_list();

	if (new_chunk == NULL) {
		// Did not get a chunk from the free list. Allocate from backing memory.
		new_chunk = allocate_new_chunk();

		if (new_chunk == NULL) {
			return false;
		}
	}

	Copy::conjoint_memory_atomic(ptr_arr, new_chunk->data, EntriesPerChunk * sizeof(G1SemeruTaskQueueEntry));

	add_chunk_to_chunk_list(new_chunk);

	return true;
}

bool G1SemeruCMMarkStack::par_pop_chunk(G1SemeruTaskQueueEntry* ptr_arr) {
	TaskQueueEntryChunk* cur = remove_chunk_from_chunk_list();

	if (cur == NULL) {
		return false;
	}

	Copy::conjoint_memory_atomic(cur->data, ptr_arr, EntriesPerChunk * sizeof(G1SemeruTaskQueueEntry));

	add_chunk_to_free_list(cur);
	return true;
}

void G1SemeruCMMarkStack::set_empty() {
	_chunks_in_chunk_list = 0;
	_hwm = 0;
	_chunk_list = NULL;
	_free_list = NULL;
}

G1SemeruCMRootRegions::G1SemeruCMRootRegions(uint const max_regions) :
	_root_regions(NEW_C_HEAP_ARRAY(HeapRegion*, max_regions, mtGC)),
	_max_regions(max_regions),
	_num_root_regions(0),
	_claimed_root_regions(0),
	_scan_in_progress(false),
	_should_abort(false) { }

G1SemeruCMRootRegions::~G1SemeruCMRootRegions() {
	FREE_C_HEAP_ARRAY(HeapRegion*, _max_regions);
}

void G1SemeruCMRootRegions::reset() {
	_num_root_regions = 0;
}

/**
 * Tag : Add a HeapRegion into CM Root Region set.
 * 
 */
void G1SemeruCMRootRegions::add(HeapRegion* hr) {
	assert_at_safepoint();
	size_t idx = Atomic::add((size_t)1, &_num_root_regions) - 1;
	assert(idx < _max_regions, "Trying to add more root regions than there is space " SIZE_FORMAT, _max_regions);
	_root_regions[idx] = hr;
}

void G1SemeruCMRootRegions::prepare_for_scan() {
	assert(!scan_in_progress(), "pre-condition");

	_scan_in_progress = _num_root_regions > 0;

	_claimed_root_regions = 0;
	_should_abort = false;
}

HeapRegion* G1SemeruCMRootRegions::claim_next() {
	if (_should_abort) {
		// If someone has set the should_abort flag, we return NULL to
		// force the caller to bail out of their loop.
		return NULL;
	}

	if (_claimed_root_regions >= _num_root_regions) {
		return NULL;
	}

	size_t claimed_index = Atomic::add((size_t)1, &_claimed_root_regions) - 1;
	if (claimed_index < _num_root_regions) {
		return _root_regions[claimed_index];
	}
	return NULL;
}

uint G1SemeruCMRootRegions::num_root_regions() const {
	return (uint)_num_root_regions;
}

void G1SemeruCMRootRegions::notify_scan_done() {
	MutexLockerEx x(RootRegionScan_lock, Mutex::_no_safepoint_check_flag);
	_scan_in_progress = false;
	RootRegionScan_lock->notify_all();
}

void G1SemeruCMRootRegions::cancel_scan() {
	notify_scan_done();
}

void G1SemeruCMRootRegions::scan_finished() {
	assert(scan_in_progress(), "pre-condition");

	if (!_should_abort) {
		assert(_claimed_root_regions >= num_root_regions(),
					 "we should have claimed all root regions, claimed " SIZE_FORMAT ", length = %u",
					 _claimed_root_regions, num_root_regions());
	}

	notify_scan_done();
}

bool G1SemeruCMRootRegions::wait_until_scan_finished() {
	if (!scan_in_progress()) {
		return false;
	}

	{
		MutexLockerEx x(RootRegionScan_lock, Mutex::_no_safepoint_check_flag);
		while (scan_in_progress()) {
			RootRegionScan_lock->wait(Mutex::_no_safepoint_check_flag);
		}
	}
	return true;
}

// Returns the maximum number of workers to be used in a concurrent
// phase based on the number of GC workers being used in a STW
// phase.
static uint scale_concurrent_worker_threads(uint num_gc_workers) {
	return MAX2((num_gc_workers + 2) / 4, 1U);		// Calculate the Concurrent Threads number according to the Parallel Threads number.
}



G1SemeruConcurrentMark::G1SemeruConcurrentMark(G1SemeruCollectedHeap* g1h,
																	 G1RegionToSpaceMapper* prev_bitmap_storage,
																	 G1RegionToSpaceMapper* next_bitmap_storage) :
	// _cm_thread set inside the constructor
	_semeru_h(g1h),
	_completed_initialization(false),

	_mark_bitmap_1(),			// initialized according to prev_bitmap_storage, it covers the whole heap.
	_mark_bitmap_2(),
	_prev_mark_bitmap(&_mark_bitmap_1),
	_next_mark_bitmap(&_mark_bitmap_2),

	_heap(_semeru_h->reserved_region()),

	_root_regions(_semeru_h->max_regions()),

	_global_mark_stack(),

	// _finger set in set_non_marking_state

	_worker_id_offset(DirtyCardQueueSet::num_par_ids() + G1ConcRefinementThreads),
	_max_num_tasks(ParallelGCThreads),				//[?] _max_num_tasks is decided by  the parameter ParallelGCThreads ??
	// _num_active_tasks set in set_non_marking_state()
	// _tasks set inside the constructor

	_task_queues(new G1SemeruCMTaskQueueSet((int) _max_num_tasks)),		// parallel threads or CM threads ?
	_terminator((int) _max_num_tasks, _task_queues),

	_first_overflow_barrier_sync(),
	_second_overflow_barrier_sync(),

	_has_overflown(false),
	_concurrent(false),
	_has_aborted(false),
	_restart_for_overflow(false),
	_gc_timer_cm(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
	_gc_tracer_cm(new (ResourceObj::C_HEAP, mtGC) G1OldTracer()),

	// _verbose_level set below

	_init_times(),
	_remark_times(),
	_remark_mark_times(),
	_remark_weak_ref_times(),
	_cleanup_times(),
	_total_cleanup_time(0.0),

	_accum_task_vtime(NULL),

	_concurrent_workers(NULL),
	_num_concurrent_workers(0),
	_max_concurrent_workers(0),

	_region_mark_stats(NEW_C_HEAP_ARRAY(G1RegionMarkStats, _semeru_h->max_regions(), mtGC)),
	_top_at_rebuild_starts(NEW_C_HEAP_ARRAY(HeapWord*, _semeru_h->max_regions(), mtGC))
{

	// [?] use the commit region to initialize bitmap.
	//_mark_bitmap_1.initialize(g1h->reserved_region(), prev_bitmap_storage);		// Allocate space to cover the whole g1 heap
	//_mark_bitmap_2.initialize(g1h->reserved_region(), next_bitmap_storage);

	// Create & start ConcurrentMark thread.
	_cm_thread = new G1ConcurrentMarkThread(this);  //[?] Only created a single, specific concurrent mark thread ? not use the CT pool?
	if (_cm_thread->osthread() == NULL) {
		vm_shutdown_during_initialization("Could not create ConcurrentMarkThread");
	}

	assert(CGC_lock != NULL, "CGC_lock must be initialized");

	// [x] if NOT set the parameter ConcGCThreads, calculated it by ParallelGCThreads
	//
	if (FLAG_IS_DEFAULT(ConcGCThreads) || ConcGCThreads == 0) {   
		// Calculate the number of concurrent worker threads by scaling
		// the number of parallel GC threads.
		uint marking_thread_num = scale_concurrent_worker_threads(ParallelGCThreads);
		FLAG_SET_ERGO(uint, ConcGCThreads, marking_thread_num);
	}

	assert(ConcGCThreads > 0, "ConcGCThreads have been set.");
	if (ConcGCThreads > ParallelGCThreads) {
		log_warning(gc)("More ConcGCThreads (%u) than ParallelGCThreads (%u).",
										ConcGCThreads, ParallelGCThreads);
		return;
	}

	log_debug(gc)("ConcGCThreads: %u offset %u", ConcGCThreads, _worker_id_offset);
	log_debug(gc)("ParallelGCThreads: %u", ParallelGCThreads);

	_num_concurrent_workers = ConcGCThreads;
	_max_concurrent_workers = _num_concurrent_workers;

	_concurrent_workers = new WorkGang("G1 Conc", _max_concurrent_workers, false, true);
	_concurrent_workers->initialize_workers();

	if (FLAG_IS_DEFAULT(MarkStackSize)) {		// [?] Who sets the MarkStackSize ??
		size_t mark_stack_size =
			MIN2(MarkStackSizeMax,
					MAX2(MarkStackSize, (size_t) (_max_concurrent_workers * TASKQUEUE_SIZE)));
		// Verify that the calculated value for MarkStackSize is in range.
		// It would be nice to use the private utility routine from Arguments.
		if (!(mark_stack_size >= 1 && mark_stack_size <= MarkStackSizeMax)) {
			log_warning(gc)("Invalid value calculated for MarkStackSize (" SIZE_FORMAT "): "
											"must be between 1 and " SIZE_FORMAT,
											mark_stack_size, MarkStackSizeMax);
			return;
		}
		FLAG_SET_ERGO(size_t, MarkStackSize, mark_stack_size);
	} else {
		// Verify MarkStackSize is in range.
		if (FLAG_IS_CMDLINE(MarkStackSize)) {
			if (FLAG_IS_DEFAULT(MarkStackSizeMax)) {
				if (!(MarkStackSize >= 1 && MarkStackSize <= MarkStackSizeMax)) {
					log_warning(gc)("Invalid value specified for MarkStackSize (" SIZE_FORMAT "): "
													"must be between 1 and " SIZE_FORMAT,
													MarkStackSize, MarkStackSizeMax);
					return;
				}
			} else if (FLAG_IS_CMDLINE(MarkStackSizeMax)) {
				if (!(MarkStackSize >= 1 && MarkStackSize <= MarkStackSizeMax)) {
					log_warning(gc)("Invalid value specified for MarkStackSize (" SIZE_FORMAT ")"
													" or for MarkStackSizeMax (" SIZE_FORMAT ")",
													MarkStackSize, MarkStackSizeMax);
					return;
				}
			}
		}
	}

	if (!_global_mark_stack.initialize(MarkStackSize, MarkStackSizeMax)) {
		vm_exit_during_initialization("Failed to allocate initial concurrent mark overflow mark stack.");
	}

	// Why not use the ConcGCThreads ??
	// Does the _tasks[] also include the ParallelThread ?
	// 
	_tasks = NEW_C_HEAP_ARRAY(G1SemeruCMTask*, _max_num_tasks, mtGC);			
	_accum_task_vtime = NEW_C_HEAP_ARRAY(double, _max_num_tasks, mtGC);

	// so that the assertion in MarkingTaskQueue::task_queue doesn't fail
	_num_active_tasks = _max_num_tasks;

	for (uint i = 0; i < _max_num_tasks; ++i) {
		G1SemeruCMTaskQueue* task_queue = new G1SemeruCMTaskQueue();
		task_queue->initialize();
		_task_queues->register_queue(i, task_queue);

		_tasks[i] = new G1SemeruCMTask(i, this, task_queue, _region_mark_stats, _semeru_h->max_regions());

		_accum_task_vtime[i] = 0.0;
	}

	reset_at_marking_complete();
	_completed_initialization = true;
}


void G1SemeruConcurrentMark::reset() {
	_has_aborted = false;

	reset_marking_for_restart();

	// Reset all tasks, since different phases will use different number of active
	// threads. So, it's easiest to have all of them ready.
	for (uint i = 0; i < _max_num_tasks; ++i) {
		_tasks[i]->reset(_next_mark_bitmap);
	}

	uint max_regions = _semeru_h->max_regions();
	for (uint i = 0; i < max_regions; i++) {
		_top_at_rebuild_starts[i] = NULL;
		_region_mark_stats[i].clear();
	}
}

void G1SemeruConcurrentMark::clear_statistics_in_region(uint region_idx) {
	for (uint j = 0; j < _max_num_tasks; ++j) {
		_tasks[j]->clear_mark_stats_cache(region_idx);
	}
	_top_at_rebuild_starts[region_idx] = NULL;
	_region_mark_stats[region_idx].clear();
}

void G1SemeruConcurrentMark::clear_statistics(HeapRegion* r) {
	uint const region_idx = r->hrm_index();
	if (r->is_humongous()) {
		assert(r->is_starts_humongous(), "Got humongous continues region here");
		uint const size_in_regions = (uint)_semeru_h->humongous_obj_size_in_regions(oop(r->humongous_start_region()->bottom())->size());
		for (uint j = region_idx; j < (region_idx + size_in_regions); j++) {
			clear_statistics_in_region(j);
		}
	} else {
		clear_statistics_in_region(region_idx);
	}
}

static void clear_mark_if_set(G1CMBitMap* bitmap, HeapWord* addr) {
	if (bitmap->is_marked(addr)) {
		bitmap->clear(addr);
	}
}

void G1SemeruConcurrentMark::humongous_object_eagerly_reclaimed(HeapRegion* r) {
	assert_at_safepoint_on_vm_thread();

	// Need to clear all mark bits of the humongous object.
	clear_mark_if_set(_prev_mark_bitmap, r->bottom());
	clear_mark_if_set(_next_mark_bitmap, r->bottom());

	if (!_semeru_h->collector_state()->mark_or_rebuild_in_progress()) {
		return;
	}

	// Clear any statistics about the region gathered so far.
	clear_statistics(r);
}

void G1SemeruConcurrentMark::reset_marking_for_restart() {
	_global_mark_stack.set_empty();

	// Expand the marking stack, if we have to and if we can.
	if (has_overflown()) {
		_global_mark_stack.expand();

		uint max_regions = _semeru_h->max_regions();
		for (uint i = 0; i < max_regions; i++) {
			_region_mark_stats[i].clear_during_overflow();
		}
	}

	clear_has_overflown();
	_finger = _heap.start();

	for (uint i = 0; i < _max_num_tasks; ++i) {
		G1SemeruCMTaskQueue* queue = _task_queues->queue(i);
		queue->set_empty();
	}
}

void G1SemeruConcurrentMark::set_concurrency(uint active_tasks) {
	assert(active_tasks <= _max_num_tasks, "we should not have more");

	_num_active_tasks = active_tasks;
	// Need to update the three data structures below according to the
	// number of active threads for this phase.
	_terminator = TaskTerminator((int) active_tasks, _task_queues);
	_first_overflow_barrier_sync.set_n_workers((int) active_tasks);
	_second_overflow_barrier_sync.set_n_workers((int) active_tasks);
}


/**
 * [?] Set a flag to identify if this is a Concurrent Phase ?
 *  
 */
void G1SemeruConcurrentMark::set_concurrency_and_phase(uint active_tasks, bool concurrent) {
	set_concurrency(active_tasks);

	_concurrent = concurrent;		// g1SemeruConcurrentMark->_concurrent specify which phase we are : CM or Remark.

	if (!concurrent) {
		// At this point we should be in a STW phase, and completed marking.
		assert_at_safepoint_on_vm_thread();
		assert(out_of_regions(),
					 "only way to get here: _finger: " PTR_FORMAT ", _heap_end: " PTR_FORMAT,
					 p2i(_finger), p2i(_heap.end()));
	}
}

void G1SemeruConcurrentMark::reset_at_marking_complete() {
	// We set the global marking state to some default values when we're
	// not doing marking.
	reset_marking_for_restart();
	_num_active_tasks = 0;
}

G1SemeruConcurrentMark::~G1SemeruConcurrentMark() {
	FREE_C_HEAP_ARRAY(HeapWord*, _top_at_rebuild_starts);
	FREE_C_HEAP_ARRAY(G1RegionMarkStats, _region_mark_stats);
	// The G1SemeruConcurrentMark instance is never freed.
	ShouldNotReachHere();
}

class G1ClearBitMapTask : public AbstractGangTask {
public:
	static size_t chunk_size() { return M; }

private:
	// Heap region closure used for clearing the given mark bitmap.
	class G1ClearBitmapHRClosure : public HeapRegionClosure {
	private:
		G1CMBitMap* _bitmap;
		G1SemeruConcurrentMark* _semeru_cm;
	public:
		G1ClearBitmapHRClosure(G1CMBitMap* bitmap, G1SemeruConcurrentMark* cm) : HeapRegionClosure(), _bitmap(bitmap), _semeru_cm(cm) {
		}

		virtual bool do_heap_region(HeapRegion* r) {
			size_t const chunk_size_in_words = G1ClearBitMapTask::chunk_size() / HeapWordSize;

			HeapWord* cur = r->bottom();
			HeapWord* const end = r->end();

			while (cur < end) {
				MemRegion mr(cur, MIN2(cur + chunk_size_in_words, end));
				_bitmap->clear_range(mr);

				cur += chunk_size_in_words;

				// Abort iteration if after yielding the marking has been aborted.
				if (_semeru_cm != NULL && _semeru_cm->do_yield_check() && _semeru_cm->has_aborted()) {
					return true;
				}
				// Repeat the asserts from before the start of the closure. We will do them
				// as asserts here to minimize their overhead on the product. However, we
				// will have them as guarantees at the beginning / end of the bitmap
				// clearing to get some checking in the product.
				assert(_semeru_cm == NULL || _semeru_cm->cm_thread()->during_cycle(), "invariant");
				assert(_semeru_cm == NULL || !G1SemeruCollectedHeap::heap()->collector_state()->mark_or_rebuild_in_progress(), "invariant");
			}
			assert(cur == end, "Must have completed iteration over the bitmap for region %u.", r->hrm_index());

			return false;
		}
	};

	G1ClearBitmapHRClosure _cl;
	SemeruHeapRegionClaimer _hr_claimer;
	bool _suspendible; // If the task is suspendible, workers must join the STS.

public:
	G1ClearBitMapTask(G1CMBitMap* bitmap, G1SemeruConcurrentMark* cm, uint n_workers, bool suspendible) :
		AbstractGangTask("G1 Clear Bitmap"),
		_cl(bitmap, suspendible ? cm : NULL),
		_hr_claimer(n_workers),
		_suspendible(suspendible)
	{ }

	void work(uint worker_id) {
		SuspendibleThreadSetJoiner sts_join(_suspendible);
		G1SemeruCollectedHeap::heap()->heap_region_par_iterate_from_worker_offset(&_cl, &_hr_claimer, worker_id);
	}

	bool is_complete() {
		return _cl.is_complete();
	}
};

void G1SemeruConcurrentMark::clear_bitmap(G1CMBitMap* bitmap, WorkGang* workers, bool may_yield) {
	assert(may_yield || SafepointSynchronize::is_at_safepoint(), "Non-yielding bitmap clear only allowed at safepoint.");

	size_t const num_bytes_to_clear = (HeapRegion::GrainBytes * _semeru_h->num_regions()) / G1CMBitMap::heap_map_factor();
	size_t const num_chunks = align_up(num_bytes_to_clear, G1ClearBitMapTask::chunk_size()) / G1ClearBitMapTask::chunk_size();

	uint const num_workers = (uint)MIN2(num_chunks, (size_t)workers->active_workers());

	G1ClearBitMapTask cl(bitmap, this, num_workers, may_yield);

	log_debug(gc, ergo)("Running %s with %u workers for " SIZE_FORMAT " work units.", cl.name(), num_workers, num_chunks);
	workers->run_task(&cl, num_workers);
	guarantee(!may_yield || cl.is_complete(), "Must have completed iteration when not yielding.");
}

void G1SemeruConcurrentMark::cleanup_for_next_mark() {
	// Make sure that the concurrent mark thread looks to still be in
	// the current cycle.
	guarantee(cm_thread()->during_cycle(), "invariant");

	// We are finishing up the current cycle by clearing the next
	// marking bitmap and getting it ready for the next cycle. During
	// this time no other cycle can start. So, let's make sure that this
	// is the case.
	guarantee(!_semeru_h->collector_state()->mark_or_rebuild_in_progress(), "invariant");

	clear_bitmap(_next_mark_bitmap, _concurrent_workers, true);

	// Repeat the asserts from above.
	guarantee(cm_thread()->during_cycle(), "invariant");
	guarantee(!_semeru_h->collector_state()->mark_or_rebuild_in_progress(), "invariant");
}

void G1SemeruConcurrentMark::clear_prev_bitmap(WorkGang* workers) {
	assert_at_safepoint_on_vm_thread();
	clear_bitmap(_prev_mark_bitmap, workers, false);
}

class NoteStartOfMarkHRClosure : public HeapRegionClosure {
public:
	bool do_heap_region(HeapRegion* r) {
		r->note_start_of_marking();
		return false;
	}
};


/**
 * Tag : Why does it need to inform each region ?
 * 
 */
void G1SemeruConcurrentMark::pre_initial_mark() {
	// Initialize marking structures. This has to be done in a STW phase.
	reset();

	// For each region note start of marking.
	NoteStartOfMarkHRClosure startcl;
	_semeru_h->heap_region_iterate(&startcl);

	_root_regions.reset();
}


void G1SemeruConcurrentMark::post_initial_mark() {
	// Start Concurrent Marking weak-reference discovery.
	ReferenceProcessor* rp = _semeru_h->ref_processor_cm();
	// enable ("weak") refs discovery
	rp->enable_discovery();
	rp->setup_policy(false); // snapshot the soft ref policy to be used in this cycle

	SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
	// This is the start of  the marking cycle, we're expected all
	// threads to have SATB queues with active set to false.
	satb_mq_set.set_active_all_threads(true, /* new active value */
																		 false /* expected_active */);

	_root_regions.prepare_for_scan();

	// update_g1_committed() will be called at the end of an evac pause
	// when marking is on. So, it's also called at the end of the
	// initial-mark pause to update the heap end, if the heap expands
	// during it. No need to call it here.
}

/*
 * Notice that in the next two methods, we actually leave the STS
 * during the barrier sync and join it immediately afterwards. If we
 * do not do this, the following deadlock can occur: one thread could
 * be in the barrier sync code, waiting for the other thread to also
 * sync up, whereas another one could be trying to yield, while also
 * waiting for the other threads to sync up too.
 *
 * Note, however, that this code is also used during remark and in
 * this case we should not attempt to leave / enter the STS, otherwise
 * we'll either hit an assert (debug / fastdebug) or deadlock
 * (product). So we should only leave / enter the STS if we are
 * operating concurrently.
 *
 * Because the thread that does the sync barrier has left the STS, it
 * is possible to be suspended for a Full GC or an evacuation pause
 * could occur. This is actually safe, since the entering the sync
 * barrier is one of the last things do_marking_step() does, and it
 * doesn't manipulate any data structures afterwards.
 */

void G1SemeruConcurrentMark::enter_first_sync_barrier(uint worker_id) {
	bool barrier_aborted;
	{
		SuspendibleThreadSetLeaver sts_leave(concurrent());
		barrier_aborted = !_first_overflow_barrier_sync.enter();
	}

	// at this point everyone should have synced up and not be doing any
	// more work

	if (barrier_aborted) {
		// If the barrier aborted we ignore the overflow condition and
		// just abort the whole marking phase as quickly as possible.
		return;
	}
}

void G1SemeruConcurrentMark::enter_second_sync_barrier(uint worker_id) {
	SuspendibleThreadSetLeaver sts_leave(concurrent());
	_second_overflow_barrier_sync.enter();

	// at this point everything should be re-initialized and ready to go
}


/**
 * Semeru Memory Server - the CM, Remark task.
 * 
 * [?] This task can be excuted in both Concurrent and STW mode.
 * 
 * [?] Only concurrent tasks can execute this task.
 * 
 */
class G1SemeruCMConcurrentMarkingTask : public AbstractGangTask {
	G1SemeruConcurrentMark*     _semeru_cm;

public:
	void work(uint worker_id) {
		assert(Thread::current()->is_ConcurrentGC_thread(), "Not a concurrent GC thread");
		ResourceMark rm;			// [?] What's this resource used for ?

		double start_vtime = os::elapsedVTime();

		{
			SuspendibleThreadSetJoiner sts_join;		// [?] Design the Semeru marking thread's suspendible thread ?

			assert(worker_id < _semeru_cm->active_tasks(), "invariant");

			G1SemeruCMTask* task = _semeru_cm->task(worker_id);		// get the CM task
			task->record_start_time();			// [?] profiling ??
			if (!_semeru_cm->has_aborted()) {		// [?] aborted ? suspendible control ?
				do {
					// task->do_marking_step(G1ConcMarkStepDurationMillis,
					// 											true  /* do_termination */,
					// 											false /* is_serial*/);					// [x] Both C Marking and STW Compaction use this function.


					task->do_semeru_marking_step(G1ConcMarkStepDurationMillis,
																true  /* do_termination */,
																false /* is_serial*/);					// [x] Both C Marking and STW Compaction use this function.


					_semeru_cm->do_yield_check();		// yield for what ?

				} while (!_semeru_cm->has_aborted() && task->has_aborted());
			}

			task->record_end_time();
			guarantee(!task->has_aborted() || _semeru_cm->has_aborted(), "invariant");
		}

		double end_vtime = os::elapsedVTime();
		_semeru_cm->update_accum_task_vtime(worker_id, end_vtime - start_vtime);
	}

	G1SemeruCMConcurrentMarkingTask(G1SemeruConcurrentMark* semeru_cm) :
			AbstractGangTask("Concurrent Mark"), _semeru_cm(semeru_cm) { }

	~G1SemeruCMConcurrentMarkingTask() { }
};

/**
 * Semeru Memory Server
 *
 * 	Reserve seperate concurrent threads for Semeru heap ? 
 * 	Not share them with the original old space.	
 * 
 */
uint G1SemeruConcurrentMark::calc_active_marking_workers() {
	uint result = 0;
	if (!UseDynamicNumberOfGCThreads ||
			(!FLAG_IS_DEFAULT(ConcGCThreads) &&
			 !ForceDynamicNumberOfGCThreads)) {
		result = _max_concurrent_workers;
	} else {
		result =
			WorkerPolicy::calc_default_active_workers(_max_concurrent_workers,
																								1, /* Minimum workers */
																								_num_concurrent_workers,
																								Threads::number_of_non_daemon_threads());
		// Don't scale the result down by scale_concurrent_workers() because
		// that scaling has already gone into "_max_concurrent_workers".
	}
	assert(result > 0 && result <= _max_concurrent_workers,
				 "Calculated number of marking workers must be larger than zero and at most the maximum %u, but is %u",
				 _max_concurrent_workers, result);
	return result;
}

/**
 * Tag : CM - Root Region Scan phase
 * 
 * [?] The usage of 3 tops ?
 * 
 * Scan range [_next_top_at_mark_start, top] ?
 * 
 * 
 */
void G1SemeruConcurrentMark::scan_root_region(HeapRegion* hr, uint worker_id) {
	assert(hr->is_old() || (hr->is_survivor() && hr->next_top_at_mark_start() == hr->bottom()),
				 "Root regions must be old or survivor but region %u is %s", hr->hrm_index(), hr->get_type_str());
	G1RootRegionScanClosure cl(_semeru_h, this, worker_id);

	const uintx interval = PrefetchScanIntervalInBytes;
	HeapWord* curr = hr->next_top_at_mark_start();	// ??
	const HeapWord* end = hr->top();								// current top
	while (curr < end) {
		Prefetch::read(curr, interval);
		oop obj = oop(curr);
		int size = obj->oop_iterate_size(&cl);		// [?] Iterate the fields of this object ?
		assert(size == obj->size(), "sanity");
		curr += size;
	}
}

/**
 * Tag : Concurrent Marking - Root Region Scan phase
 * 
 * One GC thread per HeapRegion.
 * 
 */
class G1SemeruCMRootRegionScanTask : public AbstractGangTask {
	G1SemeruConcurrentMark* _semeru_cm;
public:
	G1SemeruCMRootRegionScanTask(G1SemeruConcurrentMark* cm) :
		AbstractGangTask("G1 Root Region Scan"), _semeru_cm(cm) { }

	void work(uint worker_id) {
		assert(Thread::current()->is_ConcurrentGC_thread(),
					 "this should only be done by a conc GC thread");

		G1SemeruCMRootRegions* root_regions = _semeru_cm->root_regions();
		HeapRegion* hr = root_regions->claim_next();
		while (hr != NULL) {
			_semeru_cm->scan_root_region(hr, worker_id);
			hr = root_regions->claim_next();
		}
	}
};



/**
 * Semeru Memory Server --	Scan a single Region. 
 * 		Concurrent scavenge,
 * 		Start from a HeapRegion's Target Object Queue.
 * 		Mark the alive objects in the HeapRegion's alive_bitmap.
 * 
 * 	[?] How to assign the HeapRegion to the Semeru CM Task ?
 * 
 */
void G1SemeruConcurrentMark::semeru_concurrent_mark_a_region( HeapRegion* region_to_scan) {
	_restart_for_overflow = false;		// freshly scan, not Remark

	_num_concurrent_workers = calc_active_marking_workers();

	uint active_workers = MAX2(1U, _num_concurrent_workers);

	// Setting active workers is not guaranteed since fewer
	// worker threads may currently exist and more may not be
	// available.
	active_workers = _concurrent_workers->update_active_workers(active_workers);
	log_info(gc, task)("Using %u workers of %u for marking", active_workers, _concurrent_workers->total_workers());

	// Parallel task terminator is set in "set_concurrency_and_phase()"
	set_concurrency_and_phase(active_workers, true /* concurrent */);

	G1SemeruCMConcurrentMarkingTask marking_task(this);
	_concurrent_workers->run_task(&marking_task);
	print_stats();
}


/**
 * Semeru Memory Server - Compact a Region.
 * 		Do the compaction in STW.
 * 		
 * 	
 */
void G1SemeruConcurrentMark::semeru_stw_compact_a_region( HeapRegion* region_to_compact) {

	//debug
	tty->print("%s, Not implement yet. \n", __func__);



}



/**
 * Semeru scans from each Region's target object queue as Root.
 * There is no Root Region Scan Phase.
 */
void G1SemeruConcurrentMark::scan_root_regions() {
	// scan_in_progress() will have been set to true only if there was
	// at least one root region to scan. So, if it's false, we
	// should not attempt to do any further work.
	if (root_regions()->scan_in_progress()) {
		assert(!has_aborted(), "Aborting before root region scanning is finished not supported.");

		_num_concurrent_workers = MIN2(calc_active_marking_workers(),
																	 // We distribute work on a per-region basis, so starting
																	 // more threads than that is useless.
																	 root_regions()->num_root_regions());
		assert(_num_concurrent_workers <= _max_concurrent_workers,
					 "Maximum number of marking threads exceeded");

		G1SemeruCMRootRegionScanTask task(this);
		log_debug(gc, ergo)("Running %s using %u workers for %u work units.",
												task.name(), _num_concurrent_workers, root_regions()->num_root_regions());
		_concurrent_workers->run_task(&task, _num_concurrent_workers);

		// It's possible that has_aborted() is true here without actually
		// aborting the survivor scan earlier. This is OK as it's
		// mainly used for sanity checking.
		root_regions()->scan_finished();
	}
}

void G1SemeruConcurrentMark::concurrent_cycle_start() {
	_gc_timer_cm->register_gc_start();

	_gc_tracer_cm->report_gc_start(GCCause::_no_gc /* first parameter is not used */, _gc_timer_cm->gc_start());

	_semeru_h->trace_heap_before_gc(_gc_tracer_cm);  // [?] What does this mean ??
}

void G1SemeruConcurrentMark::concurrent_cycle_end() {
	_semeru_h->collector_state()->set_clearing_next_bitmap(false);

	_semeru_h->trace_heap_after_gc(_gc_tracer_cm);

	if (has_aborted()) {
		log_info(gc, marking)("Concurrent Mark Abort");
		_gc_tracer_cm->report_concurrent_mode_failure();
	}

	_gc_timer_cm->register_gc_end();

	_gc_tracer_cm->report_gc_end(_gc_timer_cm->gc_end(), _gc_timer_cm->time_partitions());
}

void G1SemeruConcurrentMark::mark_from_roots() {
	_restart_for_overflow = false;

	_num_concurrent_workers = calc_active_marking_workers();

	uint active_workers = MAX2(1U, _num_concurrent_workers);

	// Setting active workers is not guaranteed since fewer
	// worker threads may currently exist and more may not be
	// available.
	active_workers = _concurrent_workers->update_active_workers(active_workers);
	log_info(gc, task)("Using %u workers of %u for marking", active_workers, _concurrent_workers->total_workers());

	// Parallel task terminator is set in "set_concurrency_and_phase()"
	set_concurrency_and_phase(active_workers, true /* concurrent */);

	G1SemeruCMConcurrentMarkingTask marking_task(this);
	_concurrent_workers->run_task(&marking_task);
	print_stats();
}

/**
 * Semeru memory server - Concurent Marking phase
 * 
 * 		The first marking pass for a fresh Region evicted in memory server.
 * 
 */ 
void G1SemeruConcurrentMark::semeru_concurrent_marking() {

	//debug
	tty->print("%s, this is the main concurrent marking phase for Semeru memory server. NOT mark_from_too. \n", __func__);

	_restart_for_overflow = false;

	_num_concurrent_workers = calc_active_marking_workers();

	uint active_workers = MAX2(1U, _num_concurrent_workers);

	// Setting active workers is not guaranteed since fewer
	// worker threads may currently exist and more may not be
	// available.
	active_workers = _concurrent_workers->update_active_workers(active_workers);
	log_info(gc, task)("Using %u workers of %u for CM marking", active_workers, _concurrent_workers->total_workers());

	// Parallel task terminator is set in "set_concurrency_and_phase()"
	set_concurrency_and_phase(active_workers, true /* concurrent */);

	G1SemeruCMConcurrentMarkingTask marking_task(this);
	_concurrent_workers->run_task(&marking_task);
	print_stats();
}



void G1SemeruConcurrentMark::verify_during_pause(G1HeapVerifier::G1VerifyType type, VerifyOption vo, const char* caller) {
	G1HeapVerifier* verifier = _semeru_h->verifier();

	 verifier->verify_region_sets_optional();

	if (VerifyDuringGC) {
		GCTraceTime(Debug, gc, phases) debug(caller, _gc_timer_cm);

		size_t const BufLen = 512;
		char buffer[BufLen];

		jio_snprintf(buffer, BufLen, "During GC (%s)", caller);
		verifier->verify(type, vo, buffer);
	}

	verifier->check_bitmaps(caller);
}

class G1SemeruUpdateRemSetTrackingBeforeRebuildTask : public AbstractGangTask {
	G1SemeruCollectedHeap* _semeru_h;
	G1SemeruConcurrentMark* _semeru_cm;
	SemeruHeapRegionClaimer _hrclaimer;
	uint volatile _total_selected_for_rebuild;

	G1SemeruPrintRegionLivenessInfoClosure _cl;

	class G1SemeruUpdateRemSetTrackingBeforeRebuild : public HeapRegionClosure {
		G1SemeruCollectedHeap* _semeru_h;
		G1SemeruConcurrentMark* _semeru_cm;

		G1SemeruPrintRegionLivenessInfoClosure* _cl;

		uint _num_regions_selected_for_rebuild;  // The number of regions actually selected for rebuild.


		/**
		 * Tag : Select the Region to Rebuild its RemSet.
		 * 	Selection polocy:
		 * 			humongous region : 
		 * 			normal region : Live ratio is low. Check details in function update_before_rebuild()
		 * 											[?] The selected Regions all marked as CSet ?
		 * 
		 * Update all Old Region's top_at_rebuild_start.
		 */
		void update_remset_before_rebuild(HeapRegion* hr) {
			G1RemSetTrackingPolicy* tracking_policy = _semeru_h->g1_policy()->remset_tracker();

			bool selected_for_rebuild;
			if (hr->is_humongous()) {
				bool const is_live = _semeru_cm->liveness(hr->humongous_start_region()->hrm_index()) > 0;
				selected_for_rebuild = tracking_policy->update_humongous_before_rebuild(hr, is_live);
			} else {
				size_t const live_bytes = _semeru_cm->liveness(hr->hrm_index());
				selected_for_rebuild = tracking_policy->update_before_rebuild(hr, live_bytes);
			}
			if (selected_for_rebuild) {
				_num_regions_selected_for_rebuild++;
			}

			// Even not select this Region to rebuild its RemSet, update its _top_at_rebuild_start.
			_semeru_cm->update_top_at_rebuild_start(hr);			
		}

		// Distribute the given words across the humongous object starting with hr and
		// note end of marking.
		void distribute_marked_bytes(HeapRegion* hr, size_t marked_words) {
			uint const region_idx = hr->hrm_index();
			size_t const obj_size_in_words = (size_t)oop(hr->bottom())->size();
			uint const num_regions_in_humongous = (uint)G1SemeruCollectedHeap::humongous_obj_size_in_regions(obj_size_in_words);

			// "Distributing" zero words means that we only note end of marking for these
			// regions.
			assert(marked_words == 0 || obj_size_in_words == marked_words,
						 "Marked words should either be 0 or the same as humongous object (" SIZE_FORMAT ") but is " SIZE_FORMAT,
						 obj_size_in_words, marked_words);

			for (uint i = region_idx; i < (region_idx + num_regions_in_humongous); i++) {
				HeapRegion* const r = _semeru_h->region_at(i);
				size_t const words_to_add = MIN2(HeapRegion::GrainWords, marked_words);

				log_trace(gc, marking)("Adding " SIZE_FORMAT " words to humongous region %u (%s)",
															 words_to_add, i, r->get_type_str());
				add_marked_bytes_and_note_end(r, words_to_add * HeapWordSize);
				marked_words -= words_to_add;
			}
			assert(marked_words == 0,
						 SIZE_FORMAT " words left after distributing space across %u regions",
						 marked_words, num_regions_in_humongous);
		}

		void update_marked_bytes(HeapRegion* hr) {
			uint const region_idx = hr->hrm_index();
			size_t const marked_words = _semeru_cm->liveness(region_idx);
			// The marking attributes the object's size completely to the humongous starts
			// region. We need to distribute this value across the entire set of regions a
			// humongous object spans.
			if (hr->is_humongous()) {
				assert(hr->is_starts_humongous() || marked_words == 0,
							 "Should not have marked words " SIZE_FORMAT " in non-starts humongous region %u (%s)",
							 marked_words, region_idx, hr->get_type_str());
				if (hr->is_starts_humongous()) {
					distribute_marked_bytes(hr, marked_words);
				}
			} else {
				log_trace(gc, marking)("Adding " SIZE_FORMAT " words to region %u (%s)", marked_words, region_idx, hr->get_type_str());
				add_marked_bytes_and_note_end(hr, marked_words * HeapWordSize);
			}
		}

		void add_marked_bytes_and_note_end(HeapRegion* hr, size_t marked_bytes) {
			hr->add_to_marked_bytes(marked_bytes);
			_cl->do_heap_region(hr);
			hr->note_end_of_marking();
		}

	public:
		G1SemeruUpdateRemSetTrackingBeforeRebuild(G1SemeruCollectedHeap* g1h, G1SemeruConcurrentMark* cm, G1SemeruPrintRegionLivenessInfoClosure* cl) :
			_semeru_h(g1h), _semeru_cm(cm), _cl(cl), _num_regions_selected_for_rebuild(0) { }

		virtual bool do_heap_region(HeapRegion* r) {
			update_remset_before_rebuild(r);
			update_marked_bytes(r);

			return false;
		}

		uint num_selected_for_rebuild() const { return _num_regions_selected_for_rebuild; }
	};

public:
	G1SemeruUpdateRemSetTrackingBeforeRebuildTask(G1SemeruCollectedHeap* g1h, G1SemeruConcurrentMark* cm, uint num_workers) :
		AbstractGangTask("G1 Update RemSet Tracking Before Rebuild"),
		_semeru_h(g1h), _semeru_cm(cm), _hrclaimer(num_workers), _total_selected_for_rebuild(0), _cl("Post-Marking") { }

	virtual void work(uint worker_id) {
		G1SemeruUpdateRemSetTrackingBeforeRebuild update_cl(_semeru_h, _semeru_cm, &_cl);
		_semeru_h->heap_region_par_iterate_from_worker_offset(&update_cl, &_hrclaimer, worker_id);
		Atomic::add(update_cl.num_selected_for_rebuild(), &_total_selected_for_rebuild);
	}

	uint total_selected_for_rebuild() const { return _total_selected_for_rebuild; }

	// Number of regions for which roughly one thread should be spawned for this work.
	static const uint RegionsPerThread = 384;
};

class G1SemeruUpdateRemSetTrackingAfterRebuild : public HeapRegionClosure {
	G1SemeruCollectedHeap* _semeru_h;
public:
	G1SemeruUpdateRemSetTrackingAfterRebuild(G1SemeruCollectedHeap* g1h) : _semeru_h(g1h) { }

	virtual bool do_heap_region(HeapRegion* r) {
		_semeru_h->g1_policy()->remset_tracker()->update_after_rebuild(r);
		return false;
	}
};



/**
 * Tag : Remark the alive objects in Old Region between the end of last CM and Cleanup phase.
 * 
 * [?] What's the root for the Remark phase ?
 * 		new stack frames && new dirty cards ?   
 * 		Only care about the cross-region reference into Tracked Regions ?
 * 
 */
void G1SemeruConcurrentMark::remark() {
	assert_at_safepoint_on_vm_thread();

	// If a full collection has happened, we should not continue. However we might
	// have ended up here as the Remark VM operation has been scheduled already.
	if (has_aborted()) {
		return;
	}

	G1Policy* g1p = _semeru_h->g1_policy();
	g1p->record_concurrent_mark_remark_start();

	double start = os::elapsedTime();

	verify_during_pause(G1HeapVerifier::G1VerifyRemark, VerifyOption_G1UsePrevMarking, "Remark before");

	// Do the remark action.
	{
		GCTraceTime(Debug, gc, phases) debug("Finalize Marking", _gc_timer_cm);
		finalize_marking();
	}

	double mark_work_end = os::elapsedTime();

	bool const mark_finished = !has_overflown();
	if (mark_finished) {
		weak_refs_work(false /* clear_all_soft_refs */);

		SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
		// We're done with marking.
		// This is the end of the marking cycle, we're expected all
		// threads to have SATB queues with active set to true.
		satb_mq_set.set_active_all_threads(false, /* new active value */
																			 true /* expected_active */);

		{
			GCTraceTime(Debug, gc, phases) debug("Flush Task Caches", _gc_timer_cm);
			flush_all_task_caches();
		}

		// Install newly created mark bitmap as "prev".
		swap_mark_bitmaps();
		{
			GCTraceTime(Debug, gc, phases) debug("Update Remembered Set Tracking Before Rebuild", _gc_timer_cm);

			uint const workers_by_capacity = (_semeru_h->num_regions() + G1SemeruUpdateRemSetTrackingBeforeRebuildTask::RegionsPerThread - 1) /
																			 G1SemeruUpdateRemSetTrackingBeforeRebuildTask::RegionsPerThread;
			uint const num_workers = MIN2(_semeru_h->workers()->active_workers(), workers_by_capacity);

			G1SemeruUpdateRemSetTrackingBeforeRebuildTask cl(_semeru_h, this, num_workers);
			log_debug(gc,ergo)("Running %s using %u workers for %u regions in heap", cl.name(), num_workers, _semeru_h->num_regions());
			_semeru_h->workers()->run_task(&cl, num_workers);

			log_debug(gc, remset, tracking)("Remembered Set Tracking update regions total %u, selected %u",
																			_semeru_h->num_regions(), cl.total_selected_for_rebuild());
		}
		{
			GCTraceTime(Debug, gc, phases) debug("Reclaim Empty Regions", _gc_timer_cm);
			reclaim_empty_regions();
		}

		// Clean out dead classes
		if (ClassUnloadingWithConcurrentMark) {
			GCTraceTime(Debug, gc, phases) debug("Purge Metaspace", _gc_timer_cm);
			ClassLoaderDataGraph::purge();
		}

		_semeru_h->resize_heap_if_necessary();

		compute_new_sizes();

		verify_during_pause(G1HeapVerifier::G1VerifyRemark, VerifyOption_G1UsePrevMarking, "Remark after");

		assert(!restart_for_overflow(), "sanity");
		// Completely reset the marking state since marking completed
		reset_at_marking_complete();
	} else {
		// We overflowed.  Restart concurrent marking.
		_restart_for_overflow = true;

		verify_during_pause(G1HeapVerifier::G1VerifyRemark, VerifyOption_G1UsePrevMarking, "Remark overflow");

		// Clear the marking state because we will be restarting
		// marking due to overflowing the global mark stack.
		reset_marking_for_restart();
	}

	{
		GCTraceTime(Debug, gc, phases) debug("Report Object Count", _gc_timer_cm);
		report_object_count(mark_finished);
	}

	// Statistics
	double now = os::elapsedTime();
	_remark_mark_times.add((mark_work_end - start) * 1000.0);
	_remark_weak_ref_times.add((now - mark_work_end) * 1000.0);
	_remark_times.add((now - start) * 1000.0);

	g1p->record_concurrent_mark_remark_end();
}

class G1SemeruReclaimEmptyRegionsTask : public AbstractGangTask {
	// Per-region work during the Cleanup pause.
	class G1ReclaimEmptyRegionsClosure : public HeapRegionClosure {
		G1SemeruCollectedHeap* _semeru_h;
		size_t _freed_bytes;
		FreeRegionList* _local_cleanup_list;
		uint _old_regions_removed;
		uint _humongous_regions_removed;

	public:
		G1ReclaimEmptyRegionsClosure(G1SemeruCollectedHeap* g1h,
																 FreeRegionList* local_cleanup_list) :
			_semeru_h(g1h),
			_freed_bytes(0),
			_local_cleanup_list(local_cleanup_list),
			_old_regions_removed(0),
			_humongous_regions_removed(0) { }

		size_t freed_bytes() { return _freed_bytes; }
		const uint old_regions_removed() { return _old_regions_removed; }
		const uint humongous_regions_removed() { return _humongous_regions_removed; }

		bool do_heap_region(HeapRegion *hr) {
			if (hr->used() > 0 && hr->max_live_bytes() == 0 && !hr->is_young() && !hr->is_archive()) {
				_freed_bytes += hr->used();
				hr->set_containing_set(NULL);
				if (hr->is_humongous()) {
					_humongous_regions_removed++;
					_semeru_h->free_humongous_region(hr, _local_cleanup_list);
				} else {
					_old_regions_removed++;
					_semeru_h->free_region(hr, _local_cleanup_list, false /* skip_remset */, false /* skip_hcc */, true /* locked */);
				}
				hr->clear_cardtable();
				_semeru_h->concurrent_mark()->clear_statistics_in_region(hr->hrm_index());
				log_trace(gc)("Reclaimed empty region %u (%s) bot " PTR_FORMAT, hr->hrm_index(), hr->get_short_type_str(), p2i(hr->bottom()));
			}

			return false;
		}
	};

	G1SemeruCollectedHeap* _semeru_h;
	FreeRegionList* _cleanup_list;
	SemeruHeapRegionClaimer _hrclaimer;

public:
	G1SemeruReclaimEmptyRegionsTask(G1SemeruCollectedHeap* g1h, FreeRegionList* cleanup_list, uint n_workers) :
		AbstractGangTask("G1 Cleanup"),
		_semeru_h(g1h),
		_cleanup_list(cleanup_list),
		_hrclaimer(n_workers) {
	}

	void work(uint worker_id) {
		FreeRegionList local_cleanup_list("Local Cleanup List");
		G1ReclaimEmptyRegionsClosure cl(_semeru_h, &local_cleanup_list);
		_semeru_h->heap_region_par_iterate_from_worker_offset(&cl, &_hrclaimer, worker_id);
		assert(cl.is_complete(), "Shouldn't have aborted!");

		// Now update the old/humongous region sets
		_semeru_h->remove_from_old_sets(cl.old_regions_removed(), cl.humongous_regions_removed());
		{
			MutexLockerEx x(ParGCRareEvent_lock, Mutex::_no_safepoint_check_flag);
			_semeru_h->decrement_summary_bytes(cl.freed_bytes());

			_cleanup_list->add_ordered(&local_cleanup_list);
			assert(local_cleanup_list.is_empty(), "post-condition");
		}
	}
};

void G1SemeruConcurrentMark::reclaim_empty_regions() {
	WorkGang* workers = _semeru_h->workers();
	FreeRegionList empty_regions_list("Empty Regions After Mark List");

	G1SemeruReclaimEmptyRegionsTask cl(_semeru_h, &empty_regions_list, workers->active_workers());
	workers->run_task(&cl);

	if (!empty_regions_list.is_empty()) {
		log_debug(gc)("Reclaimed %u empty regions", empty_regions_list.length());
		// Now print the empty regions list.
		G1HRPrinter* hrp = _semeru_h->hr_printer();
		if (hrp->is_active()) {
			FreeRegionListIterator iter(&empty_regions_list);
			while (iter.more_available()) {
				HeapRegion* hr = iter.get_next();
				hrp->cleanup(hr);
			}
		}
		// And actually make them available.
		_semeru_h->prepend_to_freelist(&empty_regions_list);
	}
}

void G1SemeruConcurrentMark::compute_new_sizes() {
	MetaspaceGC::compute_new_size();

	// Cleanup will have freed any regions completely full of garbage.
	// Update the soft reference policy with the new heap occupancy.
	Universe::update_heap_info_at_gc();

	// We reclaimed old regions so we should calculate the sizes to make
	// sure we update the old gen/space data.
	_semeru_h->g1mm()->update_sizes();
}

void G1SemeruConcurrentMark::cleanup() {
	assert_at_safepoint_on_vm_thread();

	// If a full collection has happened, we shouldn't do this.
	if (has_aborted()) {
		return;
	}

	G1Policy* g1p = _semeru_h->g1_policy();
	g1p->record_concurrent_mark_cleanup_start();

	double start = os::elapsedTime();

	verify_during_pause(G1HeapVerifier::G1VerifyCleanup, VerifyOption_G1UsePrevMarking, "Cleanup before");

	{
		GCTraceTime(Debug, gc, phases) debug("Update Remembered Set Tracking After Rebuild", _gc_timer_cm);
		G1SemeruUpdateRemSetTrackingAfterRebuild cl(_semeru_h);
		_semeru_h->heap_region_iterate(&cl);
	}

	if (log_is_enabled(Trace, gc, liveness)) {
		G1SemeruPrintRegionLivenessInfoClosure cl("Post-Cleanup");
		_semeru_h->heap_region_iterate(&cl);
	}

	verify_during_pause(G1HeapVerifier::G1VerifyCleanup, VerifyOption_G1UsePrevMarking, "Cleanup after");

	// We need to make this be a "collection" so any collection pause that
	// races with it goes around and waits for Cleanup to finish.
	_semeru_h->increment_total_collections();

	// Local statistics
	double recent_cleanup_time = (os::elapsedTime() - start);
	_total_cleanup_time += recent_cleanup_time;
	_cleanup_times.add(recent_cleanup_time);

	{
		GCTraceTime(Debug, gc, phases) debug("Finalize Concurrent Mark Cleanup", _gc_timer_cm);
		_semeru_h->g1_policy()->record_concurrent_mark_cleanup_end();
	}
}

// 'Keep Alive' oop closure used by both serial parallel reference processing.
// Uses the G1SemeruCMTask associated with a worker thread (for serial reference
// processing the G1SemeruCMTask for worker 0 is used) to preserve (mark) and
// trace referent objects.
//
// Using the G1SemeruCMTask and embedded local queues avoids having the worker
// threads operating on the global mark stack. This reduces the risk
// of overflowing the stack - which we would rather avoid at this late
// state. Also using the tasks' local queues removes the potential
// of the workers interfering with each other that could occur if
// operating on the global stack.

class G1SemeruCMKeepAliveAndDrainClosure : public OopClosure {
	G1SemeruConcurrentMark* _semeru_cm;
	G1SemeruCMTask*         _task;
	uint              _ref_counter_limit;
	uint              _ref_counter;
	bool              _is_serial;
public:
	G1SemeruCMKeepAliveAndDrainClosure(G1SemeruConcurrentMark* cm, G1SemeruCMTask* task, bool is_serial) :
		_semeru_cm(cm), _task(task), _ref_counter_limit(G1RefProcDrainInterval),
		_ref_counter(_ref_counter_limit), _is_serial(is_serial) {
		assert(!_is_serial || _task->worker_id() == 0, "only task 0 for serial code");
	}

	virtual void do_oop(narrowOop* p) { do_oop_work(p); }
	virtual void do_oop(      oop* p) { do_oop_work(p); }

	template <class T> void do_oop_work(T* p) {
		if (_semeru_cm->has_overflown()) {
			return;
		}
		if (!_task->deal_with_reference(p)) {
			// We did not add anything to the mark bitmap (or mark stack), so there is
			// no point trying to drain it.
			return;
		}
		_ref_counter--;

		if (_ref_counter == 0) {
			// We have dealt with _ref_counter_limit references, pushing them
			// and objects reachable from them on to the local stack (and
			// possibly the global stack). Call G1SemeruCMTask::do_marking_step() to
			// process these entries.
			//
			// We call G1SemeruCMTask::do_marking_step() in a loop, which we'll exit if
			// there's nothing more to do (i.e. we're done with the entries that
			// were pushed as a result of the G1SemeruCMTask::deal_with_reference() calls
			// above) or we overflow.
			//
			// Note: G1SemeruCMTask::do_marking_step() can set the G1SemeruCMTask::has_aborted()
			// flag while there may still be some work to do. (See the comment at
			// the beginning of G1SemeruCMTask::do_marking_step() for those conditions -
			// one of which is reaching the specified time target.) It is only
			// when G1SemeruCMTask::do_marking_step() returns without setting the
			// has_aborted() flag that the marking step has completed.
			do {
				double mark_step_duration_ms = G1ConcMarkStepDurationMillis;
				_task->do_marking_step(mark_step_duration_ms,
															 false      /* do_termination */,
															 _is_serial);
			} while (_task->has_aborted() && !_semeru_cm->has_overflown());
			_ref_counter = _ref_counter_limit;
		}
	}
};

// 'Drain' oop closure used by both serial and parallel reference processing.
// Uses the G1SemeruCMTask associated with a given worker thread (for serial
// reference processing the G1CMtask for worker 0 is used). Calls the
// do_marking_step routine, with an unbelievably large timeout value,
// to drain the marking data structures of the remaining entries
// added by the 'keep alive' oop closure above.
//
//	[?] What's this closure used for ??
//		=> weak reference related ?
//
class G1SemeruCMDrainMarkingStackClosure : public VoidClosure {
	G1SemeruConcurrentMark* _semeru_cm;
	G1SemeruCMTask*         _task;
	bool              			_is_serial;
 public:
	G1SemeruCMDrainMarkingStackClosure(G1SemeruConcurrentMark* cm, G1SemeruCMTask* task, bool is_serial) :
		_semeru_cm(cm), _task(task), _is_serial(is_serial) {
		assert(!_is_serial || _task->worker_id() == 0, "only task 0 for serial code");
	}

	void do_void() {
		do {
			// We call G1SemeruCMTask::do_marking_step() to completely drain the local
			// and global marking stacks of entries pushed by the 'keep alive'
			// oop closure (an instance of G1SemeruCMKeepAliveAndDrainClosure above).
			//
			// G1SemeruCMTask::do_marking_step() is called in a loop, which we'll exit
			// if there's nothing more to do (i.e. we've completely drained the
			// entries that were pushed as a a result of applying the 'keep alive'
			// closure to the entries on the discovered ref lists) or we overflow
			// the global marking stack.
			//
			// Note: G1SemeruCMTask::do_marking_step() can set the G1SemeruCMTask::has_aborted()
			// flag while there may still be some work to do. (See the comment at
			// the beginning of G1SemeruCMTask::do_marking_step() for those conditions -
			// one of which is reaching the specified time target.) It is only
			// when G1SemeruCMTask::do_marking_step() returns without setting the
			// has_aborted() flag that the marking step has completed.

			_task->do_marking_step(1000000000.0 /* something very large */,
														 true         /* do_termination */,
														 _is_serial);


		} while (_task->has_aborted() && !_semeru_cm->has_overflown());
	}
};

// Implementation of AbstractRefProcTaskExecutor for parallel
// reference processing at the end of G1 concurrent marking

class G1SemeruCMRefProcTaskExecutor : public AbstractRefProcTaskExecutor {
private:
	G1SemeruCollectedHeap*  _semeru_h;
	G1SemeruConcurrentMark* _semeru_cm;
	WorkGang*         _workers;
	uint              _active_workers;

public:
	G1SemeruCMRefProcTaskExecutor(G1SemeruCollectedHeap* g1h,
													G1SemeruConcurrentMark* cm,
													WorkGang* workers,
													uint n_workers) :
		_semeru_h(g1h), _semeru_cm(cm),
		_workers(workers), _active_workers(n_workers) { }

	virtual void execute(ProcessTask& task, uint ergo_workers);
};

class G1SemeruCMRefProcTaskProxy : public AbstractGangTask {
	typedef AbstractRefProcTaskExecutor::ProcessTask ProcessTask;
	ProcessTask&      _proc_task;
	G1SemeruCollectedHeap*  _semeru_h;
	G1SemeruConcurrentMark* _semeru_cm;

public:
	G1SemeruCMRefProcTaskProxy(ProcessTask& proc_task,
											 G1SemeruCollectedHeap* g1h,
											 G1SemeruConcurrentMark* cm) :
		AbstractGangTask("Process reference objects in parallel"),
		_proc_task(proc_task), _semeru_h(g1h), _semeru_cm(cm) {
		ReferenceProcessor* rp = _semeru_h->ref_processor_cm();
		assert(rp->processing_is_mt(), "shouldn't be here otherwise");
	}

	virtual void work(uint worker_id) {
		
		// Warning;
		printf("Error in %s, please fix me. \n", __func__);
		
		// ResourceMark rm;
		// HandleMark hm;
		// G1SemeruCMTask* task = _semeru_cm->task(worker_id);
		// G1SemeruCMIsAliveClosure g1_is_alive(_semeru_h);
		// G1SemeruCMKeepAliveAndDrainClosure g1_par_keep_alive(_semeru_cm, task, false /* is_serial */);
		// G1SemeruCMDrainMarkingStackClosure g1_par_drain(_semeru_cm, task, false /* is_serial */);

		//_proc_task.work(worker_id, g1_is_alive, g1_par_keep_alive, g1_par_drain);
	}
};

void G1SemeruCMRefProcTaskExecutor::execute(ProcessTask& proc_task, uint ergo_workers) {
	assert(_workers != NULL, "Need parallel worker threads.");
	assert(_semeru_h->ref_processor_cm()->processing_is_mt(), "processing is not MT");
	assert(_workers->active_workers() >= ergo_workers,
				 "Ergonomically chosen workers(%u) should be less than or equal to active workers(%u)",
				 ergo_workers, _workers->active_workers());

	G1SemeruCMRefProcTaskProxy proc_task_proxy(proc_task, _semeru_h, _semeru_cm);

	// We need to reset the concurrency level before each
	// proxy task execution, so that the termination protocol
	// and overflow handling in G1SemeruCMTask::do_marking_step() knows
	// how many workers to wait for.
	_semeru_cm->set_concurrency(ergo_workers);
	_workers->run_task(&proc_task_proxy, ergo_workers);
}


/**
 * Semeru Memory Server
 * [?] Does the concurrent GC also need to trace the weak reference ?? 
 * 
 */
void G1SemeruConcurrentMark::weak_refs_work(bool clear_all_soft_refs) {
	ResourceMark rm;
	HandleMark   hm;

	// Is alive closure.
	G1SemeruCMIsAliveClosure g1_is_alive(_semeru_h);

	// Inner scope to exclude the cleaning of the string table
	// from the displayed time.
	{
		GCTraceTime(Debug, gc, phases) debug("Reference Processing", _gc_timer_cm);

		ReferenceProcessor* rp = _semeru_h->ref_processor_cm();

		// See the comment in G1SemeruCollectedHeap::ref_processing_init()
		// about how reference processing currently works in G1.

		// Set the soft reference policy
		rp->setup_policy(clear_all_soft_refs);
		assert(_global_mark_stack.is_empty(), "mark stack should be empty");

		// Instances of the 'Keep Alive' and 'Complete GC' closures used
		// in serial reference processing. Note these closures are also
		// used for serially processing (by the the current thread) the
		// JNI references during parallel reference processing.
		//
		// These closures do not need to synchronize with the worker
		// threads involved in parallel reference processing as these
		// instances are executed serially by the current thread (e.g.
		// reference processing is not multi-threaded and is thus
		// performed by the current thread instead of a gang worker).
		//
		// The gang tasks involved in parallel reference processing create
		// their own instances of these closures, which do their own
		// synchronization among themselves.
		G1SemeruCMKeepAliveAndDrainClosure g1_keep_alive(this, task(0), true /* is_serial */);
		G1SemeruCMDrainMarkingStackClosure g1_drain_mark_stack(this, task(0), true /* is_serial */);

		// We need at least one active thread. If reference processing
		// is not multi-threaded we use the current (VMThread) thread,
		// otherwise we use the work gang from the G1SemeruCollectedHeap and
		// we utilize all the worker threads we can.
		bool processing_is_mt = rp->processing_is_mt();
		uint active_workers = (processing_is_mt ? _semeru_h->workers()->active_workers() : 1U);
		active_workers = MAX2(MIN2(active_workers, _max_num_tasks), 1U);

		// Parallel processing task executor.
		G1SemeruCMRefProcTaskExecutor par_task_executor(_semeru_h, this,
																							_semeru_h->workers(), active_workers);
		AbstractRefProcTaskExecutor* executor = (processing_is_mt ? &par_task_executor : NULL);

		// Set the concurrency level. The phase was already set prior to
		// executing the remark task.
		set_concurrency(active_workers);

		// Set the degree of MT processing here.  If the discovery was done MT,
		// the number of threads involved during discovery could differ from
		// the number of active workers.  This is OK as long as the discovered
		// Reference lists are balanced (see balance_all_queues() and balance_queues()).
		rp->set_active_mt_degree(active_workers);

		ReferenceProcessorPhaseTimes pt(_gc_timer_cm, rp->max_num_queues());

		// Process the weak references.
		const ReferenceProcessorStats& stats =
				rp->process_discovered_references(&g1_is_alive,
																					&g1_keep_alive,
																					&g1_drain_mark_stack,
																					executor,
																					&pt);
		_gc_tracer_cm->report_gc_reference_stats(stats);
		pt.print_all_references();

		// The do_oop work routines of the keep_alive and drain_marking_stack
		// oop closures will set the has_overflown flag if we overflow the
		// global marking stack.

		assert(has_overflown() || _global_mark_stack.is_empty(),
					 "Mark stack should be empty (unless it has overflown)");

		assert(rp->num_queues() == active_workers, "why not");

		rp->verify_no_references_recorded();
		assert(!rp->discovery_enabled(), "Post condition");
	}

	if (has_overflown()) {
		// We can not trust g1_is_alive and the contents of the heap if the marking stack
		// overflowed while processing references. Exit the VM.
		fatal("Overflow during reference processing, can not continue. Please "
					"increase MarkStackSizeMax (current value: " SIZE_FORMAT ") and "
					"restart.", MarkStackSizeMax);
		return;
	}

	assert(_global_mark_stack.is_empty(), "Marking should have completed");

	{
		GCTraceTime(Debug, gc, phases) debug("Weak Processing", _gc_timer_cm);
		WeakProcessor::weak_oops_do(_semeru_h->workers(), &g1_is_alive, &do_nothing_cl, 1);
	}

	// Unload Klasses, String, Code Cache, etc.
	if (ClassUnloadingWithConcurrentMark) {
		GCTraceTime(Debug, gc, phases) debug("Class Unloading", _gc_timer_cm);
		bool purged_classes = SystemDictionary::do_unloading(_gc_timer_cm);
		_semeru_h->complete_cleaning(&g1_is_alive, purged_classes);
	} else {
		GCTraceTime(Debug, gc, phases) debug("Cleanup", _gc_timer_cm);
		// No need to clean string table as it is treated as strong roots when
		// class unloading is disabled.
		_semeru_h->partial_cleaning(&g1_is_alive, false, G1StringDedup::is_enabled());
	}
}

class G1PrecleanYieldClosure : public YieldClosure {
	G1SemeruConcurrentMark* _semeru_cm;

public:
	G1PrecleanYieldClosure(G1SemeruConcurrentMark* cm) : _semeru_cm(cm) { }

	virtual bool should_return() {
		return _semeru_cm->has_aborted();
	}

	virtual bool should_return_fine_grain() {
		_semeru_cm->do_yield_check();
		return _semeru_cm->has_aborted();
	}
};

void G1SemeruConcurrentMark::preclean() {
	assert(G1UseReferencePrecleaning, "Precleaning must be enabled.");

	SuspendibleThreadSetJoiner joiner;

	G1SemeruCMKeepAliveAndDrainClosure keep_alive(this, task(0), true /* is_serial */);
	G1SemeruCMDrainMarkingStackClosure drain_mark_stack(this, task(0), true /* is_serial */);

	set_concurrency_and_phase(1, true);

	G1PrecleanYieldClosure yield_cl(this);

	ReferenceProcessor* rp = _semeru_h->ref_processor_cm();
	// Precleaning is single threaded. Temporarily disable MT discovery.
	ReferenceProcessorMTDiscoveryMutator rp_mut_discovery(rp, false);
	rp->preclean_discovered_references(rp->is_alive_non_header(),
																		 &keep_alive,
																		 &drain_mark_stack,
																		 &yield_cl,
																		 _gc_timer_cm);
}

// When sampling object counts, we already swapped the mark bitmaps, so we need to use
// the prev bitmap determining liveness.
class G1SemeruObjectCountIsAliveClosure: public BoolObjectClosure {
	G1SemeruCollectedHeap* _semeru_h;
public:
	G1SemeruObjectCountIsAliveClosure(G1SemeruCollectedHeap* g1h) : _semeru_h(g1h) { }

	bool do_object_b(oop obj) {
		HeapWord* addr = (HeapWord*)obj;
		return addr != NULL &&
					 (!_semeru_h->is_in_g1_reserved(addr) || !_semeru_h->is_obj_dead(obj));
	}
};

void G1SemeruConcurrentMark::report_object_count(bool mark_completed) {
	// Depending on the completion of the marking liveness needs to be determined
	// using either the next or prev bitmap.
	if (mark_completed) {
		G1SemeruObjectCountIsAliveClosure is_alive(_semeru_h);
		_gc_tracer_cm->report_object_count_after_gc(&is_alive);
	} else {
		G1SemeruCMIsAliveClosure is_alive(_semeru_h);
		_gc_tracer_cm->report_object_count_after_gc(&is_alive);
	}
}


void G1SemeruConcurrentMark::swap_mark_bitmaps() {
	G1CMBitMap* temp = _prev_mark_bitmap;
	_prev_mark_bitmap = _next_mark_bitmap;
	_next_mark_bitmap = temp;
	_semeru_h->collector_state()->set_clearing_next_bitmap(true);
}

/** 
 * Closure for marking entries in SATB buffers.
 * Tag : Remark phase. Mark the alive objects recored by SATB (pre_write barrier).
 * 
 */ 
class G1CMSATBBufferClosure : public SATBBufferClosure {
private:
	G1SemeruCMTask* _task;
	G1SemeruCollectedHeap* _semeru_h;

	// This is very similar to G1SemeruCMTask::deal_with_reference, but with
	// more relaxed requirements for the argument, so this must be more
	// circumspect about treating the argument as an object.
	void do_entry(void* entry) const {
		_task->increment_refs_reached();
		oop const obj = static_cast<oop>(entry);
		_task->make_reference_alive(obj);
	}

public:
	G1CMSATBBufferClosure(G1SemeruCMTask* task, G1SemeruCollectedHeap* g1h)
		: _task(task), _semeru_h(g1h) { }

	virtual void do_buffer(void** buffer, size_t size) {
		for (size_t i = 0; i < size; ++i) {
			do_entry(buffer[i]);
		}
	}
};

class G1SemeruRemarkThreadsClosure : public ThreadClosure {
	G1CMSATBBufferClosure _cm_satb_cl;
	G1SemeruCMOopClosure _semeru_cm_cl;
	MarkingCodeBlobClosure _code_cl;
	int _thread_parity;

 public:
	G1SemeruRemarkThreadsClosure(G1SemeruCollectedHeap* g1h, G1SemeruCMTask* task) :
		_cm_satb_cl(task, g1h),
		_semeru_cm_cl(g1h, task),
		_code_cl(&_semeru_cm_cl, !CodeBlobToOopClosure::FixRelocations),
		_thread_parity(Threads::thread_claim_parity()) {}

	void do_thread(Thread* thread) {
		if (thread->is_Java_thread()) {
			if (thread->claim_oops_do(true, _thread_parity)) {
				JavaThread* jt = (JavaThread*)thread;

				// In theory it should not be neccessary to explicitly walk the nmethods to find roots for concurrent marking
				// however the liveness of oops reachable from nmethods have very complex lifecycles:
				// * Alive if on the stack of an executing method
				// * Weakly reachable otherwise
				// Some objects reachable from nmethods, such as the class loader (or klass_holder) of the receiver should be
				// live by the SATB invariant but other oops recorded in nmethods may behave differently.
				jt->nmethods_do(&_code_cl);

				// 2) Process current mutator/Java thread's SATB queue.
				//		SATB is recorded by pre_write barrier. It only records  the pre_ref_val of the reference assignment.
				G1ThreadLocalData::satb_mark_queue(jt).apply_closure_and_empty(&_cm_satb_cl);
			}
		} else if (thread->is_VM_thread()) {
			if (thread->claim_oops_do(true, _thread_parity)) {
				G1BarrierSet::satb_mark_queue_set().shared_satb_queue()->apply_closure_and_empty(&_cm_satb_cl);
			}
		}
	}
};



/**
 * Tag : The realy task of STW Remark phase.
 *  
 * 	1) Thread marking
 * 		1.1) Java/Non-Java stack variables. Totally remark.
 * 		1.2) Java thread's local SATB queue.
 * 
 * 2) do_marking_step(STW) ?? why
 * 		2.1)
 * 		2.2) Drain global G1BarrierSet->_satb_mark_queue_set.
 * 
 * 	[?] is the do_marking_step  a incremental marking ??
 * 
 */
class G1SemeruCMRemarkTask : public AbstractGangTask {
	G1SemeruConcurrentMark* _semeru_cm;
public:
	void work(uint worker_id) {
		G1SemeruCMTask* task = _semeru_cm->task(worker_id);
		task->record_start_time();
		{
			ResourceMark rm;
			HandleMark hm;

			G1SemeruRemarkThreadsClosure threads_f(G1SemeruCollectedHeap::heap(), task);
			Threads::threads_do(&threads_f);			// 1) Scan the mutators' stack variables.
		}

		// 2) Scan the mutators' dirty card queue ?
		do {
			task->do_marking_step(1000000000.0 /* something very large */,
														true         /* do_termination       */,
														false        /* is_serial            */);
		} while (task->has_aborted() && !_semeru_cm->has_overflown());
		// If we overflow, then we do not want to restart. We instead
		// want to abort remark and do concurrent marking again.
		task->record_end_time();
	}

	G1SemeruCMRemarkTask(G1SemeruConcurrentMark* cm, uint active_workers) :
		AbstractGangTask("Par Remark"), _semeru_cm(cm) {
		_semeru_cm->terminator()->reset_for_reuse(active_workers);
	}
};

/**
 * Tag : The Remark operations.
 *  
 */
void G1SemeruConcurrentMark::finalize_marking() {
	ResourceMark rm;
	HandleMark   hm;

	_semeru_h->ensure_parsability(false);

	// this is remark, so we'll use up all active threads
	uint active_workers = _semeru_h->workers()->active_workers();
	set_concurrency_and_phase(active_workers, false /* concurrent */);
	// Leave _parallel_marking_threads at it's
	// value originally calculated in the G1SemeruConcurrentMark
	// constructor and pass values of the active workers
	// through the gang in the task.

	{
		StrongRootsScope srs(active_workers);

		G1SemeruCMRemarkTask remarkTask(this, active_workers);
		// We will start all available threads, even if we decide that the
		// active_workers will be fewer. The extra ones will just bail out
		// immediately.
		_semeru_h->workers()->run_task(&remarkTask);
	}

	// [?] Remark process all the items in satb queue ?
	// 
	SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
	guarantee(has_overflown() ||
						satb_mq_set.completed_buffers_num() == 0,
						"Invariant: has_overflown = %s, num buffers = " SIZE_FORMAT,
						BOOL_TO_STR(has_overflown()),
						satb_mq_set.completed_buffers_num());

	print_stats();
}

void G1SemeruConcurrentMark::flush_all_task_caches() {
	size_t hits = 0;
	size_t misses = 0;
	for (uint i = 0; i < _max_num_tasks; i++) {
		Pair<size_t, size_t> stats = _tasks[i]->flush_mark_stats_cache();
		hits += stats.first;
		misses += stats.second;
	}
	size_t sum = hits + misses;
	log_debug(gc, stats)("Mark stats cache hits " SIZE_FORMAT " misses " SIZE_FORMAT " ratio %1.3lf",
											 hits, misses, percent_of(hits, sum));
}

void G1SemeruConcurrentMark::clear_range_in_prev_bitmap(MemRegion mr) {
	_prev_mark_bitmap->clear_range(mr);
}


/**
 * Semeru Memory Server - Cliam a Region from the Old Space to Concurrent Mark.
 *  
 * 		There is a global pointer, _finger, to the scavenge.
 * 
 */
// HeapRegion*
// G1SemeruConcurrentMark::claim_region(uint worker_id) {
// 	// "checkpoint" the finger
// 	HeapWord* finger = _finger;

// 	while (finger < _heap.end()) {
// 		assert(_semeru_h->is_in_g1_reserved(finger), "invariant");

// 		HeapRegion* curr_region = _semeru_h->heap_region_containing(finger);
// 		// Make sure that the reads below do not float before loading curr_region.
// 		OrderAccess::loadload();
// 		// Above heap_region_containing may return NULL as we always scan claim
// 		// until the end of the heap. In this case, just jump to the next region.
// 		HeapWord* end = curr_region != NULL ? curr_region->end() : finger + HeapRegion::GrainWords;

// 		// Is the gap between reading the finger and doing the CAS too long?
// 		HeapWord* res = Atomic::cmpxchg(end, &_finger, finger);
// 		if (res == finger && curr_region != NULL) {
// 			// we succeeded
// 			HeapWord*   bottom        = curr_region->bottom();
// 			HeapWord*   limit         = curr_region->next_top_at_mark_start();

// 			// notice that _finger == end cannot be guaranteed here since,
// 			// someone else might have moved the finger even further
// 			assert(_finger >= end, "the finger should have moved forward");

// 			if (limit > bottom) {
// 				return curr_region;
// 			} else {
// 				assert(limit == bottom,
// 							 "the region limit should be at bottom");
// 				// we return NULL and the caller should try calling
// 				// claim_region() again.
// 				return NULL;
// 			}
// 		} else {
// 			assert(_finger > finger, "the finger should have moved forward");
// 			// read it again
// 			finger = _finger;
// 		}
// 	}

// 	return NULL;
// }


/**
 * Semeru Memory Server 
 *  Cliam a Region from memory server's CSet.
 * 
 * [?] nothing to do with the parameter worker_id ??
 * 
 * Structrure of the Chain:
 * 	HeapRegion ---(_mem_server_cset_next)--> HeapRegion --> HeapRegion ...... 
 * 																															^
 * 																										G1SemeruConcurrentMark->_finger
 * 																										The first not scanned HeapRegion.
 * 
 * [x] Multiple concurrent threads may race for the same HeapRegion parallelly ?  NO at present.
 * 	=> In current design, the HeapRegion is quite big. 
 * 		 Everytime, reclaim a single HeapRegion and scan it parallelly, if there are multiple concurrent marking threads.	
 * 
 * [x] The _curr_region can be NULL. Because it may be reseted to NULL after finishing scaning it. 
 * 
 */
HeapRegion*
G1SemeruConcurrentMark::claim_region(uint worker_id) {
	// "checkpoint" the finger
	HeapWord* old_finger = _finger;

	//while ( finger != NULL && finger < _heap.end()) {  // [??] Warning : it's betteer to change _heap.end() to CSet end.
	
	// The Regions in memory server CSet may come from any region. 
	// The flag of reaching the end of the CSet is that _finger is NULL.
	while( _finger != NULL){  

		//debug
		//tty->print("%s, Change the _heap.end() to memory server CSet end().", __func__);

		assert(_semeru_h->is_in_g1_reserved(old_finger), "invariant");
		HeapRegion* curr_region = _semeru_h->heap_region_containing(old_finger);
		assert(curr_region != NULL, "%s,if _finger isn't NULL, curr_region can't be NULL in a serial cliamation mode.\n", __func__);

		// Read Barrier
		// Make sure that the reads below do not float before loading curr_region.
		OrderAccess::loadload();
		
		// Above heap_region_containing may return NULL as we always scan claim
		// until the end of the heap. In this case, just jump to the next region.
		//
		// [?] Why can the curr_region be NULL, if the _finger isn't NULL. 
		//	=> heap_region_containing(addr) get the corresponding regions from  SemeruHeapRegionManager->_regions[]
		//		 The freed HeapRegions will be removed from the _regions[].
		//	
		//HeapWord* end = curr_region != NULL ? curr_region->end() : finger + HeapRegion::GrainWords;
		HeapWord* end =  curr_region->mem_server_cset_next()!= NULL ? 
															curr_region->mem_server_cset_next()->bottom() : 
															NULL;	// end points the end address of current claimed Region.


		// Is the gap between reading the finger and doing the CAS too long?
		// Let _finger points to the next available HeapRegion in memory server's CSet.
		// if end == NULL, means curr_region is the last Region in memory server CSet.
		// Atomic::cmpxchg(new, pointer, old)
		HeapWord* res = Atomic::cmpxchg(end, &_finger, old_finger);

		// if  the end is NULL, also return false.
		if (res == old_finger) {
			// we succeeded
			HeapWord*   bottom        = curr_region->bottom();
			HeapWord*   limit         = curr_region->next_top_at_mark_start();

			// notice that _finger == end cannot be guaranteed here since,
			// someone else might have moved the finger even further
		//	assert(_finger >= end, "the finger should have moved forward");

			if (limit > bottom) {
				return curr_region;
			} else {
				assert(limit == bottom,
							 "the region limit should be at bottom");
				// we return NULL and the caller should try calling
				// claim_region() again.
				return NULL;
			}
		} else {
			
			// We don't get this Region successfully.
				// assert(_finger > finger, "the finger should have moved forward");
				// // read it again
				// finger = _finger;

				guarantee(false, "%s, it's not possible to reach here in a sequential mode. \n", __func__);
			
		}

	} // finger != NULL

	return NULL; // run out of CSet regions.
}








#ifndef PRODUCT
class VerifyNoCSetOops {
	G1SemeruCollectedHeap* _semeru_h;
	const char* _phase;
	int _info;

public:
	VerifyNoCSetOops(const char* phase, int info = -1) :
		_semeru_h(G1SemeruCollectedHeap::heap()),
		_phase(phase),
		_info(info)
	{ }

	void operator()(G1SemeruTaskQueueEntry task_entry) const {
		if (task_entry.is_array_slice()) {
			guarantee(_semeru_h->is_in_reserved(task_entry.slice()), "Slice " PTR_FORMAT " must be in heap.", p2i(task_entry.slice()));
			return;
		}
		guarantee(oopDesc::is_oop(task_entry.obj()),
							"Non-oop " PTR_FORMAT ", phase: %s, info: %d",
							p2i(task_entry.obj()), _phase, _info);
		guarantee(!_semeru_h->is_in_cset(task_entry.obj()),
							"obj: " PTR_FORMAT " in CSet, phase: %s, info: %d",
							p2i(task_entry.obj()), _phase, _info);
	}
};

void G1SemeruConcurrentMark::verify_no_cset_oops() {
	assert(SafepointSynchronize::is_at_safepoint(), "should be at a safepoint");
	if (!_semeru_h->collector_state()->mark_or_rebuild_in_progress()) {
		return;
	}

	// Verify entries on the global mark stack
	_global_mark_stack.iterate(VerifyNoCSetOops("Stack"));

	// Verify entries on the task queues
	for (uint i = 0; i < _max_num_tasks; ++i) {
		G1SemeruCMTaskQueue* queue = _task_queues->queue(i);
		queue->iterate(VerifyNoCSetOops("Queue", i));
	}

	// Verify the global finger
	HeapWord* global_finger = finger();
	if (global_finger != NULL && global_finger < _heap.end()) {
		// Since we always iterate over all regions, we might get a NULL HeapRegion
		// here.
		HeapRegion* global_hr = _semeru_h->heap_region_containing(global_finger);
		guarantee(global_hr == NULL || global_finger == global_hr->bottom(),
							"global finger: " PTR_FORMAT " region: " HR_FORMAT,
							p2i(global_finger), HR_FORMAT_PARAMS(global_hr));
	}

	// Verify the task fingers
	assert(_num_concurrent_workers <= _max_num_tasks, "sanity");
	for (uint i = 0; i < _num_concurrent_workers; ++i) {
		G1SemeruCMTask* task = _tasks[i];
		HeapWord* task_finger = task->finger();
		if (task_finger != NULL && task_finger < _heap.end()) {
			// See above note on the global finger verification.
			HeapRegion* task_hr = _semeru_h->heap_region_containing(task_finger);
			guarantee(task_hr == NULL || task_finger == task_hr->bottom() ||
								!task_hr->in_collection_set(),
								"task finger: " PTR_FORMAT " region: " HR_FORMAT,
								p2i(task_finger), HR_FORMAT_PARAMS(task_hr));
		}
	}
}
#endif // PRODUCT

void G1SemeruConcurrentMark::rebuild_rem_set_concurrently() {

// Error
	printf("Error in %s, please fix this. \n", __func__);

//	_semeru_h->g1_rem_set()->rebuild_rem_set(this, _concurrent_workers, _worker_id_offset);
}

void G1SemeruConcurrentMark::print_stats() {
	if (!log_is_enabled(Debug, gc, stats)) {
		return;
	}
	log_debug(gc, stats)("---------------------------------------------------------------------");
	for (size_t i = 0; i < _num_active_tasks; ++i) {
		_tasks[i]->print_stats();
		log_debug(gc, stats)("---------------------------------------------------------------------");
	}
}

void G1SemeruConcurrentMark::concurrent_cycle_abort() {
	if (!cm_thread()->during_cycle() || _has_aborted) {
		// We haven't started a concurrent cycle or we have already aborted it. No need to do anything.
		return;
	}

	// Clear all marks in the next bitmap for the next marking cycle. This will allow us to skip the next
	// concurrent bitmap clearing.
	{
		GCTraceTime(Debug, gc) debug("Clear Next Bitmap");
		clear_bitmap(_next_mark_bitmap, _semeru_h->workers(), false);
	}
	// Note we cannot clear the previous marking bitmap here
	// since VerifyDuringGC verifies the objects marked during
	// a full GC against the previous bitmap.

	// Empty mark stack
	reset_marking_for_restart();
	for (uint i = 0; i < _max_num_tasks; ++i) {
		_tasks[i]->clear_region_fields();
	}
	_first_overflow_barrier_sync.abort();
	_second_overflow_barrier_sync.abort();
	_has_aborted = true;

	SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
	satb_mq_set.abandon_partial_marking();
	// This can be called either during or outside marking, we'll read
	// the expected_active value from the SATB queue set.
	satb_mq_set.set_active_all_threads(
																 false, /* new active value */
																 satb_mq_set.is_active() /* expected_active */);
}

static void print_ms_time_info(const char* prefix, const char* name,
															 NumberSeq& ns) {
	log_trace(gc, marking)("%s%5d %12s: total time = %8.2f s (avg = %8.2f ms).",
												 prefix, ns.num(), name, ns.sum()/1000.0, ns.avg());
	if (ns.num() > 0) {
		log_trace(gc, marking)("%s         [std. dev = %8.2f ms, max = %8.2f ms]",
													 prefix, ns.sd(), ns.maximum());
	}
}

void G1SemeruConcurrentMark::print_summary_info() {
	Log(gc, marking) log;
	if (!log.is_trace()) {
		return;
	}

	log.trace(" Concurrent marking:");
	print_ms_time_info("  ", "init marks", _init_times);
	print_ms_time_info("  ", "remarks", _remark_times);
	{
		print_ms_time_info("     ", "final marks", _remark_mark_times);
		print_ms_time_info("     ", "weak refs", _remark_weak_ref_times);

	}
	print_ms_time_info("  ", "cleanups", _cleanup_times);
	log.trace("    Finalize live data total time = %8.2f s (avg = %8.2f ms).",
						_total_cleanup_time, (_cleanup_times.num() > 0 ? _total_cleanup_time * 1000.0 / (double)_cleanup_times.num() : 0.0));
	log.trace("  Total stop_world time = %8.2f s.",
						(_init_times.sum() + _remark_times.sum() + _cleanup_times.sum())/1000.0);
	log.trace("  Total concurrent time = %8.2f s (%8.2f s marking).",
						cm_thread()->vtime_accum(), cm_thread()->vtime_mark_accum());
}

void G1SemeruConcurrentMark::print_worker_threads_on(outputStream* st) const {
	_concurrent_workers->print_worker_threads_on(st);
}

void G1SemeruConcurrentMark::threads_do(ThreadClosure* tc) const {
	_concurrent_workers->threads_do(tc);
}

void G1SemeruConcurrentMark::print_on_error(outputStream* st) const {
	st->print_cr("Marking Bits (Prev, Next): (CMBitMap*) " PTR_FORMAT ", (CMBitMap*) " PTR_FORMAT,
							 p2i(_prev_mark_bitmap), p2i(_next_mark_bitmap));
	_prev_mark_bitmap->print_on_error(st, " Prev Bits: ");
	_next_mark_bitmap->print_on_error(st, " Next Bits: ");
}

static ReferenceProcessor* get_cm_oop_closure_ref_processor(G1SemeruCollectedHeap* g1h) {
	ReferenceProcessor* result = g1h->ref_processor_cm();
	assert(result != NULL, "CM reference processor should not be NULL");
	return result;
}

// Override the constructor of  G1SemeruCMOopClosure
// the declaration and definion of the class G1SemeruCMOopClosure is in G1CMOopClosure.hpp
G1SemeruCMOopClosure::G1SemeruCMOopClosure(G1SemeruCollectedHeap* semeru_h,
															 G1SemeruCMTask* task)
	: MetadataVisitingOopIterateClosure(get_cm_oop_closure_ref_processor(semeru_h)),
		_semeru_h(semeru_h), _semeru_task(task)
{ }


/**
 * Set the Region to be scanned currently ?
 * 
 * OR only assign the first region to _curr_region only
 *  
 */
void G1SemeruCMTask::setup_for_region(HeapRegion* hr) {
	assert(hr != NULL,
				"claim_region() should have filtered out NULL regions");
	_curr_region  = hr;

	// Current scanning region's alive_bitmap and dest_bitmap 
	_alive_bitmap	=	hr->alive_bitmap();
	_dest_bitmap	=	hr->dest_bitmap();

	//_finger       = hr->bottom();		// Semeru memory server CM doesn't use the local _finger.
	//update_region_limit();
}

void G1SemeruCMTask::update_region_limit() {
	HeapRegion* hr            = _curr_region;
	HeapWord* bottom          = hr->bottom();
	HeapWord* limit           = hr->next_top_at_mark_start(); // All the newly allocated objects after the CM start will be handled seperately.

	if (limit == bottom) {
		// The region was collected underneath our feet.
		// We set the finger to bottom to ensure that the bitmap
		// iteration that will follow this will not do anything.
		// (this is not a condition that holds when we set the region up,
		// as the region is not supposed to be empty in the first place)
		_finger = bottom;
	} else if (limit >= _region_limit) {
		assert(limit >= _finger, "peace of mind");
	} else {
		assert(limit < _region_limit, "only way to get here");
		// This can happen under some pretty unusual circumstances.  An
		// evacuation pause empties the region underneath our feet (NTAMS
		// at bottom). We then do some allocation in the region (NTAMS
		// stays at bottom), followed by the region being used as a GC
		// alloc region (NTAMS will move to top() and the objects
		// originally below it will be grayed). All objects now marked in
		// the region are explicitly grayed, if below the global finger,
		// and we do not need in fact to scan anything else. So, we simply
		// set _finger to be limit to ensure that the bitmap iteration
		// doesn't do anything.
		_finger = limit;
	}

	_region_limit = limit;
}

/**
 * Semeru Memory Server
 *  	Update the region limit for current MARKING region.
 */
// void G1SemeruCMTask::update_marking_region_limit() {
// 	HeapRegion* hr            = _curr_region;   // both CM marking and STW compaction use the same pointer?
// 	HeapWord* bottom          = hr->bottom();
// 	HeapWord* limit           = hr->next_top_at_mark_start(); // All the newly allocated objects after the CM start will be handled seperately.

// 	// [?] seems Semeru memory server doesn't need to maintain the G1SemeruCMTask->_finger .

// 	// if (limit == bottom) {
// 	// 	// The region was collected underneath our feet.
// 	// 	// We set the finger to bottom to ensure that the bitmap
// 	// 	// iteration that will follow this will not do anything.
// 	// 	// (this is not a condition that holds when we set the region up,
// 	// 	// as the region is not supposed to be empty in the first place)
// 	// 	_finger = bottom;
// 	// } else if (limit >= _region_limit) {
// 	// 	assert(limit >= _finger, "peace of mind");
// 	// } else {
// 	// 	assert(limit < _region_limit, "only way to get here");
// 	// 	// This can happen under some pretty unusual circumstances.  An
// 	// 	// evacuation pause empties the region underneath our feet (NTAMS
// 	// 	// at bottom). We then do some allocation in the region (NTAMS
// 	// 	// stays at bottom), followed by the region being used as a GC
// 	// 	// alloc region (NTAMS will move to top() and the objects
// 	// 	// originally below it will be grayed). All objects now marked in
// 	// 	// the region are explicitly grayed, if below the global finger,
// 	// 	// and we do not need in fact to scan anything else. So, we simply
// 	// 	// set _finger to be limit to ensure that the bitmap iteration
// 	// 	// doesn't do anything.
// 	// 	_finger = limit;
// 	// }

// 	_marking_region_limit = limit;
// }



/**
 * Semeru Memory Server
 *  	Compaction doesn't need such a pointer, need to evacuate all the alive objects in current Region.
 * 		Which is HeapRegion->top
 */




void G1SemeruCMTask::giveup_current_region() {
	assert(_curr_region != NULL, "invariant");
	clear_region_fields();
}

void G1SemeruCMTask::clear_region_fields() {
	// Values for these three fields that indicate that we're not
	// holding on to a region.
	_curr_region   = NULL;
	_finger        = NULL;
	_region_limit  = NULL;
}

/**
 *  Set the closure for scanning a marked object in the alive_bitmap. 
 *  
 */
void G1SemeruCMTask::set_cm_oop_closure(G1SemeruCMOopClosure* semeru_cm_oop_closure) {
	if (semeru_cm_oop_closure == NULL) {
		assert(_semeru_cm_oop_closure != NULL, "invariant");
	} else {
		assert(_semeru_cm_oop_closure == NULL, "invariant");
	}
	_semeru_cm_oop_closure = semeru_cm_oop_closure;
}



void G1SemeruCMTask::reset(G1CMBitMap* next_mark_bitmap) {
	guarantee(next_mark_bitmap != NULL, "invariant");
	_next_mark_bitmap              = next_mark_bitmap;
	clear_region_fields();

	_calls                         = 0;
	_elapsed_time_ms               = 0.0;
	_termination_time_ms           = 0.0;
	_termination_start_time_ms     = 0.0;

	_mark_stats_cache.reset();
}

bool G1SemeruCMTask::should_exit_termination() {
	if (!regular_clock_call()) {
		return true;
	}

	// This is called when we are in the termination protocol. We should
	// quit if, for some reason, this task wants to abort or the global
	// stack is not empty (this means that we can get work from it).
	return !_semeru_cm->mark_stack_empty() || has_aborted();
}

void G1SemeruCMTask::reached_limit() {
	assert(_words_scanned >= _words_scanned_limit ||
				 _refs_reached >= _refs_reached_limit ,
				 "shouldn't have been called otherwise");
	abort_marking_if_regular_check_fail();
}

bool G1SemeruCMTask::regular_clock_call() {
	if (has_aborted()) {
		return false;
	}

	// First, we need to recalculate the words scanned and refs reached
	// limits for the next clock call.
	recalculate_limits();

	// During the regular clock call we do the following

	// (1) If an overflow has been flagged, then we abort.
	if (_semeru_cm->has_overflown()) {
		return false;
	}

	// If we are not concurrent (i.e. we're doing remark) we don't need
	// to check anything else. The other steps are only needed during
	// the concurrent marking phase.
	if (!_semeru_cm->concurrent()) {
		return true;
	}

	// (2) If marking has been aborted for Full GC, then we also abort.
	if (_semeru_cm->has_aborted()) {
		return false;
	}

	double curr_time_ms = os::elapsedVTime() * 1000.0;

	// (4) We check whether we should yield. If we have to, then we abort.
	if (SuspendibleThreadSet::should_yield()) {
		// We should yield. To do this we abort the task. The caller is
		// responsible for yielding.
		return false;
	}

	// (5) We check whether we've reached our time quota. If we have,
	// then we abort.
	double elapsed_time_ms = curr_time_ms - _start_time_ms;
	if (elapsed_time_ms > _time_target_ms) {
		_has_timed_out = true;
		return false;
	}

	// (6) Finally, we check whether there are enough completed STAB
	// buffers available for processing. If there are, we abort.
	SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
	if (!_draining_satb_buffers && satb_mq_set.process_completed_buffers()) {
		// we do need to process SATB buffers, we'll abort and restart
		// the marking task to do so
		return false;
	}
	return true;
}

void G1SemeruCMTask::recalculate_limits() {
	_real_words_scanned_limit = _words_scanned + words_scanned_period;
	_words_scanned_limit      = _real_words_scanned_limit;

	_real_refs_reached_limit  = _refs_reached  + refs_reached_period;
	_refs_reached_limit       = _real_refs_reached_limit;
}

void G1SemeruCMTask::decrease_limits() {
	// This is called when we believe that we're going to do an infrequent
	// operation which will increase the per byte scanned cost (i.e. move
	// entries to/from the global stack). It basically tries to decrease the
	// scanning limit so that the clock is called earlier.

	_words_scanned_limit = _real_words_scanned_limit - 3 * words_scanned_period / 4;
	_refs_reached_limit  = _real_refs_reached_limit - 3 * refs_reached_period / 4;
}





/**
 * Semeru Memory Server
 * 
 * [x] Trace an alive objects directly, without marking it alive in alive_bitmap.
 *  
 * [?] This function is usually invoked to process a humongous Region.
 * 		 There should be a very large object occupied the entire Region.
 * 		 So no need to mark the  alive_bitmap for a humonguous Region ??
 * 
 * [x] Because this function is usually to process a humongous Region.
 * 		So, partially drain the semeru_taskqueue after processing the big object.
 * 
 * [?] Why does it define this scavenge within the bitmap closure ??
 * 		=> We even don't need the bitmap ?
 * 
 */
bool G1SemeruCMTask::semeru_cm_task_do_addr(HeapWord* const addr) {

	// Confirm this object is within the covered range of the bitmap.
	assert(_curr_region->is_in_reserved(addr), " oop's start address have to be in the current scanning Region." );

	scan_task_entry(G1SemeruTaskQueueEntry::from_oop(oop(addr)));
	// we only partially drain the local queue and global stack
	drain_local_queue(true);
	drain_global_stack(true);

	// if the has_aborted flag has been raised, we need to bail out of
	// the iteration
	return !has_aborted();
}





void G1SemeruCMTask::move_entries_to_global_stack() {
	// Local array where we'll store the entries that will be popped
	// from the local queue.
	G1SemeruTaskQueueEntry buffer[G1SemeruCMMarkStack::EntriesPerChunk]; // 1024 -1

	size_t n = 0;
	G1SemeruTaskQueueEntry task_entry;
	while (n < G1SemeruCMMarkStack::EntriesPerChunk && _semeru_task_queue->pop_local(task_entry)) {
		buffer[n] = task_entry;		// Assign the poped entry to the newly created buffer[].
		++n;
	}

	if (n < G1SemeruCMMarkStack::EntriesPerChunk) {
		buffer[n] = G1SemeruTaskQueueEntry();  // [?] what does this mean ? no operator new() ? invoke the constructor directly ?
	}

	if (n > 0) {
		if (!_semeru_cm->mark_stack_push(buffer)) {  // Push the newly created Buffer[] into 
			set_has_aborted();
		}
	}

	// This operation was quite expensive, so decrease the limits.
	// [?] What's the design logic for the limits ??
	decrease_limits();
}

bool G1SemeruCMTask::get_entries_from_global_stack() {
	// Local array where we'll store the entries that will be popped
	// from the global stack.
	G1SemeruTaskQueueEntry buffer[G1SemeruCMMarkStack::EntriesPerChunk];

	if (!_semeru_cm->mark_stack_pop(buffer)) {
		return false;
	}

	// We did actually pop at least one entry.
	for (size_t i = 0; i < G1SemeruCMMarkStack::EntriesPerChunk; ++i) {
		G1SemeruTaskQueueEntry task_entry = buffer[i];
		if (task_entry.is_null()) {
			break;
		}
		assert(task_entry.is_array_slice() || oopDesc::is_oop(task_entry.obj()), "Element " PTR_FORMAT " must be an array slice or oop", p2i(task_entry.obj()));
		bool success = _semeru_task_queue->push(task_entry);
		// We only call this when the local queue is empty or under a
		// given target limit. So, we do not expect this push to fail.
		assert(success, "invariant");
	}

	// This operation was quite expensive, so decrease the limits
	decrease_limits();
	return true;
}


/**
 * Tag : Drain the CM->_semeru_task_queue, StarTask queue.
 *  
 */
void G1SemeruCMTask::drain_local_queue(bool partially) {
	if (has_aborted()) {
		return;
	}

	// Decide what the target size is, depending whether we're going to
	// drain it partially (so that other tasks can steal if they run out
	// of things to do) or totally (at the very end).
	size_t target_size;
	if (partially) {
		target_size = MIN2((size_t)_semeru_task_queue->max_elems()/3, (size_t)GCDrainStackTargetSize);
	} else {
		target_size = 0;  // Drain all the items.
	}

	if (_semeru_task_queue->size() > target_size) {
		G1SemeruTaskQueueEntry entry;
		bool ret = _semeru_task_queue->pop_local(entry);
		while (ret) {
			scan_task_entry(entry);
			if (_semeru_task_queue->size() <= target_size || has_aborted()) {
				ret = false;
			} else {
				ret = _semeru_task_queue->pop_local(entry);
			}
		}
	} // end of if
}

void G1SemeruCMTask::drain_global_stack(bool partially) {
	if (has_aborted()) {
		return;
	}

	// We have a policy to drain the local queue before we attempt to
	// drain the global stack.
	assert(partially || _semeru_task_queue->size() == 0, "invariant");

	// Decide what the target size is, depending whether we're going to
	// drain it partially (so that other tasks can steal if they run out
	// of things to do) or totally (at the very end).
	// Notice that when draining the global mark stack partially, due to the racyness
	// of the mark stack size update we might in fact drop below the target. But,
	// this is not a problem.
	// In case of total draining, we simply process until the global mark stack is
	// totally empty, disregarding the size counter.
	if (partially) {
		size_t const target_size = _semeru_cm->partial_mark_stack_size_target();
		while (!has_aborted() && _semeru_cm->mark_stack_size() > target_size) {
			if (get_entries_from_global_stack()) {
				drain_local_queue(partially);
			}
		}
	} else {
		while (!has_aborted() && get_entries_from_global_stack()) {
			drain_local_queue(partially);
		}
	}
}

// SATB Queue has several assumptions on whether to call the par or
// non-par versions of the methods. this is why some of the code is
// replicated. We should really get rid of the single-threaded version
// of the code to simplify things.
void G1SemeruCMTask::drain_satb_buffers() {
	if (has_aborted()) {
		return;
	}

	// We set this so that the regular clock knows that we're in the
	// middle of draining buffers and doesn't set the abort flag when it
	// notices that SATB buffers are available for draining. It'd be
	// very counter productive if it did that. :-)
	_draining_satb_buffers = true;

	G1CMSATBBufferClosure satb_cl(this, _semeru_h);
	SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();

	// This keeps claiming and applying the closure to completed buffers
	// until we run out of buffers or we need to abort.
	while (!has_aborted() &&
				 satb_mq_set.apply_closure_to_completed_buffer(&satb_cl)) {
		abort_marking_if_regular_check_fail();
	}

	_draining_satb_buffers = false;

	assert(has_aborted() ||
				 _semeru_cm->concurrent() ||
				 satb_mq_set.completed_buffers_num() == 0, "invariant");

	// again, this was a potentially expensive operation, decrease the
	// limits to get the regular clock call early
	decrease_limits();
}

void G1SemeruCMTask::clear_mark_stats_cache(uint region_idx) {
	_mark_stats_cache.reset(region_idx);
}

Pair<size_t, size_t> G1SemeruCMTask::flush_mark_stats_cache() {
	return _mark_stats_cache.evict_all();
}

void G1SemeruCMTask::print_stats() {
	log_debug(gc, stats)("Marking Stats, task = %u, calls = %u", _worker_id, _calls);
	log_debug(gc, stats)("  Elapsed time = %1.2lfms, Termination time = %1.2lfms",
											 _elapsed_time_ms, _termination_time_ms);
	log_debug(gc, stats)("  Step Times (cum): num = %d, avg = %1.2lfms, sd = %1.2lfms max = %1.2lfms, total = %1.2lfms",
											 _step_times_ms.num(),
											 _step_times_ms.avg(),
											 _step_times_ms.sd(),
											 _step_times_ms.maximum(),
											 _step_times_ms.sum());
	size_t const hits = _mark_stats_cache.hits();
	size_t const misses = _mark_stats_cache.misses();
	log_debug(gc, stats)("  Mark Stats Cache: hits " SIZE_FORMAT " misses " SIZE_FORMAT " ratio %.3f",
											 hits, misses, percent_of(hits, hits + misses));
}

bool G1SemeruConcurrentMark::try_stealing(uint worker_id, G1SemeruTaskQueueEntry& task_entry) {
	return _task_queues->steal(worker_id, task_entry);
}

/*****************************************************************************

		The do_marking_step(time_target_ms, ...) method is the building
		block of the parallel marking framework. It can be called in parallel
		with other invocations of do_marking_step() on different tasks
		(but only one per task, obviously) and concurrently with the
		mutator threads, or during remark, hence it eliminates the need
		for two versions of the code. ** When called during remark, it will    // how ? by using the global_finger ?
		pick up from where the task left off during the concurrent marking
		phase. ** Interestingly, tasks are also claimable during evacuation
		pauses too, since do_marking_step() ensures that it aborts before
		it needs to yield.

		The data structures that it uses to do marking work are the
		following:

			(1) Marking Bitmap. If there are gray objects that appear only			// gray objects, marked alive in CM->_next_bitmap
			on the bitmap (this happens either when dealing with an overflow		// e.g. When process the SATB queue, no need to push the target object into StarTask queue.
			or when the initial marking phase has simply marked the roots
			and didn't push them on the stack), then tasks claim heap						// Not push object on the stack, means not enqueue it to scan its fields.
			regions whose bitmap they then scan to find gray objects. A
			global finger indicates where the end of the last claimed region
			is. A local finger indicates how far into the region a task has
			scanned. The two fingers are used to determine how to gray an
			object (i.e. whether simply marking it is OK, as it will be
			visited by a task in the future, or whether it needs to be also
			pushed on a stack).

			(2) Local Queue. The local queue of the task which is accessed
			reasonably efficiently by the task. Other tasks can steal from
			it when they run out of work. Throughout the marking phase, a
			task attempts to keep its local queue short but not totally
			empty, so that entries are available for stealing by other
			tasks. Only when there is no more work, a task will totally
			drain its local queue.

			(3) Global Mark Stack. This handles local queue overflow. During			// (Global) mark stack. it's overflow queue for the local queue ?
			marking only sets of entries are moved between it and the local
			queues, as access to it requires a mutex and more fine-grain
			interaction with it which might cause contention. If it
			overflows, then the marking phase should restart and iterate
			over the bitmap to identify gray objects. Throughout the marking
			phase, tasks attempt to keep the global mark stack at a small
			length but not totally empty, so that entries are available for
			popping by other tasks. Only when there is no more work, tasks
			will totally drain the global mark stack.

			(4) SATB Buffer Queue. This is where completed SATB buffers are
			made available. Buffers are regularly removed from this queue
			and scanned for roots, so that the queue doesn't get too
			long. During remark, all completed buffers are processed, as
			well as the filled in parts of any uncompleted buffers.

		The do_marking_step() method tries to abort when the time target
		has been reached. There are a few other cases when the
		do_marking_step() method also aborts:

			(1) When the marking phase has been aborted (after a Full GC).

			(2) When a global overflow (on the global stack) has been
			triggered. Before the task aborts, it will actually sync up with
			the other tasks to ensure that all the marking data structures
			(local queues, stacks, fingers etc.)  are re-initialized so that
			when do_marking_step() completes, the marking phase can
			immediately restart.

			(3) When enough completed SATB buffers are available. The
			do_marking_step() method only tries to drain SATB buffers right
			at the beginning. So, if enough buffers are available, the
			marking step aborts and the SATB buffers are processed at
			the beginning of the next invocation.

			(4) To yield. when we have to yield then we abort and yield
			right at the end of do_marking_step(). This saves us from a lot
			of hassle as, by yielding we might allow a Full GC. If this
			happens then objects will be compacted underneath our feet, the
			heap might shrink, etc. We save checking for this by just
			aborting and doing the yield right at the end.

		From the above it follows that the do_marking_step() method should
		be called in a loop (or, otherwise, regularly) until it completes.

		If a marking step completes without its has_aborted() flag being
		true, it means it has completed the current marking phase (and
		also all other marking tasks have done so and have all synced up).

		A method called regular_clock_call() is invoked "regularly" (in
		sub ms intervals) throughout marking. It is this clock method that
		checks all the abort conditions which were mentioned above and
		decides when the task should abort. A work-based scheme is used to
		trigger this clock method: when the number of object words the
		marking phase has scanned or the number of references the marking
		phase has visited reach a given limit. Additional invocations to
		the method clock have been planted in a few other strategic places
		too. The initial reason for the clock method was to avoid calling
		vtime too regularly, as it is quite expensive. So, once it was in
		place, it was natural to piggy-back all the other conditions on it
		too and not constantly check them throughout the code.

		If do_termination is true then do_marking_step will enter its
		termination protocol.

		The value of is_serial must be true when do_marking_step is being
		called serially (i.e. by the VMThread) and do_marking_step should
		skip any synchronization in the termination and overflow code.
		Examples include the serial remark code and the serial reference
		processing closures.

		The value of is_serial must be false when do_marking_step is
		being called by any of the worker threads in a work gang.
		Examples include the concurrent marking code (CMMarkingTask),
		the MT remark code, and the MT reference processing closures.

 *****************************************************************************/
void G1SemeruCMTask::do_marking_step(double time_target_ms,
															 bool do_termination,
															 bool is_serial) {
	assert(time_target_ms >= 1.0, "minimum granularity is 1ms");

	// debug
	guarantee(false, "Do NOT invoke this function for any purpose. \n");


// 	// Error
// 	guarantee(false, "%s, This function is discarded.\n", __func__);




// 	_start_time_ms = os::elapsedVTime() * 1000.0;

// 	// If do_stealing is true then do_marking_step will attempt to
// 	// steal work from the other G1SemeruCMTasks. It only makes sense to
// 	// enable stealing when the termination protocol is enabled
// 	// and do_marking_step() is not being called serially.
// 	bool do_stealing = do_termination && !is_serial;

// 	double diff_prediction_ms = _semeru_h->g1_policy()->predictor().get_new_prediction(&_marking_step_diffs_ms);
// 	_time_target_ms = time_target_ms - diff_prediction_ms;

// 	// set up the variables that are used in the work-based scheme to
// 	// call the regular clock method
// 	_words_scanned = 0;
// 	_refs_reached  = 0;
// 	recalculate_limits();

// 	// clear all flags
// 	clear_has_aborted();
// 	_has_timed_out = false;
// 	_draining_satb_buffers = false;

// 	++_calls;

// 	// Set up the bitmap and oop closures. Anything that uses them is
// 	// eventually called from this method, so it is OK to allocate these
// 	// statically.
// 	G1CMBitMapClosure bitmap_closure(this, _semeru_cm);
// 	G1CMOopClosure cm_oop_closure(_semeru_h, this);
// 	set_cm_oop_closure(&cm_oop_closure);

// 	if (_semeru_cm->has_overflown()) {
// 		// This can happen if the mark stack overflows during a GC pause
// 		// and this task, after a yield point, restarts. We have to abort
// 		// as we need to get into the overflow protocol which happens
// 		// right at the end of this task.
// 		set_has_aborted();
// 	}

// 	// First drain any available SATB buffers. After this, we will not
// 	// look at SATB buffers before the next invocation of this method.
// 	// If enough completed SATB buffers are queued up, the regular clock
// 	// will abort this task so that it restarts.
// 	drain_satb_buffers();
// 	// ...then partially drain the local queue and the global stack
// 	drain_local_queue(true);
// 	drain_global_stack(true);

// 	do {
// 		if (!has_aborted() && _curr_region != NULL) {
// 			// This means that we're already holding on to a region.
// 			assert(_finger != NULL, "if region is not NULL, then the finger "
// 						 "should not be NULL either");

// 			// We might have restarted this task after an evacuation pause
// 			// which might have evacuated the region we're holding on to
// 			// underneath our feet. Let's read its limit again to make sure
// 			// that we do not iterate over a region of the heap that
// 			// contains garbage (update_region_limit() will also move
// 			// _finger to the start of the region if it is found empty).
// 			update_region_limit();
// 			// We will start from _finger not from the start of the region,
// 			// as we might be restarting this task after aborting half-way
// 			// through scanning this region. In this case, _finger points to
// 			// the address where we last found a marked object. If this is a
// 			// fresh region, _finger points to start().
// 			MemRegion mr = MemRegion(_finger, _region_limit);

// 			assert(!_curr_region->is_humongous() || mr.start() == _curr_region->bottom(),
// 						 "humongous regions should go around loop once only");

// 			// Some special cases:
// 			// If the memory region is empty, we can just give up the region.
// 			// If the current region is humongous then we only need to check
// 			// the bitmap for the bit associated with the start of the object,
// 			// scan the object if it's live, and give up the region.
// 			// Otherwise, let's iterate over the bitmap of the part of the region
// 			// that is left.
// 			// If the iteration is successful, give up the region.
// 			if (mr.is_empty()) {
// 				giveup_current_region();
// 				abort_marking_if_regular_check_fail();
// 			} else if (_curr_region->is_humongous() && mr.start() == _curr_region->bottom()) {
// 				if (_next_mark_bitmap->is_marked(mr.start())) {
// 					// The object is marked - apply the closure
// 					bitmap_closure.do_addr(mr.start());
// 				}
// 				// Even if this task aborted while scanning the humongous object
// 				// we can (and should) give up the current region.
// 				giveup_current_region();
// 				abort_marking_if_regular_check_fail();
// 			} else if (_next_mark_bitmap->iterate(&bitmap_closure, mr)) {
// 				giveup_current_region();
// 				abort_marking_if_regular_check_fail();
// 			} else {
// 				assert(has_aborted(), "currently the only way to do so");
// 				// The only way to abort the bitmap iteration is to return
// 				// false from the do_bit() method. However, inside the
// 				// do_bit() method we move the _finger to point to the
// 				// object currently being looked at. So, if we bail out, we
// 				// have definitely set _finger to something non-null.
// 				assert(_finger != NULL, "invariant");

// 				// Region iteration was actually aborted. So now _finger
// 				// points to the address of the object we last scanned. If we
// 				// leave it there, when we restart this task, we will rescan
// 				// the object. It is easy to avoid this. We move the finger by
// 				// enough to point to the next possible object header.
// 				assert(_finger < _region_limit, "invariant");
// 				HeapWord* const new_finger = _finger + ((oop)_finger)->size();
// 				// Check if bitmap iteration was aborted while scanning the last object
// 				if (new_finger >= _region_limit) {
// 					giveup_current_region();
// 				} else {
// 					move_finger_to(new_finger);
// 				}
// 			}
// 		}
// 		// At this point we have either completed iterating over the
// 		// region we were holding on to, or we have aborted.

// 		// We then partially drain the local queue and the global stack.
// 		// (Do we really need this?)
// 		drain_local_queue(true);
// 		drain_global_stack(true);

// 		// Read the note on the claim_region() method on why it might
// 		// return NULL with potentially more regions available for
// 		// claiming and why we have to check out_of_regions() to determine
// 		// whether we're done or not.
// 		while (!has_aborted() && _curr_region == NULL && !_semeru_cm->out_of_regions()) {
// 			// We are going to try to claim a new region. We should have
// 			// given up on the previous one.
// 			// Separated the asserts so that we know which one fires.
// 			assert(_curr_region  == NULL, "invariant");
// 			assert(_finger       == NULL, "invariant");
// 			assert(_region_limit == NULL, "invariant");
// 			HeapRegion* claimed_region = _semeru_cm->claim_region(_worker_id);  // Claim a Region to concurrently mark.
// 			if (claimed_region != NULL) {
// 				// Yes, we managed to claim one
// 				setup_for_region(claimed_region);
// 				assert(_curr_region == claimed_region, "invariant");
// 			}
// 			// It is important to call the regular clock here. It might take
// 			// a while to claim a region if, for example, we hit a large
// 			// block of empty regions. So we need to call the regular clock
// 			// method once round the loop to make sure it's called
// 			// frequently enough.
// 			abort_marking_if_regular_check_fail();
// 		}

// 		if (!has_aborted() && _curr_region == NULL) {
// 			assert(_semeru_cm->out_of_regions(),
// 						 "at this point we should be out of regions");
// 		}
// 	} while ( _curr_region != NULL && !has_aborted());

// 	if (!has_aborted()) {
// 		// We cannot check whether the global stack is empty, since other
// 		// tasks might be pushing objects to it concurrently.
// 		assert(_semeru_cm->out_of_regions(),
// 					 "at this point we should be out of regions");
// 		// Try to reduce the number of available SATB buffers so that
// 		// remark has less work to do.
// 		drain_satb_buffers();
// 	}

// 	// Since we've done everything else, we can now totally drain the
// 	// local queue and global stack.
// 	drain_local_queue(false);
// 	drain_global_stack(false);

// 	// Attempt at work stealing from other task's queues.
// 	if (do_stealing && !has_aborted()) {
// 		// We have not aborted. This means that we have finished all that
// 		// we could. Let's try to do some stealing...

// 		// We cannot check whether the global stack is empty, since other
// 		// tasks might be pushing objects to it concurrently.
// 		assert(_semeru_cm->out_of_regions() && _semeru_task_queue->size() == 0,
// 					 "only way to reach here");
// 		while (!has_aborted()) {
// 			G1SemeruTaskQueueEntry entry;
// 			if (_semeru_cm->try_stealing(_worker_id, entry)) {
// 				scan_task_entry(entry);

// 				// And since we're towards the end, let's totally drain the
// 				// local queue and global stack.
// 				drain_local_queue(false);
// 				drain_global_stack(false);
// 			} else {
// 				break;
// 			}
// 		}
// 	}

// 	// We still haven't aborted. Now, let's try to get into the
// 	// termination protocol.
// 	if (do_termination && !has_aborted()) {
// 		// We cannot check whether the global stack is empty, since other
// 		// tasks might be concurrently pushing objects on it.
// 		// Separated the asserts so that we know which one fires.
// 		assert(_semeru_cm->out_of_regions(), "only way to reach here");
// 		assert(_semeru_task_queue->size() == 0, "only way to reach here");
// 		_termination_start_time_ms = os::elapsedVTime() * 1000.0;

// 		// The G1SemeruCMTask class also extends the TerminatorTerminator class,
// 		// hence its should_exit_termination() method will also decide
// 		// whether to exit the termination protocol or not.
// 		bool finished = (is_serial ||
// 										 _semeru_cm->terminator()->offer_termination(this));
// 		double termination_end_time_ms = os::elapsedVTime() * 1000.0;
// 		_termination_time_ms +=
// 			termination_end_time_ms - _termination_start_time_ms;

// 		if (finished) {
// 			// We're all done.

// 			// We can now guarantee that the global stack is empty, since
// 			// all other tasks have finished. We separated the guarantees so
// 			// that, if a condition is false, we can immediately find out
// 			// which one.
// 			guarantee(_semeru_cm->out_of_regions(), "only way to reach here");
// 			guarantee(_semeru_cm->mark_stack_empty(), "only way to reach here");
// 			guarantee(_semeru_task_queue->size() == 0, "only way to reach here");
// 			guarantee(!_semeru_cm->has_overflown(), "only way to reach here");
// 			guarantee(!has_aborted(), "should never happen if termination has completed");
// 		} else {
// 			// Apparently there's more work to do. Let's abort this task. It
// 			// will restart it and we can hopefully find more things to do.
// 			set_has_aborted();
// 		}
// 	}

// 	// Mainly for debugging purposes to make sure that a pointer to the
// 	// closure which was statically allocated in this frame doesn't
// 	// escape it by accident.
// 	set_cm_oop_closure(NULL);
// 	double end_time_ms = os::elapsedVTime() * 1000.0;
// 	double elapsed_time_ms = end_time_ms - _start_time_ms;
// 	// Update the step history.
// 	_step_times_ms.add(elapsed_time_ms);

// 	if (has_aborted()) {
// 		// The task was aborted for some reason.
// 		if (_has_timed_out) {
// 			double diff_ms = elapsed_time_ms - _time_target_ms;
// 			// Keep statistics of how well we did with respect to hitting
// 			// our target only if we actually timed out (if we aborted for
// 			// other reasons, then the results might get skewed).
// 			_marking_step_diffs_ms.add(diff_ms);
// 		}

// 		if (_semeru_cm->has_overflown()) {
// 			// This is the interesting one. We aborted because a global
// 			// overflow was raised. This means we have to restart the
// 			// marking phase and start iterating over regions. However, in
// 			// order to do this we have to make sure that all tasks stop
// 			// what they are doing and re-initialize in a safe manner. We
// 			// will achieve this with the use of two barrier sync points.

// 			if (!is_serial) {
// 				// We only need to enter the sync barrier if being called
// 				// from a parallel context
// 				_semeru_cm->enter_first_sync_barrier(_worker_id);

// 				// When we exit this sync barrier we know that all tasks have
// 				// stopped doing marking work. So, it's now safe to
// 				// re-initialize our data structures.
// 			}

// 			clear_region_fields();
// 			flush_mark_stats_cache();

// 			if (!is_serial) {
// 				// If we're executing the concurrent phase of marking, reset the marking
// 				// state; otherwise the marking state is reset after reference processing,
// 				// during the remark pause.
// 				// If we reset here as a result of an overflow during the remark we will
// 				// see assertion failures from any subsequent set_concurrency_and_phase()
// 				// calls.
// 				if (_semeru_cm->concurrent() && _worker_id == 0) {
// 					// Worker 0 is responsible for clearing the global data structures because
// 					// of an overflow. During STW we should not clear the overflow flag (in
// 					// G1SemeruConcurrentMark::reset_marking_state()) since we rely on it being true when we exit
// 					// method to abort the pause and restart concurrent marking.
// 					_semeru_cm->reset_marking_for_restart();

// 					log_info(gc, marking)("Concurrent Mark reset for overflow");
// 				}

// 				// ...and enter the second barrier.
// 				_semeru_cm->enter_second_sync_barrier(_worker_id);
// 			}
// 			// At this point, if we're during the concurrent phase of
// 			// marking, everything has been re-initialized and we're
// 			// ready to restart.
// 		}
// 	}

} // end of do_marking_step.




/**
 * Semeru Memory Server  
 * 
 * [?] This function is used for concurrently marking one region ?
 * 			=> We need a scheduler function to control the marking sequence for all the Regions in Memory Server CSet.
 * 
 * [?] Both Concurrent Marking and STW Remark use this function.
 * 
 * [?] how to let this function to support both Concurrent and STW ?
 * 
 */
void G1SemeruCMTask::do_semeru_marking_step(double time_target_ms,
															 bool do_termination,
															 bool is_serial) {
	assert(time_target_ms >= 1.0, "minimum granularity is 1ms");


	_start_time_ms = os::elapsedVTime() * 1000.0;

	// // If do_stealing is true then do_marking_step will attempt to
	// // steal work from the other G1SemeruCMTasks. It only makes sense to
	// // enable stealing when the termination protocol is enabled
	// // and do_marking_step() is not being called serially.
	// bool do_stealing = do_termination && !is_serial;

	double diff_prediction_ms = _semeru_h->g1_policy()->predictor().get_new_prediction(&_marking_step_diffs_ms);
	_time_target_ms = time_target_ms - diff_prediction_ms;		//[?] the estimated time for current marking step ??

	// // set up the variables that are used in the work-based scheme to
	// // call the regular clock method
	// _words_scanned = 0;
	// _refs_reached  = 0;
	// recalculate_limits();

	// clear all flags
	clear_has_aborted();				// [?] What's this used for ?
	_has_timed_out = false;
	// _draining_satb_buffers = false;

	++_calls;

	// Set up the bitmap and oop closures. Anything that uses them is
	// eventually called from this method, so it is OK to allocate these
	// statically.

	// 1) Scan the HeapRegion's bitmap, does Semeru need this one ?
	// 		seems not. Semeru only need to mark all the alive objects in the alive_bitmap.
	//G1CMBitMapClosure bitmap_closure(this, _semeru_cm);				// Semeru only use the alive_bitmap to mark alive objects.
	//G1CMOopClosure cm_oop_closure(_semeru_h, this);
	//set_cm_oop_closure(&cm_oop_closure);

	G1SemeruCMOopClosure semeru_cm_oop_closure(_semeru_h, this); 
	set_cm_oop_closure(&semeru_cm_oop_closure);

	// [?] we need a closure to scan the Target Obj Queue separately ?

	// the global task queue G1SemeruConcurrentMark->_global_mark_stack is also overflowed.
	// Then we have to abort this scanning.
	if (_semeru_cm->has_overflown()) {
		// This can happen if the mark stack overflows during a GC pause
		// and this task, after a yield point, restarts. We have to abort
		// as we need to get into the overflow protocol which happens
		// right at the end of this task.
		set_has_aborted();
	}


	// SATB queue is only maintained in CPU Server.
	// First drain any available SATB buffers. After this, we will not
	// look at SATB buffers before the next invocation of this method.
	// If enough completed SATB buffers are queued up, the regular clock
	// will abort this task so that it restarts.
	//drain_satb_buffers();
	
	// For semeru, this queue should be empty now before scan the Target Object Queue.
	// // ...then partially drain the local queue and the global stack
	// // [?] why do these 2 partially drain before scan the  Target Object Queue.
	// drain_local_queue(true);
	// drain_global_stack(true);

	//
	// Concurrently scan all the Regions in Memory Server's Collection Set. 
	//
	do {

		//
		// Tracing the Region pointed by _curr_region.
		//
		if (!has_aborted() && _curr_region != NULL) {  // [?] Should already make the _curr_region point to the first region of CSet.
			// This means that we're already holding on to a region.
			

			// 1.1) Handle humonguous objects separately
			// [?] _curr_region can the start of a humongous region or in the midle of a humongous obejcts ?
			//	=> Only scan the first humongous Region occupied by the humongous objects.
			if (_curr_region->is_humongous()) {
				
				assert(_curr_region->used()!=0, "%s, Can't be empty humongous Region. ", __func__);

				// 1) Humongous object is larger than HeapRegion size/2
				// 2) Humongous object allocation is always HeapRegion alignment.
				// 3) One humongous object can spread several continous HeapRegions.
				if ( _curr_region->is_starts_humongous() &&  
							_alive_bitmap->is_marked(_curr_region->bottom())) {

					// The object is marked - apply the closure
					// [?] Can this detect if this is a oop start address ??
					//			=> seem no. if not start of a oop, will crash.
					// [x] Just push this humongous objects into task_queue  ?
					//			=> No need to mark the humongous's bitmap. 
					//			=> Mark its fields alive and push them into task_queue.
					// [x] How to mark multiple Regions' alive_bitmap ??
					//			=> No need to mark any humongous region's alive_bitmap.
					//bitmap_closure.do_addr(_curr_region->bottom());  

					if(semeru_cm_task_do_addr(_curr_region->bottom()) ){
						// if scan failed,
						assert(false, "%s, process humongous Region error.",__func__);
						return; 
					}

				}

				// 1) After handle a humongous Region, reset all the  fields pointed to this Region.
				// 2) No need to process continuous humongous Regions, just skip them.
				//			[?] How to record the already scanned region , Semeru Memory Server CSet ?
				//							=> Can we use the global  G1SemeruConcurrentMark->_finger ??
				// Even if this task aborted while scanning the humongous object
				// we can (and should) give up the current region.
				giveup_current_region();
				abort_marking_if_regular_check_fail();

			} else{
			
				// 1.2) Process a Normal Region.

				// Concurrently Scan the Region poited by _curr_region.
				TargetObjQueue* target_obj_q = _curr_region->target_obj_queue();
				trim_target_object_queue(target_obj_q);

			}
	

			// // Semeru memory server doesn't need the _finger.
			// assert(_finger != NULL, "if region is not NULL, then the finger "
			//  			 "should not be NULL either");

			// // We might have restarted this task after an evacuation pause
			// // which might have evacuated the region we're holding on to
			// // underneath our feet. Let's read its limit again to make sure
			// // that we do not iterate over a region of the heap that
			// // contains garbage (update_region_limit() will also move
			// // _finger to the start of the region if it is found empty).
			// update_region_limit();


			// // We will start from _finger not from the start of the region,
			// // as we might be restarting this task after aborting half-way
			// // through scanning this region. In this case, _finger points to
			// // the address where we last found a marked object. If this is a
			// // fresh region, _finger points to start().
			// MemRegion mr = MemRegion(_finger, _region_limit);

			// assert(!_curr_region->is_humongous() || mr.start() == _curr_region->bottom(),
			// 			 "humongous regions should go around loop once only");

			// // Some special cases:
			// // If the memory region is empty, we can just give up the region.
			// // If the current region is humongous then we only need to check
			// // the bitmap for the bit associated with the start of the object,
			// // scan the object if it's live, and give up the region.
			// // Otherwise, let's iterate over the bitmap of the part of the region
			// // that is left.
			// // If the iteration is successful, give up the region.
			// if (mr.is_empty()) {
			// 	giveup_current_region();
			// 	abort_marking_if_regular_check_fail();
			// } else if (_curr_region->is_humongous() && mr.start() == _curr_region->bottom()) {
			// 	if (_next_mark_bitmap->is_marked(mr.start())) {
			// 		// The object is marked - apply the closure
			// 		bitmap_closure.do_addr(mr.start());
			// 	}
			// 	// Even if this task aborted while scanning the humongous object
			// 	// we can (and should) give up the current region.
			// 	giveup_current_region();
			// 	abort_marking_if_regular_check_fail();
			// } else if (_next_mark_bitmap->iterate(&bitmap_closure, mr)) {
			// 	giveup_current_region();
			// 	abort_marking_if_regular_check_fail();
			// } else {
			// 	assert(has_aborted(), "currently the only way to do so");
			// 	// The only way to abort the bitmap iteration is to return
			// 	// false from the do_bit() method. However, inside the
			// 	// do_bit() method we move the _finger to point to the
			// 	// object currently being looked at. So, if we bail out, we
			// 	// have definitely set _finger to something non-null.
			// 	assert(_finger != NULL, "invariant");

			// 	// Region iteration was actually aborted. So now _finger
			// 	// points to the address of the object we last scanned. If we
			// 	// leave it there, when we restart this task, we will rescan
			// 	// the object. It is easy to avoid this. We move the finger by
			// 	// enough to point to the next possible object header.
			// 	assert(_finger < _region_limit, "invariant");
			// 	HeapWord* const new_finger = _finger + ((oop)_finger)->size();
			// 	// Check if bitmap iteration was aborted while scanning the last object
			// 	if (new_finger >= _region_limit) {
			// 		giveup_current_region();
			// 	} else {
			// 		move_finger_to(new_finger);
			// 	}
			// }

		}
		// At this point we have either completed iterating over the
		// region we were holding on to, or we have aborted.

		// We then partially drain the local queue and the global stack.
		// (Do we really need this?)
		drain_local_queue(true);
		drain_global_stack(true);


		// 2) Claim a NEW Region from Semeru memory server's CSet to scan.


		// Read the note on the claim_region() method on why it might
		// return NULL with potentially more regions available for
		// claiming and why we have to check out_of_memory_server_cset() to determine
		// whether we're done or not.
		//while (!has_aborted() && _curr_region == NULL && !_semeru_cm->out_of_memory_server_cset()) {
			
		while (!has_aborted() && !_semeru_cm->out_of_memory_server_cset()) {
			// We are going to try to claim a new region. We should have
			// given up on the previous one.
			// Separated the asserts so that we know which one fires.
			//assert(_curr_region  == NULL, "invariant");
			assert(_finger       == NULL, "invariant");
			assert(_region_limit == NULL, "invariant");
			HeapRegion* claimed_region = _semeru_cm->claim_region(_worker_id);  // Claim a Region to concurrently mark.
			if (claimed_region != NULL) {
				// Yes, we managed to claim one
				setup_for_region(claimed_region);
				assert(_curr_region == claimed_region, "invariant");
			}

			// It is important to call the regular clock here. It might take
			// a while to claim a region if, for example, we hit a large
			// block of empty regions. So we need to call the regular clock
			// method once round the loop to make sure it's called
			// frequently enough.
			abort_marking_if_regular_check_fail();
		}


		// End of CSet processing.
		// If reach here, all the regions in memory server CSet should be already processed already.
		if (!has_aborted() && _curr_region == NULL) {
			assert(_semeru_cm->out_of_memory_server_cset(),
						 "at this point we should be out of regions");
		}


	} while ( _curr_region != NULL && !has_aborted());

	if (!has_aborted()) {
		// We cannot check whether the global stack is empty, since other
		// tasks might be pushing objects to it concurrently.
		assert(_semeru_cm->out_of_memory_server_cset(),
					 "at this point we should be out of regions");
		
		// Semeru memory servers don't have SATB buffer.
		
		// Try to reduce the number of available SATB buffers so that
		// remark has less work to do.
		//drain_satb_buffers();
	}

	// Since we've done everything else, we can now totally drain the
	// local queue and global stack.
	drain_local_queue(false);
	drain_global_stack(false);



	// [?] Because the Semeru Region is quit big, how about scan reach Region parallelly.
	// 		Then how to apply work stealing in this case.

	// Attempt at work stealing from other task's queues.
	// if (do_stealing && !has_aborted()) {
	// 	// We have not aborted. This means that we have finished all that
	// 	// we could. Let's try to do some stealing...

	// 	// We cannot check whether the global stack is empty, since other
	// 	// tasks might be pushing objects to it concurrently.
	// 	assert(_semeru_cm->out_of_memory_server_cset() && _semeru_task_queue->size() == 0,
	// 				 "only way to reach here");
	// 	while (!has_aborted()) {
	// 		G1SemeruTaskQueueEntry entry;
	// 		if (_semeru_cm->try_stealing(_worker_id, entry)) {
	// 			scan_task_entry(entry);

	// 			// And since we're towards the end, let's totally drain the
	// 			// local queue and global stack.
	// 			drain_local_queue(false);
	// 			drain_global_stack(false);
	// 		} else {
	// 			break;
	// 		}
	// 	}
	// }


	//
	// [?] What's the termination protocol used for ??
	//

	// // We still haven't aborted. Now, let's try to get into the
	// // termination protocol.
	// if (do_termination && !has_aborted()) {
	// 	// We cannot check whether the global stack is empty, since other
	// 	// tasks might be concurrently pushing objects on it.
	// 	// Separated the asserts so that we know which one fires.
	// 	assert(_semeru_cm->out_of_memory_server_cset(), "only way to reach here");
	// 	assert(_semeru_task_queue->size() == 0, "only way to reach here");
	// 	_termination_start_time_ms = os::elapsedVTime() * 1000.0;

	// 	// The G1SemeruCMTask class also extends the TerminatorTerminator class,
	// 	// hence its should_exit_termination() method will also decide
	// 	// whether to exit the termination protocol or not.
	// 	bool finished = (is_serial ||
	// 									 _semeru_cm->terminator()->offer_termination(this));
	// 	double termination_end_time_ms = os::elapsedVTime() * 1000.0;
	// 	_termination_time_ms +=
	// 		termination_end_time_ms - _termination_start_time_ms;

	// 	if (finished) {
	// 		// We're all done.

	// 		// We can now guarantee that the global stack is empty, since
	// 		// all other tasks have finished. We separated the guarantees so
	// 		// that, if a condition is false, we can immediately find out
	// 		// which one.
	// 		guarantee(_semeru_cm->out_of_memory_server_cset(), "only way to reach here");
	// 		guarantee(_semeru_cm->mark_stack_empty(), "only way to reach here");
	// 		guarantee(_semeru_task_queue->size() == 0, "only way to reach here");
	// 		guarantee(!_semeru_cm->has_overflown(), "only way to reach here");
	// 		guarantee(!has_aborted(), "should never happen if termination has completed");
	// 	} else {
	// 		// Apparently there's more work to do. Let's abort this task. It
	// 		// will restart it and we can hopefully find more things to do.
	// 		set_has_aborted();
	// 	}
		
	// }  // termination protocol


	//
	// End of the concurrent marking OR STW Remark.
	//  1) if have aborted, reset all the fields and try to restart the concurent marking.
	//  

	// Mainly for debugging purposes to make sure that a pointer to the
	// closure which was statically allocated in this frame doesn't
	// escape it by accident.
	set_cm_oop_closure(NULL);
	double end_time_ms = os::elapsedVTime() * 1000.0;
	double elapsed_time_ms = end_time_ms - _start_time_ms;
	// Update the step history.
	_step_times_ms.add(elapsed_time_ms);

	if (has_aborted()) {
		// The task was aborted for some reason.
		if (_has_timed_out) {
			double diff_ms = elapsed_time_ms - _time_target_ms;
			// Keep statistics of how well we did with respect to hitting
			// our target only if we actually timed out (if we aborted for
			// other reasons, then the results might get skewed).
			_marking_step_diffs_ms.add(diff_ms);
		}

		if (_semeru_cm->has_overflown()) {
			// This is the interesting one. We aborted because a global
			// overflow was raised. This means we have to restart the
			// marking phase and start iterating over regions. However, in
			// order to do this we have to make sure that all tasks stop
			// what they are doing and re-initialize in a safe manner. We
			// will achieve this with the use of two barrier sync points.

			if (!is_serial) {
				// We only need to enter the sync barrier if being called
				// from a parallel context
				_semeru_cm->enter_first_sync_barrier(_worker_id);

				// When we exit this sync barrier we know that all tasks have
				// stopped doing marking work. So, it's now safe to
				// re-initialize our data structures.
			}

			clear_region_fields();
			flush_mark_stats_cache();

			if (!is_serial) {
				// If we're executing the concurrent phase of marking, reset the marking
				// state; otherwise the marking state is reset after reference processing,
				// during the remark pause.
				// If we reset here as a result of an overflow during the remark we will
				// see assertion failures from any subsequent set_concurrency_and_phase()
				// calls.
				if (_semeru_cm->concurrent() && _worker_id == 0) { 
					// Worker 0 is responsible for clearing the global data structures because
					// of an overflow. During STW we should not clear the overflow flag (in
					// G1SemeruConcurrentMark::reset_marking_state()) since we rely on it being true when we exit
					// method to abort the pause and restart concurrent marking.
					_semeru_cm->reset_marking_for_restart();

					log_info(gc, marking)("Concurrent Mark reset for overflow");
				}

				// ...and enter the second barrier.
				_semeru_cm->enter_second_sync_barrier(_worker_id);
			}
			// At this point, if we're during the concurrent phase of
			// marking, everything has been re-initialized and we're
			// ready to restart.
		}
	}

} // end of do_marking_step.





G1SemeruCMTask::G1SemeruCMTask(uint worker_id,
									 G1SemeruConcurrentMark* cm,
									 G1SemeruCMTaskQueue* task_queue,
									 G1RegionMarkStats* mark_stats,
									 uint max_regions) :
	_objArray_processor(this),
	_worker_id(worker_id),
	_semeru_h(G1SemeruCollectedHeap::heap()),
	_semeru_cm(cm),
	_next_mark_bitmap(NULL),
	_semeru_task_queue(task_queue),
	_mark_stats_cache(mark_stats, max_regions, RegionMarkStatsCacheSize),
	_calls(0),
	_time_target_ms(0.0),
	_start_time_ms(0.0),
	_semeru_cm_oop_closure(NULL),
	_curr_region(NULL),
	_finger(NULL),
	_region_limit(NULL),
	_words_scanned(0),
	_words_scanned_limit(0),
	_real_words_scanned_limit(0),
	_refs_reached(0),
	_refs_reached_limit(0),
	_real_refs_reached_limit(0),
	_has_aborted(false),
	_has_timed_out(false),
	_draining_satb_buffers(false),
	_step_times_ms(),
	_elapsed_time_ms(0.0),
	_termination_time_ms(0.0),
	_termination_start_time_ms(0.0),
	_marking_step_diffs_ms()
{
	guarantee(task_queue != NULL, "invariant");

	_marking_step_diffs_ms.add(0.5);
}

// These are formatting macros that are used below to ensure
// consistent formatting. The *_H_* versions are used to format the
// header for a particular value and they should be kept consistent
// with the corresponding macro. Also note that most of the macros add
// the necessary white space (as a prefix) which makes them a bit
// easier to compose.

// All the output lines are prefixed with this string to be able to
// identify them easily in a large log file.
#define G1PPRL_LINE_PREFIX            "###"

#define G1PPRL_ADDR_BASE_FORMAT    " " PTR_FORMAT "-" PTR_FORMAT
#ifdef _LP64
#define G1PPRL_ADDR_BASE_H_FORMAT  " %37s"
#else // _LP64
#define G1PPRL_ADDR_BASE_H_FORMAT  " %21s"
#endif // _LP64

// For per-region info
#define G1PPRL_TYPE_FORMAT            "   %-4s"
#define G1PPRL_TYPE_H_FORMAT          "   %4s"
#define G1PPRL_STATE_FORMAT           "   %-5s"
#define G1PPRL_STATE_H_FORMAT         "   %5s"
#define G1PPRL_BYTE_FORMAT            "  " SIZE_FORMAT_W(9)
#define G1PPRL_BYTE_H_FORMAT          "  %9s"
#define G1PPRL_DOUBLE_FORMAT          "  %14.1f"
#define G1PPRL_DOUBLE_H_FORMAT        "  %14s"

// For summary info
#define G1PPRL_SUM_ADDR_FORMAT(tag)    "  " tag ":" G1PPRL_ADDR_BASE_FORMAT
#define G1PPRL_SUM_BYTE_FORMAT(tag)    "  " tag ": " SIZE_FORMAT
#define G1PPRL_SUM_MB_FORMAT(tag)      "  " tag ": %1.2f MB"
#define G1PPRL_SUM_MB_PERC_FORMAT(tag) G1PPRL_SUM_MB_FORMAT(tag) " / %1.2f %%"

G1SemeruPrintRegionLivenessInfoClosure::G1SemeruPrintRegionLivenessInfoClosure(const char* phase_name) :
	_total_used_bytes(0), _total_capacity_bytes(0),
	_total_prev_live_bytes(0), _total_next_live_bytes(0),
	_total_remset_bytes(0), _total_strong_code_roots_bytes(0)
{
	if (!log_is_enabled(Trace, gc, liveness)) {
		return;
	}

	G1SemeruCollectedHeap* g1h = G1SemeruCollectedHeap::heap();
	MemRegion g1_reserved = g1h->g1_reserved();
	double now = os::elapsedTime();

	// Print the header of the output.
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX" PHASE %s @ %1.3f", phase_name, now);
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX" HEAP"
													G1PPRL_SUM_ADDR_FORMAT("reserved")
													G1PPRL_SUM_BYTE_FORMAT("region-size"),
													p2i(g1_reserved.start()), p2i(g1_reserved.end()),
													HeapRegion::GrainBytes);
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX);
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX
													G1PPRL_TYPE_H_FORMAT
													G1PPRL_ADDR_BASE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_DOUBLE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_STATE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT,
													"type", "address-range",
													"used", "prev-live", "next-live", "gc-eff",
													"remset", "state", "code-roots");
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX
													G1PPRL_TYPE_H_FORMAT
													G1PPRL_ADDR_BASE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_DOUBLE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT
													G1PPRL_STATE_H_FORMAT
													G1PPRL_BYTE_H_FORMAT,
													"", "",
													"(bytes)", "(bytes)", "(bytes)", "(bytes/ms)",
													"(bytes)", "", "(bytes)");
}

bool G1SemeruPrintRegionLivenessInfoClosure::do_heap_region(HeapRegion* r) {
	if (!log_is_enabled(Trace, gc, liveness)) {
		return false;
	}

	const char* type       = r->get_type_str();
	HeapWord* bottom       = r->bottom();
	HeapWord* end          = r->end();
	size_t capacity_bytes  = r->capacity();
	size_t used_bytes      = r->used();
	size_t prev_live_bytes = r->live_bytes();
	size_t next_live_bytes = r->next_live_bytes();
	double gc_eff          = r->gc_efficiency();
	size_t remset_bytes    = r->rem_set()->mem_size();
	size_t strong_code_roots_bytes = r->rem_set()->strong_code_roots_mem_size();
	const char* remset_type = r->rem_set()->get_short_state_str();

	_total_used_bytes      += used_bytes;
	_total_capacity_bytes  += capacity_bytes;
	_total_prev_live_bytes += prev_live_bytes;
	_total_next_live_bytes += next_live_bytes;
	_total_remset_bytes    += remset_bytes;
	_total_strong_code_roots_bytes += strong_code_roots_bytes;

	// Print a line for this particular region.
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX
													G1PPRL_TYPE_FORMAT
													G1PPRL_ADDR_BASE_FORMAT
													G1PPRL_BYTE_FORMAT
													G1PPRL_BYTE_FORMAT
													G1PPRL_BYTE_FORMAT
													G1PPRL_DOUBLE_FORMAT
													G1PPRL_BYTE_FORMAT
													G1PPRL_STATE_FORMAT
													G1PPRL_BYTE_FORMAT,
													type, p2i(bottom), p2i(end),
													used_bytes, prev_live_bytes, next_live_bytes, gc_eff,
													remset_bytes, remset_type, strong_code_roots_bytes);

	return false;
}

G1SemeruPrintRegionLivenessInfoClosure::~G1SemeruPrintRegionLivenessInfoClosure() {
	if (!log_is_enabled(Trace, gc, liveness)) {
		return;
	}

	// add static memory usages to remembered set sizes
	_total_remset_bytes += HeapRegionRemSet::fl_mem_size() + HeapRegionRemSet::static_mem_size();
	// Print the footer of the output.
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX);
	log_trace(gc, liveness)(G1PPRL_LINE_PREFIX
												 " SUMMARY"
												 G1PPRL_SUM_MB_FORMAT("capacity")
												 G1PPRL_SUM_MB_PERC_FORMAT("used")
												 G1PPRL_SUM_MB_PERC_FORMAT("prev-live")
												 G1PPRL_SUM_MB_PERC_FORMAT("next-live")
												 G1PPRL_SUM_MB_FORMAT("remset")
												 G1PPRL_SUM_MB_FORMAT("code-roots"),
												 bytes_to_mb(_total_capacity_bytes),
												 bytes_to_mb(_total_used_bytes),
												 percent_of(_total_used_bytes, _total_capacity_bytes),
												 bytes_to_mb(_total_prev_live_bytes),
												 percent_of(_total_prev_live_bytes, _total_capacity_bytes),
												 bytes_to_mb(_total_next_live_bytes),
												 percent_of(_total_next_live_bytes, _total_capacity_bytes),
												 bytes_to_mb(_total_remset_bytes),
												 bytes_to_mb(_total_strong_code_roots_bytes));
}