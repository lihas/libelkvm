#include <linux/kvm.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stropts.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <elkvm.h>
#include <flats.h>
#include <gdt.h>
#include <idt.h>
#include <kvm.h>
#include <pager.h>
#include <region.h>
#include <stack.h>
#include <vcpu.h>

int kvm_vm_create(struct elkvm_opts *opts, struct kvm_vm *vm, int mode, int cpus, int memory_size, struct elkvm_handlers *handlers) {
	int err = 0;

	if(opts->fd <= 0) {
		return -EIO;
	}

	vm->fd = ioctl(opts->fd, KVM_CREATE_VM, 0);
	if(vm->fd < 0) {
		return -errno;
	}

	vm->run_struct_size = ioctl(opts->fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if(vm->run_struct_size < 0) {
		return -EIO;
	}

	for(int i = 0; i < cpus; i++) {
		err = kvm_vcpu_create(vm, mode);
		if(err) {
			return err;
		}
	}

	err = elkvm_region_setup(vm);
	if(err) {
		return err;
	}

	err = kvm_pager_initialize(vm, mode);
	if(err) {
		return err;
	}

	err = elkvm_initialize_stack(opts, vm);
	if(err) {
		return err;
	}

	err = kvm_vm_map_chunk(vm, &vm->pager.system_chunk);
	if(err) {
		return err;
	}

	err = elkvm_gdt_setup(vm);
	if(err) {
		return err;
	}

	struct elkvm_flat idth;
	char *isr_path = "/home/flo/Dokumente/projekte/libelkvm/res/isr";
	err = elkvm_load_flat(vm, &idth, isr_path);
	if(err) {
		return err;
	}

	err = elkvm_idt_setup(vm, &idth);
	if(err) {
		return err;
	}

	struct elkvm_flat sysenter;
	char *sysenter_path = "/home/flo/Dokumente/projekte/libelkvm/res/entry";
	err = elkvm_load_flat(vm, &sysenter, sysenter_path);
	if(err) {
		return err;
	}

	/*
	 * setup the lstar register with the syscall handler
	 */
	err = kvm_vcpu_set_msr(vm->vcpus->vcpu,
			VCPU_MSR_LSTAR,
			sysenter.region->guest_virtual);
	if(err) {
		return err;
	}

	vm->syscall_handlers = handlers;

	return 0;
}

int elkvm_load_flat(struct kvm_vm *vm, struct elkvm_flat *flat, const char * path) {
	int fd = open(path, O_RDONLY);
	if(fd < 0) {
		return -errno;
	}

	struct stat stbuf;
	int err = fstat(fd, &stbuf);
	if(err) {
		close(fd);
		return -errno;
	}

	flat->size = stbuf.st_size;
	flat->region = elkvm_region_create(vm, stbuf.st_size);
	flat->region->guest_virtual = 0x0;

	flat->region->guest_virtual = kvm_pager_map_kernel_page(&vm->pager,
			flat->region->host_base_p, 0, 1);
	if(flat->region->guest_virtual == 0) {
		close(fd);
		return -ENOMEM;
	}

	char *buf = flat->region->host_base_p;
	int bufsize = 0x1000;
	int bytes = 0;
	while((bytes = read(fd, buf, bufsize)) > 0) {
		buf += bytes;
	}

	close(fd);

	return 0;
}

int elkvm_region_setup(struct kvm_vm *vm) {
	/* create an initial chunk for system data */

	void *system_chunk_p;
  vm->root_region = NULL;

	int err = posix_memalign(&system_chunk_p, 0x1000, ELKVM_SYSTEM_MEMSIZE);
	if(err) {
		return err;
	}

  struct elkvm_memory_region *region = elkvm_region_alloc(system_chunk_p,
      ELKVM_SYSTEM_MEMSIZE, 0);
  vm->root_region = elkvm_region_list_prepend(vm, region);

	vm->pager.system_chunk.userspace_addr = (__u64)system_chunk_p;
	vm->pager.system_chunk.guest_phys_addr = 0x0;
	vm->pager.system_chunk.memory_size = ELKVM_SYSTEM_MEMSIZE;
	vm->pager.system_chunk.flags = 0;
	vm->pager.system_chunk.slot = 0;

  vm->pager.total_memsz = vm->pager.system_chunk.memory_size;

	return 0;
}

int kvm_check_cap(struct elkvm_opts *kvm, int cap) {
	if(kvm->fd < 1) {
		return -EIO;
	}

	int r = ioctl(kvm->fd, KVM_CHECK_EXTENSION, cap);
	if(r < 0) {
		return -errno;
	}
	return r;
}

int kvm_vm_vcpu_count(struct kvm_vm *vm) {
	int count = 0;
	struct vcpu_list *vl = vm->vcpus;
	if(vl == NULL) {
		return 0;
	}

	while(vl != NULL) {
		if(vl->vcpu != NULL) {
			count++;
		}
		vl = vl->next;
	}
	return count;
}

int kvm_vm_destroy(struct kvm_vm *vm) {
	return -1;
}

int elkvm_init(struct elkvm_opts *opts, int argc, char **argv, char **environ) {
	opts->argc = argc;
	opts->argv = argv;
	opts->environ = environ;

	opts->fd = open(KVM_DEV_PATH, O_RDWR);
	if(opts->fd < 0) {
		return opts->fd;
	}

	int version = ioctl(opts->fd, KVM_GET_API_VERSION, 0);
	if(version != KVM_EXPECT_VERSION) {
		return -1;
	}

	opts->run_struct_size = ioctl(opts->fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if(opts->run_struct_size <= 0) {
		return -1;
	}

	return 0;
}

int elkvm_cleanup(struct elkvm_opts *opts) {
	close(opts->fd);
	opts->fd = 0;
	opts->run_struct_size = 0;
	return 0;
}

int elkvm_initialize_stack(struct elkvm_opts *opts, struct kvm_vm *vm) {
	/* for now the region to hold env etc. will be 12 pages large */
	struct elkvm_memory_region *env_region = elkvm_region_create(vm, 0x12000);
	env_region->guest_virtual = LINUX_64_STACK_BASE -
		env_region->region_size;

	/* get a 4 page large region for the stack */
	/* TODO check if this still makes sense! */
	struct elkvm_memory_region *stack_region = elkvm_region_create(vm, 0x4000);
	stack_region->guest_virtual = env_region->guest_virtual;
	stack_region->grows_downward = 1;

	/* get a frame for the kernel (interrupt) stack */
  /* this is only ONE page large */
	vm->kernel_stack = elkvm_region_create(vm, 0x1000);
	vm->kernel_stack->grows_downward = 1;

	/* create a mapping for the kernel (interrupt) stack */
	vm->kernel_stack->guest_virtual = kvm_pager_map_kernel_page(&vm->pager,
			vm->kernel_stack->host_base_p, 1, 0);
	if(vm->kernel_stack->guest_virtual == 0) {
		return -ENOMEM;
	}
  /* as stack grows downward we save it's virtual address at the page afterwards */
  vm->kernel_stack->guest_virtual += 0x1000;

	int err = kvm_vcpu_get_regs(vm->vcpus->vcpu);
	if(err) {
		return err;
	}

	vm->vcpus->vcpu->regs.rsp = env_region->guest_virtual;

	err = kvm_vcpu_set_regs(vm->vcpus->vcpu);
	if(err) {
		return err;
	}

	err = kvm_pager_create_mapping(&vm->pager,
			env_region->host_base_p,
			vm->vcpus->vcpu->regs.rsp, 1, 0);
	if(err) {
		return err;
	}

	void *host_target_p = env_region->host_base_p;

  /* TODO put the auxv pointers onto the stack in the correct order */
  /* XXX this breaks, if we do not get the original envp */
  char **auxv_p = (char *)opts->environ;
  printf("Got auxv_p at: %p\n", auxv_p);
  while(*auxv_p != NULL) {
    auxv_p++;
    printf("Got auxv_p at: %p, val: %p\n", auxv_p, *auxv_p);
  }
  auxv_p++;

  Elf64_auxv_t *auxv = (Elf64_auxv_t *)auxv_p;
  for( ; auxv->a_type != AT_NULL; auxv++);

  for( ; auxv > auxv_p; auxv--) {
    elkvm_pushq(vm, vm->vcpus->vcpu, auxv->a_un.a_val);
    elkvm_pushq(vm, vm->vcpus->vcpu, auxv->a_type);
  }
  elkvm_pushq(vm, vm->vcpus->vcpu, 0);


  elkvm_pushq(vm, vm->vcpus->vcpu, 0);
	int bytes = elkvm_copy_and_push_str_arr_p(vm,
      env_region, 0,
      opts->environ);
	elkvm_pushq(vm, vm->vcpus->vcpu, 0);
  assert(bytes > 0);

	/* followed by argv pointers */
	bytes = elkvm_copy_and_push_str_arr_p(vm,
      env_region, bytes,
      opts->argv);
  assert(bytes > 0);

	/* at last push argc on the stack */
	elkvm_pushq(vm, vm->vcpus->vcpu, opts->argc);

  elkvm_dump_stack(vm, vm->vcpus->vcpu);

	return 0;
}

int elkvm_copy_and_push_str_arr_p(struct kvm_vm *vm,
    struct elkvm_memory_region *region,
    uint64_t offset,
	 	char **str) {
  if(str == NULL) {
    return 0;
  }

	void *target = region->host_base_p + offset;
  uint64_t guest_virtual = region->guest_virtual + offset;
	int bytes = 0;

	//first push the environment onto the stack
	int i = 0;
	while(str[i]) {
		int len = strlen(str[i]) + 1;

		//copy the data into the vm memory
		strcpy(target, str[i]);

		//and push the pointer for the vm
		int err = elkvm_pushq(vm, vm->vcpus->vcpu, guest_virtual);
		if(err) {
			return err;
		}

    target = target + len;
		bytes += len;
    guest_virtual = guest_virtual + len;
		i++;
	}

	return bytes;
}

int kvm_vm_map_chunk(struct kvm_vm *vm, struct kvm_userspace_memory_region *chunk) {
	int err = ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, chunk);
//	if(err) {
//		long sz = sysconf(_SC_PAGESIZE);
//		printf("Could not set memory region\n");
//		printf("Error No: %i Msg: %s\n", errno, strerror(errno));
//		printf("Pagesize is: %li\n", sz);
//		printf("Here are some sanity checks that are applied in kernel:\n");
//		int ms = chunk->memory_size & (sz-1);
//		int pa = chunk->guest_phys_addr & (sz-1);
//		int ua = chunk->userspace_addr & (sz-1);
//		printf("memory_size & (PAGE_SIZE -1): %i\n", ms);
//		printf("guest_phys_addr & (PAGE_SIZE-1): %i\n", pa);
//		printf("userspace_addr & (PAGE_SIZE-1): %i\n", ua);
//		printf("TODO verify write access\n");
//	}
	return err;
}

int elkvm_dump_valid_msrs(struct elkvm_opts *opts) {
	struct kvm_msr_list *list = malloc(
			sizeof(struct kvm_msr_list) + 255 * sizeof(uint32_t));
	list->nmsrs = 255;

	int err = ioctl(opts->fd, KVM_GET_MSR_INDEX_LIST, list);
	if(err < 0) {
		free(list);
		return -errno;
	}

	for(int i = 0; i < list->nmsrs; i++) {
		printf("MSR: 0x%x\n", list->indices[i]);
	}
	free(list);

	return 0;
}

void elkvm_print_regions(struct kvm_vm *vm) {
	printf("\n System Memory Regions:\n");
	printf(" ----------------------\n");
	printf(" Host virtual\t\tGuest virtual\t\tSize\t\t\tD\n");
	elkvm_dump_region(vm->root_region->data);
	printf("\n");
}

void elkvm_dump_region(struct elkvm_memory_region *region) {
	printf("%16p\t0x%016lx\t0x%016lx\t%i\n", region->host_base_p,
		region->guest_virtual, region->region_size, region->grows_downward);
	if(region->lc != NULL) {
		elkvm_dump_region(region->lc);
	}
	if(region->rc != NULL) {
		elkvm_dump_region(region->rc);
	}
}
