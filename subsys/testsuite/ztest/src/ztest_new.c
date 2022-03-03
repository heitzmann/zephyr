/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ztest.h>
#include <stdio.h>
#include <app_memory/app_memdomain.h>
#ifdef CONFIG_USERSPACE
#include <sys/libc-hooks.h>
#endif
#include <sys/reboot.h>
#include <logging/log_ctrl.h>

#ifdef KERNEL
static struct k_thread ztest_thread;
#endif

#ifdef CONFIG_ARCH_POSIX
#include <unistd.h>
#endif

/* ZTEST_DMEM and ZTEST_BMEM are used for the application shared memory test  */

/**
 * @brief Each enum member represents a distinct phase of execution for the
 *        test binary. TEST_PHASE_FRAMEWORK is active when internal ztest code
 *        is executing; the rest refer to corresponding phases of user test
 *        code.
 */
enum ztest_phase {
	TEST_PHASE_SETUP,
	TEST_PHASE_BEFORE,
	TEST_PHASE_TEST,
	TEST_PHASE_AFTER,
	TEST_PHASE_TEARDOWN,
	TEST_PHASE_FRAMEWORK
};

/**
 * @brief Tracks the current phase that ztest is operating in.
 */
ZTEST_DMEM enum ztest_phase phase = TEST_PHASE_FRAMEWORK;

static ZTEST_BMEM int test_status;

/**
 * @brief Try to shorten a filename by removing the current directory
 *
 * This helps to reduce the very long filenames in assertion failures. It
 * removes the current directory from the filename and returns the rest.
 * This makes assertions a lot more readable, and sometimes they fit on one
 * line.
 *
 * @param file Filename to check
 * @returns Shortened filename, or @file if it could not be shortened
 */
const char *ztest_relative_filename(const char *file)
{
#ifdef CONFIG_ARCH_POSIX
	const char *cwd;
	char buf[200];

	cwd = getcwd(buf, sizeof(buf));
	if (cwd && strlen(file) > strlen(cwd) &&
	    !strncmp(file, cwd, strlen(cwd)))
		return file + strlen(cwd) + 1; /* move past the trailing '/' */
#endif
	return file;
}

static int cleanup_test(struct ztest_unit_test *test)
{
	int ret = TC_PASS;
	int mock_status;

	mock_status = z_cleanup_mock();

#ifdef KERNEL
	/* we need to remove the ztest_thread information from the timeout_q.
	 * Because we reuse the same k_thread structure this would
	 * causes some problems.
	 */
	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		k_thread_abort(&ztest_thread);
	}
#endif

	if (!ret && mock_status == 1) {
		PRINT("Test %s failed: Unused mock parameter values\n",
		      test->name);
		ret = TC_FAIL;
	} else if (!ret && mock_status == 2) {
		PRINT("Test %s failed: Unused mock return values\n",
		      test->name);
		ret = TC_FAIL;
	} else {
		;
	}

	return ret;
}

#ifdef KERNEL
#ifdef CONFIG_SMP
#define NUM_CPUHOLD (CONFIG_MP_NUM_CPUS - 1)
#else
#define NUM_CPUHOLD 0
#endif
#define CPUHOLD_STACK_SZ (512 + CONFIG_TEST_EXTRA_STACK_SIZE)

static struct k_thread cpuhold_threads[NUM_CPUHOLD];
K_KERNEL_STACK_ARRAY_DEFINE(cpuhold_stacks, NUM_CPUHOLD, CPUHOLD_STACK_SZ);
static struct k_sem cpuhold_sem;
volatile int cpuhold_active;

/* "Holds" a CPU for use with the "1cpu" test cases.  Note that we
 * can't use tools like the cpumask feature because we have tests that
 * may need to control that configuration themselves.  We do this at
 * the lowest level, but locking interrupts directly and spinning.
 */
