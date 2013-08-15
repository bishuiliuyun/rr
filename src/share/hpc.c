/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "hpc.h"

#include <assert.h>
#include <err.h>
#include <perfmon/pfmlib_perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../share/sys.h"
#include "../share/task.h"

/**
 * libpfm4 specific stuff
 */
void init_libpfm()
{
	int ret = pfm_initialize();
	if (ret != PFM_SUCCESS) {
		errx(1, "cannot initialize libpfm: %s\n", pfm_strerror(ret));
	}
}

void close_libpfm()
{
	pfm_terminate();
}

void libpfm_event_encoding(struct perf_event_attr* attr, const char* event_str, int hw_event)
{
	memset(attr, 0, sizeof(struct perf_event_attr));

	int ret;
	char* fstr;

	attr->size = sizeof(struct perf_event_attr);
	ret = pfm_get_perf_event_encoding(event_str, PFM_PLM0, attr, &fstr, NULL);
	if (ret != PFM_SUCCESS) {
		errx(1, "error while encoding event string %s :%s\n", event_str, pfm_strerror(ret));
	}
	if (hw_event && attr->type != PERF_TYPE_RAW) {
		errx(1, "error: %s is not a raw hardware event\n", event_str);
	}
	sys_free((void**) &fstr);
}


enum cpuid_requests {
  CPUID_GETVENDORSTRING,
  CPUID_GETFEATURES,
  CPUID_GETTLB,
  CPUID_GETSERIAL,

  CPUID_INTELEXTENDED=0x80000000,
  CPUID_INTELFEATURES,
  CPUID_INTELBRANDSTRING,
  CPUID_INTELBRANDSTRINGMORE,
  CPUID_INTELBRANDSTRINGEND,
};

/** issue a single request to CPUID. Fits 'intel features', for instance
 *  note that even if only "eax" and "edx" are of interest, other registers
 *  will be modified by the operation, so we need to tell the compiler about it.
 */
static inline void cpuid(int code, unsigned int *a, unsigned int *d) {
/* this asm returns 1 if CPUID is supported, 0 otherwise (ZF is also set accordingly)
 * add it later for full compatibility

	pushfd ; get
	pop eax
	mov ecx, eax ; save
	xor eax, 0x200000 ; flip
	push eax ; set
	popfd
	pushfd ; and test
	pop eax
	xor eax, ecx ; mask changed bits
	shr eax, 21 ; move bit 21 to bit 0
	and eax, 1 ; and mask others
	push ecx
	popfd ; restore original flags
	ret

 */
  asm volatile("cpuid":"=a"(*a),"=d"(*d):"a"(code):"ecx","ebx");
}

/*
 * Find out the cpu model using the cpuid instruction.
 * full list of CPUIDs at http://sandpile.org/x86/cpuid.htm
 */
typedef enum { UnknownArch = -1, IntelSandyBridge , IntelIvyBridge, IntelNehalem, IntelMerom } cpu_type;
cpu_type get_cpu_type(){
	unsigned int cpu_type,eax,edx;
	cpuid(CPUID_GETFEATURES,&eax,&edx);
	cpu_type = (eax & 0xF0FF0);
	switch (cpu_type) {
	case 0x006F0:
		assert(0 && "Merom not completely supported yet (find deterministic events).");
		return IntelMerom;
		break;
	case 0x106E0:
		return IntelNehalem;
		break;
	case 0x206A0:
	case 0x206D0:
		return IntelSandyBridge;
		break;
	case 0x306A0:
		return IntelIvyBridge;
		break;
	default:
		fprintf(stderr,	"CPU 0x%5X not supported yet (add cpuid and adjust the event string to add support).",
			cpu_type);
		exit(0);
		break; /* not reached */
	}
	return UnknownArch;
}

/**
 * initialize hpc here
 */
