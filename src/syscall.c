#include <errno.h>
#include <string.h>
#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <elkvm.h>
#include <heap.h>
#include <mapping.h>
#include <stack.h>
#include <syscall.h>
#include <vcpu.h>

int elkvm_handle_hypercall(struct kvm_vm *vm, struct kvm_vcpu *vcpu) {
  int err = 0;

  uint64_t call = kvm_vcpu_get_hypercall_type(vm, vcpu);
  switch(call) {
    case ELKVM_HYPERCALL_SYSCALL:
			err = elkvm_handle_syscall(vm, vcpu);
      break;
    case ELKVM_HYPERCALL_INTERRUPT:
      err = elkvm_handle_interrupt(vm, vcpu);
      if(err) {
        return err;
      }
      break;
    default:
      fprintf(stderr,
          "Hypercall was something else, don't know how to handle, ABORT!\n");
      return 1;
  }

	if(err) {
		return err;
	}

  err = elkvm_emulate_vmcall(vm, vcpu);
  if(err) {
    return err;
  }

  return 0;
}

int elkvm_handle_interrupt(struct kvm_vm *vm, struct kvm_vcpu *vcpu) {
  uint64_t interrupt_vector = elkvm_popq(vm, vcpu);

  if(vm->debug) {
    printf(" INTERRUPT with vector 0x%lx detected\n", interrupt_vector);
    kvm_vcpu_dump_regs(vcpu);
    elkvm_dump_stack(vm, vcpu);
  }

  /* Stack Segment */
  if(interrupt_vector == 0x0c) {
    uint64_t err_code = elkvm_popq(vm, vcpu);
    printf("STACK SEGMENT FAULT\n");
    printf("Error Code: %lu\n", err_code);
    return 1;
  }

  /* General Protection */
  if(interrupt_vector == 0x0d) {
    uint64_t err_code = elkvm_popq(vm, vcpu);
    printf("GENERAL PROTECTION FAULT\n");
    printf("Error Code: %lu\n", err_code);
    return 1;

  }

  /* page fault */
	if(interrupt_vector == 0x0e) {
    int err = kvm_vcpu_get_sregs(vcpu);
    if(err) {
      return err;
    }

    if(vcpu->sregs.cr2 == 0x0) {
      printf("\n\nABORT: SEGMENTATION FAULT\n\n");
      exit(1);
      return 1;
    }

    uint32_t err_code = elkvm_popq(vm, vcpu);
    err = kvm_pager_handle_pagefault(&vm->pager, vcpu->sregs.cr2, err_code);

		return err;
	}

	return 1;
}

/* Taken from uClibc/libc/sysdeps/linux/x86_64/bits/syscalls.h
   The Linux/x86-64 kernel expects the system call parameters in
   registers according to the following table:

   syscall number  rax
   arg 1   rdi
   arg 2   rsi
   arg 3   rdx
   arg 4   r10
   arg 5   r8
   arg 6   r9

   The Linux kernel uses and destroys internally these registers:
   return address from
   syscall   rcx
   additionally clobered: r12-r15,rbx,rbp
   eflags from syscall r11

   Normal function call, including calls to the system call stub
   functions in the libc, get the first six parameters passed in
   registers and the seventh parameter and later on the stack.  The
   register use is as follows:

    system call number in the DO_CALL macro
    arg 1    rdi
    arg 2    rsi
    arg 3    rdx
    arg 4    rcx
    arg 5    r8
    arg 6    r9

   We have to take care that the stack is aligned to 16 bytes.  When
   called the stack is not aligned since the return address has just
   been pushed.


   Syscalls of more than 6 arguments are not supported.  */

int elkvm_handle_syscall(struct kvm_vm *vm, struct kvm_vcpu *vcpu) {
	uint64_t syscall_num = vcpu->regs.rax;
  if(vm->debug) {
    fprintf(stderr, " SYSCALL %3lu detected\n", syscall_num);
  }

	long result;
	if(syscall_num > NUM_SYSCALLS) {
    fprintf(stderr, "\tINVALID syscall_num: %lu\n", syscall_num);
		result = -ENOSYS;
	} else {
    if(vm->debug) {
      fprintf(stderr, "(%s)\n", elkvm_syscalls[syscall_num].name);
    }
		result = elkvm_syscalls[syscall_num].func(vm);
    if(syscall_num == __NR_exit_group) {
      return ELKVM_HYPERCALL_EXIT;
    }
	}
	/* binary expects syscall result in rax */
	vcpu->regs.rax = result;

	return 0;
}