static void cpu_hold(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	unsigned int key = arch_irq_lock();
	uint32_t dt, start_ms = k_uptime_get_32();

	k_sem_give(&cpuhold_sem);

#if defined(CONFIG_ARM64) && defined(CONFIG_FPU_SHARING)
	/*
	 * We'll be spinning with IRQs disabled. The flush-your-FPU request
	 * IPI will never be serviced during that time. Therefore we flush
	 * the FPU preemptively here to prevent any other CPU waiting after
	 * this CPU forever and deadlock the system.
	 */
	extern void z_arm64_flush_local_fpu(void);
	z_arm64_flush_local_fpu();
#endif

	while (cpuhold_active) {
		k_busy_wait(1000);
	}

	/* Holding the CPU via spinning is expensive, and abusing this
	 * for long-running test cases tends to overload the CI system
	 * (qemu runs separate CPUs in different threads, but the CI
	 * logic views it as one "job") and cause other test failures.
	 */
	dt = k_uptime_get_32() - start_ms;
	zassert_true(dt < 3000,
		     "1cpu test took too long (%d ms)", dt);
	arch_irq_unlock(key);
}

void z_impl_z_test_1cpu_start(void)
{
	cpuhold_active = 1;
	char tname[CONFIG_THREAD_MAX_NAME_LEN];

	k_sem_init(&cpuhold_sem, 0, 999);

	/* Spawn N-1 threads to "hold" the other CPUs, waiting for
	 * each to signal us that it's locked and spinning.
	 *
	 * Note that NUM_CPUHOLD can be a value that causes coverity
	 * to flag the following loop as DEADCODE so suppress the warning.
	 */
	/* coverity[DEADCODE] */
	for (int i = 0; i < NUM_CPUHOLD; i++)  {
		k_thread_create(&cpuhold_threads[i],
				cpuhold_stacks[i], CPUHOLD_STACK_SZ,
				(k_thread_entry_t) cpu_hold, NULL, NULL, NULL,
				K_HIGHEST_THREAD_PRIO, 0, K_NO_WAIT);
		if (IS_ENABLED(CONFIG_THREAD_NAME)) {
			snprintk(tname, CONFIG_THREAD_MAX_NAME_LEN, "cpuhold%02d", i);
			k_thread_name_set(&cpuhold_threads[i], tname);
		}
		k_sem_take(&cpuhold_sem, K_FOREVER);
	}
}

void z_impl_z_test_1cpu_stop(void)
{
	cpuhold_active = 0;

	/* Note that NUM_CPUHOLD can be a value that causes coverity
	 * to flag the following loop as DEADCODE so suppress the warning.
	 */
	/* coverity[DEADCODE] */
	for (int i = 0; i < NUM_CPUHOLD; i++)  {
		k_thread_abort(&cpuhold_threads[i]);
	}
}

#ifdef CONFIG_USERSPACE
void z_vrfy_z_test_1cpu_start(void)
{
	z_impl_z_test_1cpu_start();
}
#include <syscalls/z_test_1cpu_start_mrsh.c>

void z_vrfy_z_test_1cpu_stop(void)
{
	z_impl_z_test_1cpu_stop();
}
#include <syscalls/z_test_1cpu_stop_mrsh.c>
#endif /* CONFIG_USERSPACE */
#endif

static void run_test_rules(bool is_before, struct ztest_unit_test *test, void *data)
{
	for (struct ztest_test_rule *rule = _ztest_test_rule_list_start;
	     rule < _ztest_test_rule_list_end; ++rule) {
		if (is_before && rule->before_each) {
			rule->before_each(test, data);
		} else if (!is_before && rule->after_each) {
			rule->after_each(test, data);
		}
	}
}

static void run_test_functions(struct ztest_suite_node *suite, struct ztest_unit_test *test,
			       void *data)
{
	phase = TEST_PHASE_TEST;
	test->test(data);
}

#ifndef KERNEL

