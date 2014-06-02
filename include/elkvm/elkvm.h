#pragma once

#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <libelf.h>

typedef uint64_t guestptr_t;

#include "kvm.h"
#include "pager.h"
#include "region.h"
#include "vcpu.h"
#include "list.h"
#include "elkvm-signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_MODE_X86    1
#define VM_MODE_PAGING 2
#define VM_MODE_X86_64 3

#define ELKVM_USER_CHUNK_OFFSET 1024*1024*1024

#ifdef _PREFIX_
#define RES_PATH _PREFIX_ "/share/libelkvm"
#endif

struct region_mapping {
  void *host_p;
  uint64_t guest_virt;
  size_t length;
  unsigned mapped_pages;
};

struct kvm_vm {
	int fd;
	struct vcpu_list *vcpus;
	struct kvm_pager pager;
	int run_struct_size;
  list(struct elkvm_memory_region *, root_region);
	const struct elkvm_handlers *syscall_handlers;
  list(struct region_mapping *, mappings);

	struct elkvm_memory_region *text;
  list(struct elkvm_memory_region *, heap);
	struct elkvm_memory_region *kernel_stack;
	struct elkvm_memory_region *gdt_region;
	struct elkvm_memory_region *idt_region;
  struct elkvm_memory_region *current_user_stack;
  struct elkvm_memory_region *env_region;

  struct elkvm_signals sigs;
  struct elkvm_flat *sighandler_cleanup;
  struct rlimit rlimits[RLIMIT_NLIMITS];

  int debug;
};

struct elkvm_handlers {
	long (*read) (int fd, void *buf, size_t count);
	long (*write) (int fd, void *buf, size_t count);
	long (*open) (const char *pathname, int flags, mode_t mode);
	long (*close) (int fd);
	long (*stat) (const char *path, struct stat *buf);
	long (*fstat) (int fd, struct stat *buf);
	long (*lstat) (const char *path, struct stat *buf);
	long (*poll) (struct pollfd *fds, nfds_t nfds, int timeout);
	long (*lseek) (int fd, off_t offset, int whence);
	long (*mmap) (void *addr, size_t length, int prot, int flags, int fd,
      off_t offset, struct region_mapping *);
	long (*mprotect) (void *addr, size_t len, int prot);
	long (*munmap) (struct region_mapping *mapping);
  /* ... */
  long (*sigaction) (int signum, const struct sigaction *act,
      struct sigaction *oldact);
  long (*sigprocmask)(int how, const sigset_t *set, sigset_t *oldset);
  /* ... */
  long (*readv) (int fd, struct iovec *iov, int iovcnt);
  long (*writev) (int fd, struct iovec *iov, int iovcnt);
  long (*access) (const char *pathname, int mode);
  long (*pipe) (int pipefd[2]);
  long (*dup) (int oldfd);
  /* ... */
  long (*nanosleep)(const struct timespec *req, struct timespec *rem);
  long (*getpid)(void);
  /* ... */
  long (*getuid)(void);
  long (*getgid)(void);
  /* ... */
  long (*geteuid)(void);
  long (*getegid)(void);
	/* ... */
	long (*uname) (struct utsname *buf);
  long (*fcntl) (int fd, int cmd, ...);
  long (*truncate) (const char *path, off_t length);
  long (*ftruncate) (int fd, off_t length);
  char *(*getcwd) (char *buf, size_t size);
  long (*mkdir) (const char *pathname, mode_t mode);
  long (*unlink) (const char *pathname);
  long (*readlink) (const char *path, char *buf, size_t bufsiz);
  /* ... */
  long (*gettimeofday) (struct timeval *tv, struct timezone *tz);
  long (*getrusage) (int who, struct rusage *usage);
  /* ... */
  long (*times) (struct tms *buf);
  /* ... */
  long (*gettid)(void);
  /* ... */
  long (*time) (time_t *t);
  long (*futex)(int *uaddr, int op, int val, const struct timespec *timeout,
      int *uaddr2, int val3);
  /* ... */
  long (*clock_gettime) (clockid_t clk_id, struct timespec *tp);
  void (*exit_group) (int status);
  long (*tgkill)(int tgid, int tid, int sig);

  /* ELKVM debug callbacks */

  /*
   * called after a breakpoint has been hit, should return 1 to abort the program
   * 0 otherwise, if this is set to NULL elkvm will execute a simple debug shell
   */
  int (*bp_callback)(struct kvm_vm *vm);

};

/*
	Create a new VM, with the given mode, cpu count, memory and syscall handlers
	Return 0 on success, -1 on error
*/
int elkvm_vm_create(struct elkvm_opts *, struct kvm_vm *, int, int, int,
		const struct elkvm_handlers *, const char *binary);

/*
 * \brief Put the VM in debug mode
 */
int elkvm_set_debug(struct kvm_vm *);

/*
 * Setup the addresses of the system regions
 */
int elkvm_region_setup(struct kvm_vm *vm);

/*
	Returns the number of VCPUs in a VM
*/
int elkvm_vcpu_count(struct kvm_vm *);

/*
 * \brief Emulates (skips) the VMCALL instruction
 */
int elkvm_emulate_vmcall(struct kvm_vm *, struct kvm_vcpu *);

/*
 * \brief Deletes (frees) the chunk with number num and hands a new chunk
 *        with the newsize to a vm at the same memory slot.
 *        THIS WILL DELETE ALL DATA IN THE OLD CHUNK!
 */
int elkvm_chunk_remap(struct kvm_vm *, int num, uint64_t newsize);

struct kvm_vcpu *elkvm_vcpu_get(struct kvm_vm *, int vcpu_id);
int elkvm_chunk_count(struct kvm_vm *);

struct kvm_userspace_memory_region elkvm_get_chunk(struct kvm_vm *, int chunk);

int elkvm_dump_valid_msrs(struct elkvm_opts *);

/*
 * Print the locations of the system memory regions
 */
void elkvm_print_regions(struct kvm_vm *);
void elkvm_dump_region(struct elkvm_memory_region *);

/**
 * \brief Initialize the gdbstub and wait for gdb
 *        to connect
 */
void elkvm_gdbstub_init(struct kvm_vm *vm);

/**
 * \brief Enable VCPU debug mode
 */
int elkvm_debug_enable(struct kvm_vcpu *vcpu);

#ifdef __cplusplus
}
#endif