void init_hpc(struct task* t)
{

	struct hpc_context* counters = sys_malloc_zero(sizeof(struct hpc_context));
	t->hpc = counters;

	/* get the event that counts down to the initial value
	 * the precision level enables PEBS support. precise=0 uses the counter
	 * with PEBS disabled */
	const char * rbc_event = 0;
	const char * inst_event = 0;
	const char * hw_int_event = 0;
	const char * page_faults_event = "PERF_COUNT_SW_PAGE_FAULTS:u";
	switch (get_cpu_type()) {
	case IntelMerom :
		rbc_event = "BR_INST_RETIRED:u";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INT_RCV:u";
		break;
	case IntelNehalem :
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		break;
	case IntelSandyBridge :
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	case IntelIvyBridge :
		rbc_event = "BR_INST_RETIRED:COND:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	default: // best guess
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	}

	libpfm_event_encoding(&(counters->inst.attr), inst_event , 1);
	libpfm_event_encoding(&(counters->rbc.attr), rbc_event , 1);
	/* counts up to double check */
	//libpfm_event_encoding(&(counters->rbc.attr), rbc_event, 1);
	libpfm_event_encoding(&(counters->hw_int.attr), hw_int_event, 1);
	//libpfm_event_encoding(&(counters->hw_int.attr), event_str, 1);
	libpfm_event_encoding(&(counters->page_faults.attr), page_faults_event, 0);
}

static void __start_hpc(struct task* t)
{
	struct hpc_context *counters = t->hpc;
	pid_t tid = t->tid;
	// see http://www.eece.maine.edu/~vweaver/projects/perf_events/perf_event_open.html for more information
	START_COUNTER(tid,-1,counters->hw_int); // group leader.
	START_COUNTER(tid,counters->hw_int.fd,counters->inst);
	START_COUNTER(tid,counters->hw_int.fd,counters->rbc);
	START_COUNTER(tid,counters->hw_int.fd,counters->page_faults);
	//START_COUNTER(tid,counters->hw_int.fd,counters->rbc);

	sys_fcntl_f_setown(counters->rbc.fd, tid);
	sys_fcntl_f_setfl_o_async(counters->rbc.fd);

	counters->started = 1;
}

void stop_rbc(struct task* t)
{
	STOP_COUNTER(t->hpc->rbc.fd);
}

void stop_hpc(struct task* t)
{
	struct hpc_context* counters = t->hpc;

	STOP_COUNTER(counters->hw_int.fd);
	STOP_COUNTER(counters->inst.fd);
	STOP_COUNTER(counters->page_faults.fd);
	STOP_COUNTER(counters->rbc.fd);
}

void cleanup_hpc(struct task* t)
{
	struct hpc_context* counters = t->hpc;

	stop_hpc(t);

	sys_close(counters->hw_int.fd);
	sys_close(counters->inst.fd);
	sys_close(counters->page_faults.fd);
	sys_close(counters->rbc.fd);
	counters->started = 0;
}

/*
 * Starts the hpc.
 * @param t: the current execution context
 * @param reset: the counters are (if enabled) reset
 */
void start_hpc(struct task *t, uint64_t val)
{
	t->hpc->rbc.attr.sample_period = val;
	__start_hpc(t);
}

void reset_hpc(struct task *t, uint64_t val)
{
	if (t->hpc->started) {
		cleanup_hpc(t);
	}
	t->hpc->rbc.attr.sample_period = val;
	__start_hpc(t);
}
/**
 * Ultimately frees all resources that are used by hpc of the corresponding
 * t. After calling this function, counters cannot be used anymore
 */
void destry_hpc(struct task *t)
{
	struct hpc_context* counters = t->hpc;
	sys_free((void**) &counters);
}

/**
 * Counter access functions
 */
uint64_t read_hw_int(struct hpc_context *counters)
{
	uint64_t tmp;
	READ_COUNTER(counters->hw_int.fd, &tmp, sizeof(uint64_t));
	return tmp;
}

uint64_t read_insts(struct hpc_context *counters)
{
	uint64_t tmp;
	READ_COUNTER(counters->inst.fd, &tmp, sizeof(uint64_t));
	return tmp;
}


uint64_t read_page_faults(struct hpc_context *counters)
{
	uint64_t tmp;
	READ_COUNTER(counters->page_faults.fd, &tmp, sizeof(uint64_t));
	return tmp;
}

uint64_t read_rbc(struct hpc_context *counters)
{
	uint64_t tmp;
	READ_COUNTER(counters->rbc.fd, &tmp, sizeof(uint64_t));
	return tmp;
}