/* Static code analysis tool can raise a violation that the standard header
 * <setjmp.h> shall not be used.
 *
 * setjmp is using in a test code, not in a runtime code, it is acceptable.
 * It is a deliberate deviation.
 */
#include <setjmp.h> /* parasoft-suppress MISRAC2012-RULE_21_4-a MISRAC2012-RULE_21_4-b*/
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#define FAIL_FAST 0

static jmp_buf test_fail;
static jmp_buf test_pass;
static jmp_buf stack_fail;

void ztest_test_fail(void)
{
	raise(SIGABRT);
}

void ztest_test_pass(void)
{
	longjmp(test_pass, 1);
}

/**
 * @brief Get a friendly name string for a given test phrase.
 *
 * @param phase an enum ztest_phase value describing the desired test phase
 * @returns a string name for `phase`
 */
static inline const char *get_friendly_phase_name(enum ztest_phase phase)
{
	switch (phase) {
	case TEST_PHASE_SETUP:
		return "setup";
	case TEST_PHASE_BEFORE:
		return "before";
	case TEST_PHASE_TEST:
		return "test";
	case TEST_PHASE_AFTER:
		return "after";
	case TEST_PHASE_TEARDOWN:
		return "teardown";
	case TEST_PHASE_FRAMEWORK:
		return "framework";
	default:
		return "(unknown)";
	}
}

static void handle_signal(int sig)
{
	PRINT("    %s", strsignal(sig));
	switch (phase) {
	case TEST_PHASE_SETUP:
	case TEST_PHASE_BEFORE:
	case TEST_PHASE_TEST:
	case TEST_PHASE_AFTER:
	case TEST_PHASE_TEARDOWN:
		PRINT(" at %s function\n", get_friendly_phase_name(phase));
		longjmp(test_fail, 1);
	case TEST_PHASE_FRAMEWORK:
		PRINT("\n");
		longjmp(stack_fail, 1);
	}
}

static void init_testing(void)
{
	signal(SIGABRT, handle_signal);
	signal(SIGSEGV, handle_signal);

	if (setjmp(stack_fail)) {
		PRINT("Test suite crashed.");
		exit(1);
	}
}

static int run_test(struct ztest_suite_node *suite, struct ztest_unit_test *test, void *data)
{
	int ret = TC_PASS;

	TC_START(test->name);

	if (setjmp(test_fail)) {
		ret = TC_FAIL;
		goto out;
	}

	if (setjmp(test_pass)) {
		ret = TC_PASS;
		goto out;
	}

	run_test_functions(suite, test, data);
out:
	ret |= cleanup_test(test);
	Z_TC_END_RESULT(ret, test->name);

	return ret;
}

#else /* KERNEL */

/* Zephyr's probably going to cause all tests to fail if one test fails, so
 * skip the rest of tests if one of them fails
 */
#ifdef CONFIG_ZTEST_FAIL_FAST
#define FAIL_FAST 1
#else
#define FAIL_FAST 0
#endif

K_THREAD_STACK_DEFINE(ztest_thread_stack, CONFIG_ZTEST_STACK_SIZE + CONFIG_TEST_EXTRA_STACK_SIZE);
static ZTEST_BMEM int test_result;

static void test_finalize(void)
{
	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		k_thread_abort(&ztest_thread);
		k_thread_abort(k_current_get());
	}
}

void ztest_test_fail(void)
{
	test_result = -1;
	test_finalize();
}

void ztest_test_pass(void)
{
	test_result = 0;
	test_finalize();
}

void ztest_test_skip(void)
{
	test_result = -2;
	test_finalize();
}

void ztest_simple_1cpu_before(void *data)
{
	ARG_UNUSED(data);
	z_test_1cpu_start();
}

void ztest_simple_1cpu_after(void *data)
{
	ARG_UNUSED(data);
	z_test_1cpu_stop();
}

static void init_testing(void)
{
	k_object_access_all_grant(&ztest_thread);
}

