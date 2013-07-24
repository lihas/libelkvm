#include <assert.h>
#include <elkvm.h>
#include <pager.h>
#include <stack.h>
#include <vcpu.h>

uint16_t pop_stack(struct kvm_vm *vm, struct kvm_vcpu *vcpu) {
	int err = kvm_vcpu_get_regs(vcpu);
	assert(err == 0);

	uint16_t *host_p = (uint16_t *)kvm_pager_get_host_p(&vm->pager, vcpu->regs.rsp);

	vcpu->regs.rsp += 0x10;
	err = kvm_vcpu_set_regs(vcpu);
	assert(err == 0);

	return *host_p;
}

int push_stack(struct kvm_vm *vm, struct kvm_vcpu *vcpu, uint16_t val) {
	int err = kvm_vcpu_get_regs(vcpu);
	if(err < 0) {
		return err;
	}

	vcpu->regs.rsp -= 0x10;

	uint16_t *host_p = kvm_pager_get_host_p(&vm->pager, vcpu->regs.rsp);
	if(host_p == NULL) {
		host_p = kvm_pager_get_host_p(&vm->pager, vcpu->regs.rsp + 0x10);
		if(host_p == NULL) {
			return -1;
		}
		err = kvm_pager_create_mapping(&vm->pager, host_p - 0x1000, 
				vcpu->regs.rsp + 0x10 - 0x1000);
		if(err < 0) {
			return err;
		}
		host_p -= 0x10;
	}
	*host_p = val;

	err = kvm_vcpu_set_regs(vcpu);
	return err;
}