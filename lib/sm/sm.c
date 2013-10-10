/*
 * Copyright (c) 2013 Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <err.h>
#include <trace.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <lib/heap.h>
#include <lib/kmap.h>
#include <lib/sm.h>
#include <lib/sm/sm_err.h>
#include <lk/init.h>
#include <sys/types.h>

void sm_set_mon_stack(void *stack);

extern unsigned long monitor_vector_table;
static trusted_service_handler_routine ts_handler;

extern long sm_platform_boot_args[2];
static void *boot_args;
static int boot_args_refcnt;
static mutex_t boot_args_lock = MUTEX_INITIAL_VALUE(boot_args_lock);
static thread_t *nsthread;

static void sm_wait_for_smcall(void)
{
	long ret = 0;
	ts_args_t *ns_args, args;

	while (true) {
		enter_critical_section();

		thread_yield();
		ns_args = sm_sched_nonsecure(ret);

		if (!ns_args) {
			ret = SM_ERR_UNEXPECTED_RESTART;
			exit_critical_section();
			continue;
		}

		/* Pull args out before enabling interrupts */
		args = *ns_args;
		exit_critical_section();

		/* Dispatch service handler */
		if (ts_handler)
			ret = ts_handler(&args);
		else {
			dprintf(CRITICAL,
				"No service handler registered!\n");
			ret = SM_ERR_NOT_SUPPORTED;
		}
	}
}

/* per-cpu secure monitor initialization */
static void sm_secondary_init(uint level)
{
	const size_t stack_size = 4096;
	void *mon_stack;

	mon_stack = heap_alloc(stack_size, 8);
	if (!mon_stack)
		dprintf(CRITICAL, "failed to allocate monitor mode stack!\n");
	else
		sm_set_mon_stack(mon_stack + stack_size);

	/* let normal world enable SMP, lock TLB, access CP10/11 */
	__asm__ volatile (
		"mrc	p15, 0, r1, c1, c1, 2	\n"
		"orr	r1, r1, #0xC00		\n"
		"orr	r1, r1, #0x60000	\n"
		"mcr	p15, 0, r1, c1, c1, 2	@ NSACR	\n"
		::: "r1"
	);

	__asm__ volatile (
		"mcr	p15, 0, %0, c12, c0, 1	\n"
		: : "r" (&monitor_vector_table)
	);
}

LK_INIT_HOOK_FLAGS(libsm_cpu, sm_secondary_init, LK_INIT_LEVEL_PLATFORM - 2, LK_INIT_FLAG_ALL_CPUS);

static void sm_init(uint level)
{
	status_t err;

	mutex_acquire(&boot_args_lock);

	/* Map the boot arguments if supplied by the bootloader */
	if (sm_platform_boot_args[0] && sm_platform_boot_args[1]) {
		err = kmap_contig(sm_platform_boot_args[0],
				sm_platform_boot_args[1],
				KM_R | KM_NS_MEM,
				PAGE_SIZE_1M,
				(vaddr_t *)&boot_args);

		if (!err) {
			boot_args_refcnt++;
		} else {
			boot_args = NULL;
			TRACEF("Error mapping boot parameter block: %d\n", err);
		}
	}

	mutex_release(&boot_args_lock);

	nsthread = thread_create("ns-switch",
			(thread_start_routine)sm_wait_for_smcall,
			NULL, LOWEST_PRIORITY + 1, DEFAULT_STACK_SIZE);
	if (!nsthread) {
		dprintf(CRITICAL, "failed to create NS switcher thread!\n");
		halt();
	}
}

LK_INIT_HOOK(libsm, sm_init, LK_INIT_LEVEL_PLATFORM - 1);

status_t sm_register_trusted_service_handler(trusted_service_handler_routine fn)
{
	if (ts_handler)
		return ERR_ALREADY_EXISTS;

	enter_critical_section();
	ts_handler = fn;
	exit_critical_section();

	return NO_ERROR;
}

enum handler_return sm_handle_irq(void)
{
	ts_args_t *args;

	args = sm_sched_nonsecure(SM_ERR_INTERRUPTED);
	while(args)
		args = sm_sched_nonsecure(SM_ERR_INTERLEAVED_SMC);

	return INT_NO_RESCHEDULE;
}

status_t sm_get_boot_args(void **boot_argsp, size_t *args_sizep)
{
	status_t err = NO_ERROR;

	if (!boot_argsp || !args_sizep)
		return ERR_INVALID_ARGS;

	mutex_acquire(&boot_args_lock);

	if (!boot_args) {
		err = ERR_NOT_CONFIGURED;
		goto unlock;
	}

	boot_args_refcnt++;
	*boot_argsp = boot_args;
	*args_sizep = sm_platform_boot_args[1];
unlock:
	mutex_release(&boot_args_lock);
	return err;
}

void sm_put_boot_args(void)
{
	mutex_acquire(&boot_args_lock);

	if (!boot_args) {
		TRACEF("WARNING: caller does not own "
			"a reference to boot parameters\n");
		goto unlock;
	}

	boot_args_refcnt--;
	if (boot_args_refcnt == 0) {
		kunmap((vaddr_t)boot_args, sm_platform_boot_args[1]);
		boot_args = NULL;
		thread_resume(nsthread);
	}
unlock:
	mutex_release(&boot_args_lock);
}

static void sm_release_boot_args(uint level)
{
	if (boot_args) {
		sm_put_boot_args();
	} else {
		/* we need to resume the ns-switcher here if
		 * the boot loader didn't pass bootargs
		 */
		thread_resume(nsthread);
	}

	if (boot_args)
		TRACEF("WARNING: outstanding reference to boot args"
				"at the end of initialzation!\n");
}

LK_INIT_HOOK(libsm_bootargs, sm_release_boot_args, LK_INIT_LEVEL_LAST);