static void test_cb(void *a, void *b, void *c)
{
	struct ztest_suite_node *suite = a;
	struct ztest_unit_test *test = b;

	test_result = 1;
	if (suite->before) {
		suite->before(/*data=*/c);
	}
	run_test_rules(/*is_before=*/true, test, /*data=*/c);
	run_test_functions(suite, test, c);
	test_result = 0;
}

static int run_test(struct ztest_suite_node *suite, struct ztest_unit_test *test, void *data)
{
	int ret = TC_PASS;

	TC_START(test->name);

	phase = TEST_PHASE_BEFORE;

	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		k_thread_create(&ztest_thread, ztest_thread_stack,
				K_THREAD_STACK_SIZEOF(ztest_thread_stack),
				(k_thread_entry_t)test_cb, suite, test, data,
				CONFIG_ZTEST_THREAD_PRIORITY,
				test->thread_options | K_INHERIT_PERMS, K_FOREVER);

		if (test->name != NULL) {
			k_thread_name_set(&ztest_thread, test->name);
		}
		k_thread_start(&ztest_thread);
		k_thread_join(&ztest_thread, K_FOREVER);
	} else {
		test_result = 1;
		if (suite->before) {
			suite->before(data);
		}
		run_test_rules(/*is_before=*/true, test, data);
		run_test_functions(suite, test, data);
	}

	phase = TEST_PHASE_AFTER;
	if (suite->after != NULL) {
		suite->after(data);
	}
	run_test_rules(/*is_before=*/false, test, data);
	phase = TEST_PHASE_FRAMEWORK;

	/* Flush all logs in case deferred mode and default logging thread are used. */
	while (IS_ENABLED(CONFIG_TEST_LOGGING_FLUSH_AFTER_TEST) &&
	       IS_ENABLED(CONFIG_LOG_PROCESS_THREAD) &&
	       log_data_pending()) {
		k_msleep(100);
	}

	if (test_result == -1) {
		ret = TC_FAIL;
	}

	if (!test_result || !FAIL_FAST) {
		ret |= cleanup_test(test);
	}

	if (test_result == -2) {
		Z_TC_END_RESULT(TC_SKIP, test->name);
	} else {
		Z_TC_END_RESULT(ret, test->name);
	}

	return ret;
}

#endif /* !KERNEL */

static struct ztest_suite_node *ztest_find_test_suite(const char *name)
{
	struct ztest_suite_node *node;

	for (node = _ztest_suite_node_list_start; node < _ztest_suite_node_list_end; ++node) {
		if (strcmp(name, node->name) == 0) {
			return node;
		}
	}

	return NULL;
}

struct ztest_unit_test *ztest_get_next_test(const char *suite, struct ztest_unit_test *prev)
{
	struct ztest_unit_test *test = (prev == NULL) ? _ztest_unit_test_list_start : prev + 1;

	for (; test < _ztest_unit_test_list_end; ++test) {
		if (strcmp(suite, test->test_suite_name) == 0) {
			return test;
		}
	}
	return NULL;
}

static int z_ztest_run_test_suite_ptr(struct ztest_suite_node *suite)
{
	struct ztest_unit_test *test = NULL;
	void *data = NULL;
	int fail = 0;

	if (test_status < 0) {
		return test_status;
	}

	if (suite == NULL) {
		test_status = 1;
		return -1;
	}

	init_testing();

	TC_SUITE_START(suite->name);
	phase = TEST_PHASE_SETUP;
	if (suite->setup != NULL) {
		data = suite->setup();
	}
	while ((test = ztest_get_next_test(suite->name, test)) != NULL) {
		fail += run_test(suite, test, data);

		if (fail && FAIL_FAST) {
			break;
		}
	}
	TC_SUITE_END(suite->name, (fail > 0 ? TC_FAIL : TC_PASS));
	phase = TEST_PHASE_TEARDOWN;
	if (suite->teardown != NULL) {
		suite->teardown(data);
	}

	test_status = (test_status || fail) ? 1 : 0;

	return fail;
}

