//
// libelkvm - A library that allows execution of an ELF binary inside a virtual
// machine without a full-scale operating system
// Copyright (C) 2013-2015 Florian Pester <fpester@os.inf.tu-dresden.de>, Björn
// Döbel <doebel@os.inf.tu-dresden.de>,   economic rights: Technische Universitaet
// Dresden (Germany)
//
// This file is part of libelkvm.
//
// libelkvm is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// libelkvm is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with libelkvm.  If not, see <http://www.gnu.org/licenses/>.
//

#include <algorithm>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <elkvm/elkvm.h>
#include <elkvm/elkvm-log.h>
#include <elkvm/elkvm-internal.h>
#include <elkvm/elkvm-rlimit.h>
#include <elkvm/pager.h>
#include <elkvm/vcpu.h>

namespace Elkvm {
  extern std::vector<VM> vmi;

  VM::VM(int vmfd, int argc, char ** argv, char **environ,
      int run_struct_size,
      const Elkvm::hypercall_handlers * const hyp_handlers,
      const Elkvm::elkvm_handlers * const handlers,
      int debug) :
    cpus(),
    _debug(debug == 1),
    _rm(std::make_shared<RegionManager>(vmfd)),
    _gdt(nullptr),
    hm(_rm),
    _vmfd(vmfd),
    _argc(argc),
    _argv(argv),
    _environ(environ),
    _run_struct_size(run_struct_size),
    _rlimit(),
    sigs(),
    sighandler_cleanup(),
    hypercall_handlers(hyp_handlers),
    syscall_handlers(handlers)
  {}

  int VM::add_cpu() {
    std::shared_ptr<VCPU> vcpu =
      std::make_shared<VCPU>(_rm, _vmfd, cpus.size());

    if(vcpu == NULL) {
      return -ENOMEM;
    }

    cpus.push_back(vcpu);

    vcpu->set_regs();
    vcpu->set_sregs();
    return 0;
  }

  bool VM::address_mapped(guestptr_t addr) const {
    return hm.address_mapped(addr);
  }

  Mapping &VM::find_mapping(guestptr_t addr) {
    if(hm.contains_address(addr)) {
      return hm.find_mapping(addr);
    }
    assert(false && "could not find mapping!");
  }


  int VM::load_flat(Elkvm::elkvm_flat &flat, const std::string path,
      bool kernel) {
    int fd = open(path.c_str(), O_RDONLY);
    if(fd < 0) {
      ERROR() << "Could not find flat binary at: " << path << std::endl;
      return -errno;
    }

    struct stat stbuf;
    int err = fstat(fd, &stbuf);
    if(err) {
      close(fd);
      return -errno;
    }

    flat.size = stbuf.st_size;
    std::shared_ptr<Elkvm::Region> region = _rm->allocate_region(stbuf.st_size,path.c_str());

    if(kernel) {
      guestptr_t addr = _rm->get_pager().map_kernel_page(
          region->base_address(),
          PT_OPT_EXEC);
      if(addr == 0x0) {
        close(fd);
        return -ENOMEM;
      }
      region->set_guest_addr(addr);
    } else {
      /* XXX this will break! */
      region->set_guest_addr(0x1000);
      err = _rm->get_pager().map_user_page(
          region->base_address(),
          region->guest_address(),
          PT_OPT_EXEC);
      assert(err == 0);
    }

    char *buf = reinterpret_cast<char *>(region->base_address());
    int bufsize = ELKVM_PAGESIZE;
    int bytes = 0;
    while((bytes = read(fd, buf, bufsize)) > 0) {
      buf += bytes;
    }

    close(fd);
    flat.region = region;

    return 0;
  }

  const std::shared_ptr<VCPU>& VM::get_vcpu(int num) const {
    return cpus.at(num);
  }

  Elkvm::elkvm_flat &VM::get_cleanup_flat() {
    return sighandler_cleanup;
  }

  const struct sigaction* VM::get_sig_ptr(unsigned sig) const {
    return &sigs.signals[sig];
  }

  // TODO: needed?
  bool operator==(const VM &lhs, const VM &rhs) {
    return lhs.get_vmfd() == rhs.get_vmfd();
  }

#if 0
  VM &get_vmi(Elkvm::kvm_vm *vm) {
    auto it = std::find(vmi.begin(), vmi.end(), *vm);
    assert(it != vmi.end());
    return *it;
  }
#endif

  /* TODO: Should be a function of the vCPU */
  unsigned get_hypercall_type(const std::shared_ptr<VCPU>& vcpu)
  {
    return vcpu->pop();
  }

  //namespace Elkvm
}
