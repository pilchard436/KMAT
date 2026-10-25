#ifndef KSP_THREAD_POOL_H
#define KSP_THREAD_POOL_H

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;  // Use Windows HANDLE for threads
typedef CRITICAL_SECTION thread_mutex_t;  // Use Windows CRITICAL_SECTION for mutexes
#else
#include <pthread.h>
typedef pthread_t thread_t;  // Use pthread_t for Linux
typedef pthread_mutex_t thread_mutex_t;  // Use pthread_mutex_t for Linux
#endif

#define THREAD_POOL_MAX_WORKERS 32

/**
 * @brief Represents a thread pool containing multiple threads and the number of threads in the pool
 */
struct Thread_Pool {
	thread_t threads[THREAD_POOL_MAX_WORKERS]; /**< Handles owned by this pool */
	size_t size;        /**< Size of the thread pool */
};


/**
 * @brief Creates, initializes, and runs up to 32 workers, reserving one logical processor when possible.
 *
 * @param thread_method Pointer to the thread method function
 * @param thread_args   Pointer to the arguments for the thread method
 * @return A populated Thread_Pool structure with the selected worker count
 */
struct Thread_Pool use_thread_pool32(void *thread_method(void*), void *thread_args);
/**
 * @brief Creates a worker pool without resetting the shared thread counters.
 *
 * Use when worker scheduling is owned by the caller and existing cancellation/progress counters
 * must remain available to the workers and caller.
 */
struct Thread_Pool use_thread_pool32_preserving_counters(void *thread_method(void*), void *thread_args);
struct Thread_Pool use_thread_pool32_preserving_counters_with_limit(
	void *thread_method(void*), void *thread_args, size_t max_workers);
/* Caps process-wide worker creation; set before starting any pools. */
void set_thread_pool_worker_limit(size_t max_workers);
struct Thread_Pool use_thread_pool01(void *thread_method(void*), void *thread_args);
size_t get_thread_pool32_worker_capacity(void);

// Initialize/reset counters before exposing thread-pool controls to another thread.
void prepare_thread_pool_counters(void);

/**
 * @brief Gets the current specified thread counter value
 *
 * @param counter_index The index of the specified thread counter
 * @return The current value of the thread counter
 */
int get_thread_counter(int counter_index);


/**
 * @brief Gets the current specified thread counter value and increases it by one
 *
 * @param counter_index The index of the specified thread counter
 * @return The current value of the thread counter
 */
int get_incr_thread_counter(int counter_index);


/**
 * @brief Waits for all threads in the specified thread pool to finish.
 *
 * @param thread_pool The thread pool structure to be joined.
 */
void join_thread_pool(struct Thread_Pool thread_pool);


/**
 * @brief Increases specified thread counter by given amount
 *
 * @param counter_index The index of the specified thread counter
 * @param amount The amount the counter should be increased by
 */
void incr_thread_counter_by_amount(int counter_index, int amount);



#endif //KSP_THREAD_POOL_H
