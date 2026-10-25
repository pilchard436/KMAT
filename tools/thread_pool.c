#include "thread_pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define NUM_COUNTER 8
#define CANCELLATION_COUNTER_INDEX 3

int counter[NUM_COUNTER];
thread_mutex_t counter_lock[NUM_COUNTER];
static int counters_prepared;
static size_t worker_count_limit = THREAD_POOL_MAX_WORKERS;
static atomic_int cancellation_counter = ATOMIC_VAR_INIT(0);

#ifdef _WIN32
static INIT_ONCE counter_locks_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK initialize_counter_locks(PINIT_ONCE once, PVOID parameter, PVOID *context) {
	(void)once;
	(void)parameter;
	(void)context;
	for(int i = 0; i < NUM_COUNTER; i++) InitializeCriticalSection(&counter_lock[i]);
	return TRUE;
}

static void ensure_counter_locks_initialized(void) {
	InitOnceExecuteOnce(&counter_locks_once, initialize_counter_locks, NULL, NULL);
}
#else
static pthread_once_t counter_locks_once = PTHREAD_ONCE_INIT;

static void initialize_counter_locks(void) {
	for(int i = 0; i < NUM_COUNTER; i++) pthread_mutex_init(&counter_lock[i], NULL);
}

static void ensure_counter_locks_initialized(void) {
	pthread_once(&counter_locks_once, initialize_counter_locks);
}
#endif

void prepare_thread_pool_counters(void) {
	ensure_counter_locks_initialized();
	for(int i = 0; i < NUM_COUNTER; i++) counter[i] = 0;
	atomic_store_explicit(&cancellation_counter, 0, memory_order_relaxed);
	counters_prepared = 1;
}

static void prepare_counters_for_pool(void) {
	ensure_counter_locks_initialized();
	if(!counters_prepared) {
		for(int i = 0; i < NUM_COUNTER; i++) counter[i] = 0;
		atomic_store_explicit(&cancellation_counter, 0, memory_order_relaxed);
	}
	counters_prepared = 0;
}

static size_t get_worker_count(void) {
#ifdef _WIN32
	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);
	size_t processor_count = system_info.dwNumberOfProcessors;
#else
	long available_processors = sysconf(_SC_NPROCESSORS_ONLN);
	size_t processor_count = available_processors > 0 ? (size_t)available_processors : 1;
#endif
	if(processor_count > 1) processor_count--;
	if(processor_count > 32) processor_count = 32;
	if(processor_count > worker_count_limit) processor_count = worker_count_limit;
	return processor_count > 0 ? processor_count : 1;
}

void set_thread_pool_worker_limit(size_t max_workers) {
	worker_count_limit = max_workers > THREAD_POOL_MAX_WORKERS ?
		THREAD_POOL_MAX_WORKERS : max_workers;
}

static struct Thread_Pool create_thread_pool32(void *thread_method(void*), void *thread_args,
		int reset_counters, size_t max_workers) {
	size_t size = get_worker_count();
	if(size > max_workers) size = max_workers;
	struct Thread_Pool thread_pool = {.size = size};

	if(size == 0) return thread_pool;
	if(reset_counters) prepare_counters_for_pool();

	// Create threads
	for (int i = 0; i < thread_pool.size; i++) {
		#ifdef _WIN32
			thread_pool.threads[i] = CreateThread(
					NULL,                        // default security attributes
					0,                           // default stack size
					(LPTHREAD_START_ROUTINE) thread_method, // pointer to the thread function
					thread_args,                 // argument to thread function
					0,                           // default creation flags
					NULL);                       // thread ID (not needed in this case)

			if (thread_pool.threads[i] == NULL) {
				fprintf(stderr, "Error creating thread %d\n", i);
				exit(EXIT_FAILURE);
			}
		#else
			if (pthread_create(&thread_pool.threads[i], NULL, thread_method, thread_args) != 0) {
				perror("pthread_create");
				exit(EXIT_FAILURE);
			}
		#endif
	}

	return thread_pool;
}

struct Thread_Pool use_thread_pool32(void *thread_method(void*), void *thread_args) {
	return create_thread_pool32(thread_method, thread_args, 1, THREAD_POOL_MAX_WORKERS);
}

