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

#include <errno.h>
#include <algorithm>
#include <cstring>
#include <iostream>

#include <elkvm/elkvm.h>
#include <elkvm/elkvm-internal.h>
#include <elkvm/elfloader.h>
#include <elkvm/heap.h>
#include <elkvm/region.h>
#include <elkvm/pager.h>
#include <elkvm/region_manager.h>

namespace Elkvm {

  void HeapManager::free_unused_mappings(guestptr_t brk) {
    while(brk <= mappings_for_brk.back().guest_address()) {
      /* no need to call pop_back here, unmap does this for us */
      int err = unmap(mappings_for_brk.back());
      assert(err == 0);
    }
  }

  int HeapManager::shrink(guestptr_t newbrk) {
    free_unused_mappings(newbrk);

    guestptr_t slice_base = newbrk;
    if(!page_aligned<guestptr_t>(newbrk)) {
      slice_base = next_page(slice_base);
    }

    Mapping &m = mappings_for_brk.back();
    if(m.guest_address() + m.get_length() == slice_base) {
      return 0;
    }

    assert(m.guest_address() + m.get_length() > slice_base);
    size_t len = m.guest_address() + m.get_length() - slice_base;
    slice(m, slice_base, len);
    return 0;
  }

  int HeapManager::grow(guestptr_t newbrk) {
    assert(newbrk > curbrk);
    size_t sz = newbrk - curbrk;
    std::shared_ptr<Region> r = _rm->allocate_region(sz);
    Mapping m(r, curbrk, sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, 0, 0);
    mappings_for_brk.push_back(m);
    return map(m);
  }

  int HeapManager::brk(guestptr_t newbrk) {
    if(newbrk < curbrk) {
      int err = shrink(newbrk);
      if(err) {
        return err;
      }
      curbrk = newbrk;
      return 0;
    }

    Mapping &m = mappings_for_brk.back();
    if(!m.fits_address(newbrk-1)) {
      curbrk = m.grow_to_fill();
      map(m);

      int err = grow(newbrk);
      if(err) {
        return err;
      }
      curbrk = newbrk;
      return 0;
    }

    auto sz = m.get_length();
    auto growsz = newbrk - curbrk;
    auto newsz = m.grow(sz + growsz);
    assert(newsz == sz + growsz && "mapping could not grow by correct size");
    map(m);
    curbrk = newbrk;

    return 0;
  }

