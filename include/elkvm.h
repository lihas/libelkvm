#pragma once

#include <libelf.h>

#include <kvm.h>
#include <pager.h>

#define VM_MODE_X86    1
#define VM_MODE_PAGING 2
#define VM_MODE_X86_64 3

#define ELKVM_USER_CHUNK_OFFSET 1024*1024*1024

struct kvm_vm {
	int fd;
	struct vcpu_list *vcpus;
	struct kvm_pager pager;
};

/*
	Create a new VM, with the given mode, cpu count and memory
	Return 0 on success, -1 on error
*/
int kvm_vm_create(struct kvm_opts *, struct kvm_vm *, int, int, int);

/*
	Load an ELF binary, given by the filename into the VM
*/
int kvm_vm_load_binary(struct kvm_vm *, const char *);

/*
	Writes the state of the VM to a given file descriptor
*/
void kvm_dump_vm(struct kvm_vm *, int);

/*
	Destroys a VM and all its data structures
*/
int kvm_vm_destroy(struct kvm_vm *);