struct Thread_Pool use_thread_pool32_preserving_counters(void *thread_method(void*), void *thread_args) {
	return create_thread_pool32(thread_method, thread_args, 0, THREAD_POOL_MAX_WORKERS);
}

struct Thread_Pool use_thread_pool32_preserving_counters_with_limit(void *thread_method(void*),
		void *thread_args, size_t max_workers) {
	return create_thread_pool32(thread_method, thread_args, 0, max_workers);
}

size_t get_thread_pool32_worker_capacity(void) {
	return get_worker_count();
}

struct Thread_Pool use_thread_pool01(void *thread_method(void*), void *thread_args) {
	struct Thread_Pool thread_pool = {.size = 1};

	prepare_counters_for_pool();

	// Create threads
	for (int i = 0; i < thread_pool.size; i++) {
#ifdef _WIN32
		thread_pool.threads[i] = CreateThread(
				NULL,                        // default security attributes
				0,                           // default stack size
				(LPTHREAD_START_ROUTINE) thread_method, // pointer to the thread function
				thread_args,                 // argument to thread function
				0,                           // default creation flags
				NULL);                       // thread ID (not needed in this case)

		if (thread_pool.threads[i] == NULL) {
			fprintf(stderr, "Error creating thread %d\n", i);
			exit(EXIT_FAILURE);
		}
#else
		if (pthread_create(&thread_pool.threads[i], NULL, thread_method, thread_args) != 0) {
				perror("pthread_create");
				exit(EXIT_FAILURE);
			}
#endif
	}

	return thread_pool;
}

int get_thread_counter(int counter_index) {
	/* Cancellation polling is hot; writers maintain this atomic mirror under the counter lock. */
	if(counter_index == CANCELLATION_COUNTER_INDEX)
		return atomic_load_explicit(&cancellation_counter, memory_order_relaxed);
	ensure_counter_locks_initialized();
	int counter_value;
	#ifdef _WIN32
		EnterCriticalSection(&counter_lock[counter_index]);
	#else
		pthread_mutex_lock(&counter_lock[counter_index]);
	#endif
	counter_value = counter[counter_index];
	#ifdef _WIN32
		LeaveCriticalSection(&counter_lock[counter_index]);
	#else
		pthread_mutex_unlock(&counter_lock[counter_index]);
	#endif
	return counter_value;
}

int get_incr_thread_counter(int counter_index) {
	ensure_counter_locks_initialized();
	int counter_value;
	#ifdef _WIN32
		EnterCriticalSection(&counter_lock[counter_index]); // Windows lock
	#else
		pthread_mutex_lock(&counter_lock[counter_index]); // Linux lock
	#endif
	counter_value = counter[counter_index];
	counter[counter_index]++;
	if(counter_index == CANCELLATION_COUNTER_INDEX)
		atomic_store_explicit(&cancellation_counter, counter[counter_index], memory_order_relaxed);
	#ifdef _WIN32
		LeaveCriticalSection(&counter_lock[counter_index]); // Windows unlock
	#else
		pthread_mutex_unlock(&counter_lock[counter_index]); // Linux unlock
	#endif
	return counter_value;
}

void incr_thread_counter_by_amount(int counter_index, int amount) {
	ensure_counter_locks_initialized();
	#ifdef _WIN32
		EnterCriticalSection(&counter_lock[counter_index]); // Windows lock
	#else
		pthread_mutex_lock(&counter_lock[counter_index]); // Linux lock
	#endif
	counter[counter_index] += amount;
	if(counter_index == CANCELLATION_COUNTER_INDEX)
		atomic_store_explicit(&cancellation_counter, counter[counter_index], memory_order_relaxed);
	#ifdef _WIN32
		LeaveCriticalSection(&counter_lock[counter_index]); // Windows unlock
	#else
		pthread_mutex_unlock(&counter_lock[counter_index]); // Linux unlock
	#endif
}

void join_thread_pool(struct Thread_Pool thread_pool) {
    for(int i = 0; i < thread_pool.size; i++) {
		#ifdef _WIN32
				WaitForSingleObject(thread_pool.threads[i], INFINITE); // Windows join
		#else
				pthread_join(thread_pool.threads[i], NULL); // Linux join
		#endif
    }
}