int z_ztest_run_test_suite(const char *name)
{
	return z_ztest_run_test_suite_ptr(ztest_find_test_suite(name));
}

void end_report(void)
{
	if (test_status) {
		TC_END_REPORT(TC_FAIL);
	} else {
		TC_END_REPORT(TC_PASS);
	}
}

#ifdef CONFIG_USERSPACE
K_APPMEM_PARTITION_DEFINE(ztest_mem_partition);
#endif

int ztest_run_test_suites(const void *state)
{
	struct ztest_suite_node *ptr;
	int count = 0;

	for (ptr = _ztest_suite_node_list_start; ptr < _ztest_suite_node_list_end; ++ptr) {
		struct ztest_suite_stats *stats = &ptr->stats;
		bool should_run = true;

		if (ptr->predicate != NULL) {
			should_run = ptr->predicate(state);
		} else  {
			/* If predicate is NULL, only run this test once. */
			should_run = stats->run_count == 0;
		}

		if (should_run) {
			int fail = z_ztest_run_test_suite_ptr(ptr);

			count++;
			stats->run_count++;
			stats->fail_count += (fail != 0) ? 1 : 0;
		} else {
			stats->skip_count++;
		}
	}

	return count;
}

void ztest_verify_all_test_suites_ran(void)
{
	bool all_tests_run = true;
	struct ztest_suite_node *suite;
	struct ztest_unit_test *test;

	for (suite = _ztest_suite_node_list_start; suite < _ztest_suite_node_list_end; ++suite) {
		if (suite->stats.run_count < 1) {
			PRINT("ERROR: Test suite '%s' did not run.\n", suite->name);
			all_tests_run = false;
		}
	}

	for (test = _ztest_unit_test_list_start; test < _ztest_unit_test_list_end; ++test) {
		suite = ztest_find_test_suite(test->test_suite_name);
		if (suite == NULL) {
			PRINT("ERROR: Test '%s' assigned to test suite '%s' which doesn't exist\n",
			      test->name, test->test_suite_name);
			all_tests_run = false;
		}
	}

	if (!all_tests_run) {
		test_status = 1;
	}
}

void __weak test_main(void)
{
	ztest_run_test_suites(NULL);
	ztest_verify_all_test_suites_ran();
}

#ifndef KERNEL
int main(void)
{
	z_init_mock();
	test_main();
	end_report();

	return test_status;
}
#else
void main(void)
{
#ifdef CONFIG_USERSPACE
	/* Partition containing globals tagged with ZTEST_DMEM and ZTEST_BMEM
	 * macros. Any variables that user code may reference need to be
	 * placed in this partition if no other memory domain configuration
	 * is made.
	 */
	k_mem_domain_add_partition(&k_mem_domain_default,
				   &ztest_mem_partition);
#ifdef Z_MALLOC_PARTITION_EXISTS
	/* Allow access to malloc() memory */
	k_mem_domain_add_partition(&k_mem_domain_default,
				   &z_malloc_partition);
#endif
#endif /* CONFIG_USERSPACE */

	z_init_mock();
	test_main();
	end_report();
	LOG_PANIC();
	if (IS_ENABLED(CONFIG_ZTEST_RETEST_IF_PASSED)) {
		static __noinit struct {
			uint32_t magic;
			uint32_t boots;
		} state;
		const uint32_t magic = 0x152ac523;

		if (state.magic != magic) {
			state.magic = magic;
			state.boots = 0;
		}
		state.boots += 1;
		if (test_status == 0) {
			PRINT("Reset board #%u to test again\n",
				state.boots);
			k_msleep(10);
			sys_reboot(SYS_REBOOT_COLD);
		} else {
			PRINT("Failed after %u attempts\n", state.boots);
			state.boots = 0;
		}
	}
}
#endif