#include <signal.h>

#include <elkvm.h>

#include "flats.h"
#include "stack.h"
#include "elkvm-signal.h"

static int pending_signals[32];
static int num_pending_signals = 0;

void elkvm_signal_handler(int signum) {

  printf("\n============ LIBELKVM ===========\n");
  printf(" CAUGHT SIGNAL %i\n", signum);
  printf(" SIGNALS pending: %i\n", num_pending_signals);
  printf("=================================\n");

  pending_signals[num_pending_signals] = signum;
  num_pending_signals++;

}

int elkvm_signal_init(struct kvm_vm *vm) {
  assert(vm != NULL);
  memset(&vm->sigs, 0, sizeof(struct elkvm_signals));
  return 0;
}

int elkvm_signal_register(struct kvm_vm *vm, int signum, struct sigaction *act,
    struct sigaction *oldact) {
  assert(vm != NULL);
  assert(signum < _NSIG);

  if(oldact != NULL) {
    memcpy(oldact, &vm->sigs.signals[signum], sizeof(struct sigaction));
  }

  if(act != NULL) {
    memcpy(&vm->sigs.signals[signum], act, sizeof(struct sigaction));

    struct sigaction sa;
    sa.sa_handler = elkvm_signal_handler;
    int err = sigemptyset(&sa.sa_mask);
    assert(err == 0);
    sa.sa_flags = 0;
    err = sigaction(signum, &sa, NULL);
    assert(err == 0);
  }

  return 0;
}

int elkvm_signal_deliver(struct kvm_vm *vm) {
  assert(vm != NULL);

  if(num_pending_signals <= 0) {
    return 0;
  }

  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);
  assert(vcpu != NULL);

  /* save the vcpu regs */
  memcpy(&vm->sigs.saved_vcpu.regs, &vcpu->regs,
      sizeof(struct kvm_regs) + sizeof(struct kvm_sregs));
  /* set signal handler flag in vm */
  vm->sigs.handler_active = true;

  num_pending_signals--;
  int signum = pending_signals[num_pending_signals];

  printf("sighandler_cleanup: 0x%lx (%p)\n",
      vm->sighandler_cleanup->region->guest_virtual,
      vm->sighandler_cleanup->region->host_base_p);
  /* push rax onto stack */
  elkvm_pushq(vm, vcpu, vcpu->regs.rax);

  /* push signal handler cleanup asm addr onto stack */
  elkvm_pushq(vm, vcpu, vm->sighandler_cleanup->region->guest_virtual);

  /* setup the signal handler stack frame and pass the signal number as arg */
  elkvm_pushq(vm, vcpu, (uint64_t) vm->sigs.signals[signum].sa_handler);
  vcpu->regs.rdi = signum;

//  kvm_vcpu_dump_regs(vcpu);
//  kvm_vcpu_dump_code(vcpu);
//  elkvm_dump_stack(vm, vcpu);

  return 0;
}

int elkvm_signal_cleanup(struct kvm_vm *vm) {
  struct kvm_vcpu *vcpu = elkvm_vcpu_get(vm, 0);
  assert(vcpu != NULL);

  /* clear signal handler flag */
  vm->sigs.handler_active = false;

  /* restore the vcpu regs */
  memcpy(&vcpu->regs, &vm->sigs.saved_vcpu.regs,
      sizeof(struct kvm_regs) + sizeof(struct kvm_sregs));

  /* TODO check for more pending signals */

  return 0;
}