  bool HeapManager::contains_address(guestptr_t addr) const {
    if(!brk_contains_address(addr)) {
      auto it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
          [addr](const Mapping &m) { return m.contains_address(addr); });
      return it == mappings_for_mmap.end();
    }
    return true;
  }

  Mapping &HeapManager::find_mapping(guestptr_t addr) {
    auto it = std::find_if(mappings_for_brk.begin(), mappings_for_brk.end(),
        [addr](const Mapping &m) { return m.contains_address(addr); });
    if(it == mappings_for_brk.end()) {
      it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
          [addr](const Mapping &m) { return m.contains_address(addr); });
      assert(it != mappings_for_mmap.end());
    }

    return *it;
  }

  Mapping &HeapManager::find_mapping(void *host_p) {
    auto it = std::find_if(mappings_for_brk.begin(), mappings_for_brk.end(),
        [host_p](const Mapping &m) { return m.contains_address(host_p); });
    if(it == mappings_for_brk.end()) {
      it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
          [host_p](const Mapping &m) { return m.contains_address(host_p); });
    }
    assert(it != mappings_for_mmap.end());

    return *it;
  }

  bool HeapManager::address_mapped(guestptr_t addr) const {
    auto it = std::find_if(mappings_for_brk.begin(), mappings_for_brk.end(),
        [addr](const Mapping &m) { return m.contains_address(addr); });
    if(it == mappings_for_brk.end()) {
      it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
          [addr](const Mapping &m) { return m.contains_address(addr); });
      return it != mappings_for_mmap.end();
    } else {
      return true;
    }
  }

  Mapping &HeapManager::get_mapping(guestptr_t addr, size_t length, int prot,
      int flags, int fd, off_t off) {
    length = pagesize_align(length);
    if(addr && (flags & MAP_FIXED)) {
      /* check if we already have a mapping for that address,
       * if we do, we need to split the old mapping, and replace the contents
       * with whatever the user requested,
       * however if we have an exact match, we need to return that */
      auto it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
          [addr, length, prot, flags, fd, off](const Mapping &m)
          { return m.guest_address() == addr
                && m.get_length() == length; });
      if(it == mappings_for_mmap.end()) {
        it = std::find_if(mappings_for_mmap.begin(), mappings_for_mmap.end(),
            [addr](const Mapping &m) { return m.contains_address(addr); });
        if(it != mappings_for_mmap.end()) {
          /* TODO this should be done after we get back to the user! */
          /* this mapping needs to be split! */
          slice(*it, addr, length);
        }
        return create_mapping(addr, length, prot, flags, fd, off);
      }

      /* if we have an exact match, we only need to update this mapping's protection
       * and flags etc. and return the mapping object */
      it->modify(prot, flags, fd, off);
      map(*it);

      assert(!it->get_region()->is_free());
      assert(_rm->find_region(it->base_address()) != nullptr);
      return *it;
    } else {
      return create_mapping(0x0, length, prot, flags, fd, off);
    }
  }

  Mapping &HeapManager::create_mapping(guestptr_t addr, size_t length, int prot,
      int flags, int fd, off_t off, std::shared_ptr<Region> r) {

    length = pagesize_align(length);

    if(r == nullptr) {
      std::ostringstream str;
      str << "mapping with fd: " << std::dec << fd;
      r = _rm->allocate_region(length, str.str());
    }

    mappings_for_mmap.emplace_back(r, addr, length, prot, flags, fd, off);
    Mapping &mapping = mappings_for_mmap.back();
    int err = map(mapping);
    assert(err == 0);

    assert(!mapping.get_region()->is_free());
    assert(_rm->find_region(mapping.base_address()) != nullptr);
    return mapping;
  }

  void HeapManager::free_mapping(Mapping &mapping) {
    auto it = std::find(mappings_for_brk.begin(), mappings_for_brk.end(), mapping);
    if(it == mappings_for_brk.end()) {
      it = std::find(mappings_for_mmap.begin(), mappings_for_mmap.end(), mapping);
      assert(it != mappings_for_mmap.end());
      mappings_for_mmap.erase(it);
    } else {
      mappings_for_brk.erase(it);
    }
  }

  int HeapManager::init(std::shared_ptr<Region> data, size_t sz) {
    mappings_for_brk.emplace_back(data, data->guest_address(), sz,
        PROT_READ | PROT_WRITE, MAP_ANONYMOUS, 0, 0);

    curbrk = next_page(data->guest_address() + sz);
    assert(data->contains_address(curbrk - 1) && "initial brk address must be in data region");

    return 0;
  }

  void HeapManager::dump_mappings() const {
    std::cout << "DUMPING ALL MAPPINGS:\n";
    std::cout << "====================\n";
    for(const auto &reg : mappings_for_brk) {
      print(std::cout, reg);
    }
    for(const auto &reg : mappings_for_mmap) {
      print(std::cout, reg);
    }

    std::cout << std::endl << std::endl;
  }

  int HeapManager::map(Mapping &m) {
    if(!m.readable() && !m.writeable() && !m.executable()) {
      _rm->get_pager().unmap_region(m.guest_address(), m.get_pages());
      m.set_unmapped();
      return 0;
    }

    auto it = std::find(mappings_for_mmap.begin(), mappings_for_mmap.end(), m);
    if(it == mappings_for_mmap.end()) {
      mappings_for_mmap.push_back(m);
    }

    ptopt_t opts = 0;
    if(m.writeable()) {
      opts |= PT_OPT_WRITE;
    }
    if(m.executable()) {
      opts |= PT_OPT_EXEC;
    }

    /* add page table entries according to the options specified by the monitor */
    assert(m.base_address() == m.get_region()->base_address());
    int err = _rm->get_pager().map_region(m.base_address(), m.guest_address(),
        m.get_pages(), opts);
    if(m.get_region()->is_free()) {
      _rm->use_region(m.get_region());
    }
    assert(err == 0);
    return err;
  }

  guestptr_t
  HeapManager::remap(Mapping &m, guestptr_t new_address_p, size_t new_size, int flags) {
	(void)new_address_p; // unused warning...
    /* 2 simple cases:
     *   1) the mapping gets smaller, just make it so
     *   2) the mapping gets larger but still fits into the region, just make it so
     * hard case:
     *   the mapping gets larger and will not fit into the region,
     *   we can get a new mapping, copy everything over and be done
     *   or we can get a new mapping and map it so that virtual memory still
     *   fits.
     * MREMAP_FIXED case:
     *   check if there is an old mapping at this point and unmap if so
     *   then remap to this location.
     */
    assert(!(flags & MREMAP_FIXED) && "MREMAP_FIXED not supported right now");

    if(new_size < m.get_length()) {
      unmap_to_new_size(m, new_size);
      return m.guest_address();
    }

    if(m.fits_address(m.guest_address() + new_size - 1)) {
      m.grow(new_size);
      return m.guest_address();
    }

    return create_resized_mapping(m, new_size);
  }

  void HeapManager::unmap_to_new_size(Mapping &m, size_t new_size) {
      size_t diff = m.get_length() - new_size;
      guestptr_t unmap_addr = m.guest_address() + new_size;
      unsigned pages = pages_from_size(diff);
      unmap(m, unmap_addr, pages);
  }

  guestptr_t HeapManager::create_resized_mapping(Mapping &m, size_t new_size) {
    Mapping &new_mapping = get_mapping(0x0, new_size, m.get_prot(), m.get_flags(),
        m.get_fd(), m.get_offset());

    std::memcpy(new_mapping.base_address(), m.base_address(), m.get_length());
    map(new_mapping);

    /* unmap invalidates the ref to m AND new_mapping!
     * so we need to get the correct guest address of the new mapping here
     * and return that after the unmap */
    guestptr_t addr = new_mapping.guest_address();
    unmap(m);

    return addr;
  }

  int HeapManager::unmap(Mapping &m) {
    return unmap(m, m.guest_address(), m.get_pages());
  }

  int HeapManager::unmap(Mapping &m, guestptr_t unmap_addr, unsigned pages) {
    assert(m.contains_address(unmap_addr));
    assert(pages <= m.get_pages());
    assert(m.contains_address(unmap_addr + ((pages-1) * ELKVM_PAGESIZE)));

    int err = _rm->get_pager().unmap_region(unmap_addr, pages);
    assert(err == 0 && "could not unmap this mapping");
    m.pages_unmapped(pages);

    auto pages_left = m.get_pages();
    if(pages_left == 0) {
      _rm->free_region(m.get_region());
      free_mapping(m);
    }

    return pages_left;
  }

  void HeapManager::slice_region(Mapping &m, off_t off, size_t len) {
    auto regions = m.get_region()->slice_center(off, len);
    assert(m.get_region() != nullptr);
    _rm->use_region(regions.first);
    _rm->add_free_region(regions.second);
  }

  void HeapManager::slice(Mapping &m, guestptr_t slice_base, size_t len) {
    assert(m.contains_address(slice_base)
        && "slice address must be contained in mapping");
    guestptr_t addr = m.guest_address();
    if(slice_base == addr) {
      slice_begin(m, len);
      return;
    }

    /* slice_base is now always larger than host_p */
    assert(slice_base >= addr);
    off_t off = slice_base - addr;

    if(m.contains_address(slice_base + len)) {
      /* slice_center also includes the case that the end of the sliced region
       * is the end of this region */
      slice_center(m, off, len);
    } else {
      /* slice_end is only needed, when we want to expand the new region beyond
       * the end of this region */
      slice_end(m, slice_base);
    }
  }

  void HeapManager::slice_begin(Mapping &m, size_t len) {
    unsigned pages = pages_from_size(len);
    unmap(m, m.guest_address(), pages);
    std::shared_ptr<Region> r = m.move_guest_address(len);
    _rm->add_free_region(r);
    assert(m.get_region() != nullptr);
  }

  void HeapManager::slice_center(Mapping &m, off_t off, size_t len) {
    assert(m.contains_address(reinterpret_cast<char *>(m.base_address()) + off
          + len));
    assert(0 <= off);
    assert(off < (off_t)m.get_length());
    assert(m.get_region() != nullptr);

    assert(m.get_length() <= m.get_region()->size());

    /* unmap the old stuff */
    unsigned pages = pages_from_size(len);
    unmap(m, m.guest_address() + off, pages);

    slice_region(m, off, len);
    size_t slice_sz = off + len;
    size_t mapping_sz = m.get_length();

    /* set the size of the old mapping here, because create_mapping invalidates m */
    m.set_length(off);

    if(mapping_sz > slice_sz) {
      size_t rem = mapping_sz - off - len;
      auto r = _rm->find_region(reinterpret_cast<char *>(m.base_address()) + off
          + len);
      /* There should be no need to process this mapping any further, because we
       * feed it the split memory region, with the old data inside */
      create_mapping(m.guest_address() + slice_sz, rem, m.get_prot(), m.get_flags(),
          m.get_fd(), m.get_offset() + slice_sz, r);
      /* CANNOT use m here anymore, because we got m from an iterator and the
       * container gets changed by create_mapping! */
    }
  }

  void HeapManager::slice_end(Mapping &m, guestptr_t slice_base) {
    assert(m.contains_address(slice_base));

    /* unmap the old stuff */
    unmap(m, slice_base, pages_from_size((m.guest_address() + m.get_length())
          - slice_base));

    assert(((m.guest_address() + m.get_length()) - slice_base) < m.get_length());
    m.set_length(m.get_length() - ((m.guest_address() + m.get_length())
          - slice_base));

    /* TODO free part of the attached memory region */
  }

  //namespace Elkvm
}
