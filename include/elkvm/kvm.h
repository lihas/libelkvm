#pragma once

#include <string>

#include <elkvm/regs.h>
#include <elkvm/syscall.h>

#define KVM_EXPECT_VERSION 12
#define KVM_DEV_PATH "/dev/kvm"

namespace Elkvm {
  class Segment;

struct elkvm_opts {
	int argc;
	char **argv;
	char **environ;
  bool debug;

  /* TODO kvm-specific stuff */
  int fd;
  int run_struct_size;
};

namespace KVM {

  int init(struct elkvm_opts *opts);

  class VCPU {
    private:
      int fd;
      struct kvm_regs regs;
      struct kvm_sregs sregs;
      struct kvm_run *run_struct;

      Elkvm::Segment get_reg(const struct kvm_dtable * const ptr) const;
      Elkvm::Segment get_reg(const struct kvm_segment * const ptr) const;
      void set_reg(struct kvm_dtable *ptr, const Elkvm::Segment &seg);
      void set_reg(struct kvm_segment *ptr, const Elkvm::Segment &seg);

      /* internal debugging stuff */
      struct kvm_guest_debug debug;
      int set_debug();

    public:
      VCPU(int vmfd, unsigned num);

      CURRENT_ABI::paramtype get_reg(Elkvm::Reg_t reg) const;
      void set_reg(Elkvm::Reg_t reg, CURRENT_ABI::paramtype val);
      Segment get_reg(Elkvm::Seg_t segtype) const;
      void set_reg(Elkvm::Seg_t segtype, const Elkvm::Segment &seg);

      int get_regs();
      int get_sregs();
      int set_regs();
      int set_sregs();
      CURRENT_ABI::paramtype get_interrupt_bitmap(unsigned idx) const;
      CURRENT_ABI::paramtype get_msr(uint32_t idx);
      void set_msr(uint32_t idx, CURRENT_ABI::paramtype data);

      int run();

      /* Debugging */
      int enable_debug();
      int singlestep();
      int singlestep_off();
      int enable_software_breakpoints();

      uint32_t exit_reason();
      uint64_t hardware_exit_reason();
      uint64_t hardware_entry_failure_reason();
      std::ostream &print_mmio(std::ostream &os);

  };

//namespace KVM
}

} // namespace Elkvm

int elkvm_init(Elkvm::elkvm_opts *, int, char **, char **);
int elkvm_cleanup(Elkvm::elkvm_opts *);