int elkvm_syscall1(struct kvm_vm *vm, struct kvm_vcpu *vcpu, uint64_t *arg) {
	*arg = vcpu->regs.rdi;
	return 0;
}

int elkvm_syscall2(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		uint64_t *arg1, uint64_t *arg2) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	return 0;
}

int elkvm_syscall3(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
	return 0;
}

int elkvm_syscall4(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
	return 0;
}

int elkvm_syscall5(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4,
    uint64_t *arg5) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
  *arg5 = vcpu->regs.r8;
	return 0;
}

int elkvm_syscall6(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		uint64_t *arg1, uint64_t *arg2, uint64_t *arg3, uint64_t *arg4,
    uint64_t *arg5, uint64_t *arg6) {
	*arg1 = vcpu->regs.rdi;
	*arg2 = vcpu->regs.rsi;
	*arg3 = vcpu->regs.rdx;
  *arg4 = vcpu->regs.r10;
  *arg5 = vcpu->regs.r8;
  *arg6 = vcpu->regs.r9;
	return 0;
}

long elkvm_do_read(struct kvm_vm *vm) {
	if(vm->syscall_handlers->read == NULL) {
		printf("READ handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	uint64_t fd;
	uint64_t buf_p;
	char *buf;
	uint64_t count;

	int err = elkvm_syscall3(vm, vcpu, &fd, &buf_p, &count);
	if(err) {
		return -EIO;
	}

	buf = kvm_pager_get_host_p(&vm->pager, buf_p);
  if(vm->debug) {
    printf("READ from fd: %i to %p with %zd bytes\n", (int)fd, buf, (size_t)count);
  }

	long result = vm->syscall_handlers->read((int)fd, buf, (size_t)count);
  if(vm->debug) {
    printf("RESULT (%li): %.*s\n", result, (int)result, buf);
  }

	return result;
}

long elkvm_do_write(struct kvm_vm *vm) {
  if(vm->syscall_handlers->write == NULL) {
    printf("WRITE handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

  uint64_t fd = 0x0;
  uint64_t buf_p = 0x0;
  void *buf;
  uint64_t count = 0x0;

  int err = elkvm_syscall3(vm, vcpu, &fd, &buf_p, &count);
  if(err) {
    return -EIO;
  }

  buf = kvm_pager_get_host_p(&vm->pager, buf_p);
  if(vm->debug) {
    printf("WRITE to fd: %i from %p (guest: 0x%lx) with %zd bytes\n",
      (int)fd, buf, buf_p, (size_t)count);
    printf("\tDATA: %.*s\n", (int)count, (char *)buf);
  }

  long result = vm->syscall_handlers->write((int)fd, buf, (size_t)count);
  if(vm->debug) {
    printf("RESULT: %li\n", result);
  }

  return result;
}

long elkvm_do_open(struct kvm_vm *vm) {
	if(vm->syscall_handlers->open == NULL) {
		printf("OPEN handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	uint64_t pathname_p = 0x0;
	char *pathname = NULL;
	uint64_t flags = 0x0;
	uint64_t mode = 0x0;

	int err = elkvm_syscall3(vm, vcpu, &pathname_p, &flags, &mode);
	if(err) {
		return -EIO;
	}
	pathname = kvm_pager_get_host_p(&vm->pager, pathname_p);

  if(vm->debug) {
  }
	long result = vm->syscall_handlers->open(pathname, (int)flags, (mode_t)mode);

  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("OPEN file %s with flags %i and mode %x\n", pathname,
			(int)flags, (mode_t)mode);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

	return result;
}

long elkvm_do_close(struct kvm_vm *vm) {
	if(vm->syscall_handlers->close == NULL) {
		printf("CLOSE handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	uint64_t fd = 0;
	int err = elkvm_syscall1(vm, vcpu, &fd);
	if(err) {
		return -EIO;
	}

  if(vm->debug) {
    printf("CLOSE file with fd: %li\n", fd);
  }
	long result = vm->syscall_handlers->close((int)fd);

  if(vm->debug) {
    printf("RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_stat(struct kvm_vm *vm) {
  if(vm->syscall_handlers->stat == NULL) {
    printf("STAT handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  uint64_t path_p = 0;
  uint64_t buf_p = 0;
  char *path = NULL;
  struct stat *buf;
  int err = elkvm_syscall2(vm, vcpu, &path_p, &buf_p);
  if(err) {
    return -EIO;
  }
  path = kvm_pager_get_host_p(&vm->pager, path_p);
  buf  = kvm_pager_get_host_p(&vm->pager, buf_p);

  long result = vm->syscall_handlers->stat(path, buf);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("STAT file %s with buf at: 0x%lx (%p)\n",
        path, buf_p, buf);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_fstat(struct kvm_vm *vm) {
  if(vm->syscall_handlers->fstat == NULL) {
    printf("FSTAT handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

  uint64_t fd = 0;
  uint64_t buf_p = 0;
  struct stat *buf = NULL;
  int err = elkvm_syscall2(vm, vcpu, &fd, &buf_p);
  if(err) {
    return -EIO;
  }
	buf = kvm_pager_get_host_p(&vm->pager, buf_p);

  if(vm->debug) {
    printf("FSTAT file with fd %li buf at 0x%lx (%p)\n", fd, buf_p, buf);
  }
  long result = vm->syscall_handlers->fstat(fd, buf);

  if(vm->debug) {
    printf("RESULT: %li\n", result);
  }

  return result;
}

long elkvm_do_lstat(struct kvm_vm *vm) {
	return -ENOSYS;
}

long elkvm_do_poll(struct kvm_vm *vm) {
	return -ENOSYS;
}

long elkvm_do_lseek(struct kvm_vm *vm) {
  if(vm->syscall_handlers->lseek == NULL) {
    printf("LSEEK handler not found\n");
    return -ENOSYS;
  }

  uint64_t fd;
  uint64_t off;
  uint64_t whence;
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  int err = elkvm_syscall3(vm, vcpu, &fd, &off, &whence);
  if(err) {
    return -EFAULT;
  }

  long result = vm->syscall_handlers->lseek(fd, off, whence);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("LSEEK fd %lu offset %lu whence %lu\n",
        fd, off, whence);
    printf("RESULT: %li\n", result);
    printf("=================================\n");

  }
  return result;


}

long elkvm_do_mmap(struct kvm_vm *vm) {
  if(vm->syscall_handlers->mmap == NULL) {
    printf("MMAP handler not found\n");
    return -ENOSYS;
  }

  uint64_t addr_p = 0;
  void *addr = NULL;
  uint64_t length = 0;
  uint64_t prot = 0;
  uint64_t flags = 0;
  uint64_t fd = 0;
  uint64_t offset = 0;
  int err = elkvm_syscall6(vm, vm->vcpus->vcpu, &addr_p, &length, &prot, &flags,
      &fd, &offset);
  if(err) {
    return -EIO;
  }
  addr = kvm_pager_get_host_p(&vm->pager, addr_p);

  struct region_mapping *mapping = elkvm_mapping_alloc();
  long result = vm->syscall_handlers->mmap((void *)addr_p, length, prot,
      flags, fd, offset, mapping);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MMAP addr_p %p length %lu prot %lu flags %lu fd %lu offset %lu\n",
        addr, length, prot, flags, fd, offset);
    printf("RESULT: %li\n", result);
    if(length % 0x1000) {
      mapping->mapped_pages = length / 0x1000 + 1;
    } else {
      mapping->mapped_pages = length / 0x1000;
    }
    if(result >= 0) {
      printf("MAPPING: %p host_p: %p guest_virt: 0x%lx length %zd mapped pages %i\n",
          mapping, mapping->host_p, mapping->guest_virt, mapping->length, mapping->mapped_pages);
    }
    printf("=================================\n");
  }
  if(result < 0) {
    return -errno;
  }

  struct kvm_userspace_memory_region *chunk =
    kvm_pager_alloc_chunk(&vm->pager, mapping->host_p, length, 0);
  if(chunk == NULL) {
    return -ENOMEM;
  }
  err = kvm_vm_map_chunk(vm, chunk);
  if(err) {
    printf("ERROR mapping chunk %p\n", chunk);
    return err;
  }

  void *host_current_p = mapping->host_p;
  uint64_t guest_addr = mapping->guest_virt;
  assert(guest_addr != 0);

  if(length % 0x1000) {
    mapping->mapped_pages = length / 0x1000 + 1;
  } else {
    mapping->mapped_pages = length / 0x1000;
  }
  for(int page = 0; page < mapping->mapped_pages; page++) {
    err = kvm_pager_create_mapping(&vm->pager, host_current_p, guest_addr,
        flags & PROT_WRITE,
        flags & PROT_EXEC);
    if(err) {
      printf("ERROR CREATING PT entries\n");
      return err;
    }
//    void *addr = kvm_pager_get_host_p(&vm->pager, guest_addr);
//    assert((uint64_t)addr == guest_addr);
    host_current_p+=0x1000;
    guest_addr+=0x1000;
  }

  list_push(vm->mappings, mapping);
  return (long)mapping->guest_virt;
}

long elkvm_do_mprotect(struct kvm_vm *vm) {
	return -ENOSYS;
}

long elkvm_do_munmap(struct kvm_vm *vm) {
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  uint64_t addr_p = 0;
  void *addr = NULL;
  uint64_t length = 0;
  int err = elkvm_syscall2(vm, vcpu, &addr_p, &length);
  if(err) {
    return err;
  }

  addr = kvm_pager_get_host_p(&vm->pager, addr_p);

  struct kvm_userspace_memory_region *region =
    kvm_pager_find_region_for_host_p(&vm->pager, addr);
  assert(region != &vm->pager.system_chunk);
  assert(region != NULL);

  struct region_mapping *mapping = elkvm_mapping_find(vm, addr);

  for(uint64_t guest_addr = addr_p;
      guest_addr < addr_p + length;
      guest_addr += 0x1000) {
    err = kvm_pager_destroy_mapping(&vm->pager, guest_addr);
    mapping->mapped_pages--;
    assert(err == 0);
  }

  long result = -1;
  if(mapping->mapped_pages == 0) {
    region->memory_size = 0;
    err = kvm_vm_map_chunk(vm, region);
    if(vm->syscall_handlers->munmap != NULL) {
      result = vm->syscall_handlers->munmap(mapping);
    } else {
      printf("MUNMAP handler not found!\n");
      result = -ENOSYS;
    }
  }

  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("MUNMAP reguested with address: 0x%lx (%p) length: 0x%lx\n",
        addr_p, addr, length, mapping->mapped_pages);
    printf("MAPPING %p pages mapped: %u\n", mapping, mapping->mapped_pages);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }

  return 0;

//  if(result < 0) {
//    return result;
//  }
//
//
//  return err;
}

long elkvm_do_brk(struct kvm_vm *vm) {
  uint64_t user_brk_req = 0;
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;
  int err = elkvm_syscall1(vm, vcpu, &user_brk_req);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("BRK reguested with address: 0x%lx current brk address: 0x%lx\n",
        user_brk_req, vm->pager.brk_addr);
  }

  if(err) {
    return -EIO;
  }

  /* if the requested brk address is 0 just return the current brk address */
  if(user_brk_req == 0) {
    return vm->pager.brk_addr;
  }

  /*
   * if the requested brk address is smaller than the current brk,
   * adjust the new brk, free mapped pages
   * TODO mark used regions as free, merge regions
   */
  if(user_brk_req < vm->pager.brk_addr) {
    for(uint64_t guest_addr = vm->pager.brk_addr;
        guest_addr >= next_page(user_brk_req);
        guest_addr -= 0x1000) {
      err = kvm_pager_destroy_mapping(&vm->pager, guest_addr);
      assert(err == 0);
    }

    vm->pager.brk_addr = user_brk_req;
    return user_brk_req;
  }

  /* if the requested brk address is still within the current data region,
   * just push the brk */
  err = elkvm_brk(vm, user_brk_req);
  if(vm->debug) {
    printf("BRK done: err: %i (%s) newbrk: 0x%lx\n",
        err, strerror(err), vm->pager.brk_addr);
    printf("=================================\n");
  }
  if(err) {
    return err;
  }

  return vm->pager.brk_addr;
}

long elkvm_do_sigaction(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sigprocmask(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sigreturn(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_ioctl(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_pread64(struct kvm_vm *vm) {
  return -ENOSYS;
}

void elkvm_get_host_iov(struct kvm_vm *vm, uint64_t iov_p, uint64_t iovcnt,
    struct iovec *host_iov) {
  struct iovec *guest_iov = NULL;
  guest_iov = kvm_pager_get_host_p(&vm->pager, iov_p);

  for(int i = 0; i < iovcnt; i++) {
    host_iov[i].iov_base = kvm_pager_get_host_p(&vm->pager,
        (uint64_t)guest_iov[i].iov_base);
    host_iov[i].iov_len  = guest_iov[i].iov_len;
  }

}

long elkvm_do_readv(struct kvm_vm *vm) {
  if(vm->syscall_handlers->readv == NULL) {
    printf("READV handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  uint64_t fd = 0;
  uint64_t iov_p = 0;
  uint64_t iovcnt = 0;

  int err = elkvm_syscall3(vm, vcpu, &fd, &iov_p, &iovcnt);
  if(err) {
    return err;
  }

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vm, iov_p, iovcnt, host_iov);

  long result = vm->syscall_handlers->readv(fd, host_iov, iovcnt);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("READV with fd: %i (%p) iov: 0x%lx iovcnt: %i\n",
        (int)fd, &fd, iov_p, (int)iovcnt);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_writev(struct kvm_vm *vm) {
  if(vm->syscall_handlers->writev == NULL) {
    printf("WRITEV handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  uint64_t fd = 0;
  uint64_t iov_p = 0;
  struct iovec *guest_iov = NULL;
  uint64_t iovcnt = 0;

  int err = elkvm_syscall3(vm, vcpu, &fd, &iov_p, &iovcnt);
  if(err) {
    return err;
  }

  struct iovec host_iov[iovcnt];
  elkvm_get_host_iov(vm, iov_p, iovcnt, host_iov);

  long result = vm->syscall_handlers->writev(fd, host_iov, iovcnt);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("WRITEV with fd: %i iov: 0x%lx iovcnt: %i\n",
        (int)fd, iov_p, (int)iovcnt);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_access(struct kvm_vm *vm) {
	if(vm->syscall_handlers->access == NULL) {
    printf("ACCESS handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

  uint64_t path_p;
  uint64_t mode;

  int err = elkvm_syscall2(vm, vcpu, &path_p, &mode);
  if(err) {
    return err;
  }

  char *pathname = kvm_pager_get_host_p(&vm->pager, path_p);
  if(pathname == NULL) {
    return EFAULT;
  }
  if(vm->debug) {
    printf("CALLING ACCESS handler with pathname %s and mode %i\n",
      pathname, (int)mode);
  }

  long result = vm->syscall_handlers->access(pathname, mode);
  if(vm->debug) {
    printf("ACCESS result: %li\n", result);
  }

  return -errno;
}

long elkvm_do_pipe(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_select(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sched_yield(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_mremap(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_msync(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_mincore(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_madvise(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_shmget(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_shmat(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_shmctl(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_dup(struct kvm_vm *vm) {
	if(vm->syscall_handlers->dup == NULL) {
    printf("DUP handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

  uint64_t oldfd;

  int err = elkvm_syscall1(vm, vcpu, &oldfd);
  if(err) {
    return err;
  }

  if(vm->debug) {
    printf("CALLING DUP handler with oldfd %i\n",
      (int)oldfd);
  }

  long result = vm->syscall_handlers->dup(oldfd);
  if(vm->debug) {
    printf("DUP result: %li\n", result);
  }

  return -errno;
}

long elkvm_do_dup2(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_pause(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_nanosleep(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getitimer(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_alarm(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setitimer(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getpid(struct kvm_vm *vm) {
  if(vm->syscall_handlers->getpid == NULL) {
    return -ENOSYS;
  }

  long pid = vm->syscall_handlers->getpid();
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETPID\n");
    printf("RESULT: %li\n", pid);
    printf("=================================\n");
  }

  return pid;
}

long elkvm_do_sendfile(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_socket(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_connect(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_accept(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sendto(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_recvfrom(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sendmsg(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_recvmsg(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_shutdown(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_bind(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_listen(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getsockname(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getpeername(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_socketpair(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setsockopt(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getsockopt(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_clone(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fork(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_vfork(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_execve(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_exit(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_wait4(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_kill(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_uname(struct kvm_vm *vm) {
	if(vm->syscall_handlers->uname == NULL) {
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	struct utsname *buf = NULL;
	uint64_t bufp = 0;
	int err = elkvm_syscall1(vm, vcpu, &bufp);
	if(err) {
		return -EIO;
	}
	buf = (struct utsname *)kvm_pager_get_host_p(&vm->pager, bufp);
  if(vm->debug) {
    printf("CALLING UNAME handler with buf pointing to: %p (0x%lx)\n", buf,
			host_to_guest_physical(&vm->pager, buf));
  }
	if(buf == NULL) {
		return -EIO;
	}

	long result = vm->syscall_handlers->uname(buf);
	result = 1;
  if(vm->debug) {
    printf("UNAME result: %li\n", result);
    printf("\tsyname: %s nodename: %s release: %s version: %s machine: %s\n",
			buf->sysname, buf->nodename, buf->release, buf->version, buf->machine);
  }
	return result;
}

long elkvm_do_semget(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_semop(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_semctl(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_shmdt(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_msgget(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_msgsnd(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_msgrcv(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_msgctl(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fcntl(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_flock(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fsync(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fdatasync(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_truncate(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_ftruncate(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getdents(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getcwd(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_chdir(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fchdir(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rename(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_mkdir(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rmdir(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_creat(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_link(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_unlink(struct kvm_vm *vm) {
  if(vm->syscall_handlers->unlink == NULL) {
    printf("UNLINK handler not found\n");
    return -ENOSYS;
  }
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);

  uint64_t pathname_p = 0;
  char *pathname = NULL;

  int err = elkvm_syscall1(vm, vcpu, &pathname_p);
  if(err) {
    return err;
  }

  pathname = kvm_pager_get_host_p(&vm->pager, pathname_p);
  long result = vm->syscall_handlers->unlink(pathname);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("UNLINK with pathname at: %p (%s)\n",
        pathname, pathname);
    printf("RESULT: %li\n", result);
    if(result < 0) {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }
  return result;
}

long elkvm_do_symlink(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_readlink(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_chmod(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fchmod(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_chown(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_fchown(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_lchown(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_umask(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_gettimeofday(struct kvm_vm *vm) {
  if(vm->syscall_handlers->gettimeofday == NULL) {
    return -ENOSYS;
  }

  uint64_t tv_p = 0;
  uint64_t tz_p = 0;
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);
  int err = elkvm_syscall2(vm, vcpu, &tv_p, &tz_p);
  if(err) {
    return err;
  }

  struct timeval  *tv = kvm_pager_get_host_p(&vm->pager, tv_p);
  struct timezone *tz = kvm_pager_get_host_p(&vm->pager, tz_p);

  long result = vm->syscall_handlers->gettimeofday(tv, tz);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("GETTIMEOFDAY with timeval: %lx (%p) timezone: %lx (%p)\n",
        tv_p, tv, tz_p, tz);
    printf("RESULT: %li\n", result);
    if(result == 0) {
      printf("timeval: tv_sec: %lu tv_usec: %lu\n", tv->tv_sec, tv->tv_usec);
      printf("timezone: tz_minuteswest: %i tz_dsttime %i\n",
          tz->tz_minuteswest, tz->tz_dsttime);
    } else {
      printf("ERROR No: %i Msg: %s\n", errno, strerror(errno));
    }
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_getrlimit(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getrusage(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_sysinfo(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_times(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_ptrace(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getuid(struct kvm_vm *vm) {
	if(vm->syscall_handlers->getuid == NULL) {
		printf("GETUID handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	long result = vm->syscall_handlers->getuid();
  if(vm->debug) {
    printf("GETUID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_syslog(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getgid(struct kvm_vm *vm) {
	if(vm->syscall_handlers->getgid == NULL) {
		printf("GETGID handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	long result = vm->syscall_handlers->getgid();
  if(vm->debug) {
    printf("GETGID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_setuid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_geteuid(struct kvm_vm *vm) {
	if(vm->syscall_handlers->geteuid == NULL) {
		printf("GETEUID handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	long result = vm->syscall_handlers->geteuid();
  if(vm->debug) {
    printf("GETEUID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_getegid(struct kvm_vm *vm) {
	if(vm->syscall_handlers->getegid == NULL) {
		printf("GETEGID handler not found\n");
		return -ENOSYS;
	}
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

	long result = vm->syscall_handlers->getegid();
  if(vm->debug) {
    printf("GETEGID RESULT: %li\n", result);
  }

	return result;
}

long elkvm_do_setpgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getppid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getpgrp(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setsid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setreuid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setregid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getgroups(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setgroups(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setresuid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getresuid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setresgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getresgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getpgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setfsuid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_setfsgid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_getsid(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_capget(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_capset(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rt_sigpending(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rt_sigtimedwait(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rt_sigqueueinfo(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_rt_sigsuspend(struct kvm_vm *vm) {
  return -ENOSYS;
}

long elkvm_do_arch_prctl(struct kvm_vm *vm) {
  uint64_t code = 0;
  uint64_t user_addr = 0;
  struct kvm_vcpu *vcpu = vm->vcpus->vcpu;

  int err = kvm_vcpu_get_sregs(vcpu);
  if(err) {
    return err;
  }

  err = elkvm_syscall2(vm, vcpu, &code, &user_addr);
  if(err) {
    return err;
  }
  uint64_t *host_addr = kvm_pager_get_host_p(&vm->pager, user_addr);
  if(host_addr == NULL) {
    return EFAULT;
  }

  if(vm->debug) {
    printf("ARCH PRCTL with code %i user_addr 0x%lx\n", (int)code, user_addr);
  }
  switch(code) {
    case ARCH_SET_FS:
      vcpu->sregs.fs.base = user_addr;
      break;
    case ARCH_GET_FS:
      *host_addr = vcpu->sregs.fs.base;
      break;
    case ARCH_SET_GS:
      vcpu->sregs.gs.base = user_addr;
      break;
    case ARCH_GET_GS:
      *host_addr = vcpu->sregs.gs.base;
      break;
    default:
      return EINVAL;
  }

  err = kvm_vcpu_set_sregs(vcpu);
  if(err) {
    return err;
  }

  return 0;
}

long elkvm_do_time(struct kvm_vm *vm) {
  if(vm->syscall_handlers->time == NULL) {
    return -ENOSYS;
  }

  uint64_t time_p = 0;
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);
  int err = elkvm_syscall1(vm, vcpu, &time_p);
  if(err) {
    return err;
  }

  time_t *time = kvm_pager_get_host_p(&vm->pager, time_p);

  long result = vm->syscall_handlers->time(time);
  if(vm->debug) {
    printf("\n============ LIBELKVM ===========\n");
    printf("TIME with arg %lx (%p)\n", time_p, time);
    printf("RESULT: %li\n", result);
    printf("=================================\n");
  }

  return result;
}

long elkvm_do_exit_group(struct kvm_vm *vm) {
  uint64_t status = 0;
  int err = elkvm_syscall1(vm, vm->vcpus->vcpu, &status);
  if(err) {
    return err;
  }

  vm->syscall_handlers->exit_group(status);
  /* should not be reached... */
  return -ENOSYS;
}

