#ifndef _RISCV_MMU_H
#define _RISCV_MMU_H

#include "decode.h"
#include "trap.h"
#include "common.h"
#include "processor.h"

class processor_t;

// virtual memory configuration
typedef reg_t pte_t;
const reg_t LEVELS = 4;
const reg_t PGSHIFT = 12;
const reg_t PGSIZE = 1 << PGSHIFT;
const reg_t PTIDXBITS = PGSHIFT - (sizeof(pte_t) == 8 ? 3 : 2);
const reg_t PPN_BITS = 8*sizeof(reg_t) - PGSHIFT;

// page table entry (PTE) fields
#define PTE_T    0x001 // Entry is a page Table descriptor
#define PTE_E    0x002 // Entry is a page table Entry
#define PTE_R    0x004 // Referenced
#define PTE_D    0x008 // Dirty
#define PTE_UX   0x010 // User eXecute permission
#define PTE_UW   0x020 // User Read permission
#define PTE_UR   0x040 // User Write permission
#define PTE_SX   0x080 // Supervisor eXecute permission
#define PTE_SW   0x100 // Supervisor Read permission
#define PTE_SR   0x200 // Supervisor Write permission
#define PTE_PERM (PTE_SR | PTE_SW | PTE_SX | PTE_UR | PTE_UW | PTE_UX)
#define PTE_PERM_SHIFT 4
#define PTE_PPN_SHIFT  12 // LSB of physical page number in the PTE

// this class implements a processor's port into the virtual memory system.
// an MMU and instruction cache are maintained for simulator performance.
class mmu_t
{
public:
  mmu_t(char* _mem, size_t _memsz);
  ~mmu_t();

  // template for functions that load an aligned value from memory
  #define load_func(type) \
    type##_t load_##type(reg_t addr) { \
      if(unlikely(addr % sizeof(type##_t))) \
      { \
        badvaddr = addr; \
        throw trap_load_address_misaligned; \
      } \
      void* paddr = translate(addr, false, false); \
      return *(type##_t*)paddr; \
    }

  // load value from memory at aligned address; zero extend to register width
  load_func(uint8)
  load_func(uint16)
  load_func(uint32)
  load_func(uint64)

  // load value from memory at aligned address; sign extend to register width
  load_func(int8)
  load_func(int16)
  load_func(int32)
  load_func(int64)

  // template for functions that store an aligned value to memory
  #define store_func(type) \
    void store_##type(reg_t addr, type##_t val) { \
      if(unlikely(addr % sizeof(type##_t))) \
      { \
        badvaddr = addr; \
        throw trap_store_address_misaligned; \
      } \
      void* paddr = translate(addr, true, false); \
      *(type##_t*)paddr = val; \
    }

  // store value to memory at aligned address
  store_func(uint8)
  store_func(uint16)
  store_func(uint32)
  store_func(uint64)

  // load instruction from memory at aligned address.
  // (needed because instruction alignment requirement is variable
  // if RVC is supported)
  // returns the instruction at the specified address, given the current
  // RVC mode.  func is set to a pointer to a function that knows how to
  // execute the returned instruction.
  insn_t __attribute__((always_inline)) load_insn(reg_t addr, bool rvc,
                                                  insn_func_t* func)
  {
    insn_t insn;

    #ifdef RISCV_ENABLE_RVC
    if(addr % 4 == 2 && rvc) // fetch across word boundary
    {
      void* addr_lo = translate(addr, false, true);
      insn.bits = *(uint16_t*)addr_lo;

      *func = processor_t::dispatch_table
               [insn.bits % processor_t::DISPATCH_TABLE_SIZE];

      if(!INSN_IS_RVC(insn.bits))
      {
        void* addr_hi = translate(addr+2, false, true);
        insn.bits |= (uint32_t)*(uint16_t*)addr_hi << 16;
      }
    }
    else
    #endif
    {
      reg_t idx = (addr/sizeof(insn_t)) % ICACHE_ENTRIES;
      insn_t data = icache_data[idx];
      *func = icache_func[idx];
      if(likely(icache_tag[idx] == addr))
        return data;

      // the processor guarantees alignment based upon rvc mode
      void* paddr = translate(addr, false, true);
      insn = *(insn_t*)paddr;

      icache_tag[idx] = addr;
      icache_data[idx] = insn;
      icache_func[idx] = *func = processor_t::dispatch_table
                                 [insn.bits % processor_t::DISPATCH_TABLE_SIZE];
    }

    return insn;
  }

  // get the virtual address that caused a fault
  reg_t get_badvaddr() { return badvaddr; }

  // get/set the page table base register
  reg_t get_ptbr() { return ptbr; }
  void set_ptbr(reg_t addr) { ptbr = addr & ~(PGSIZE-1); flush_tlb(); }

  // keep the MMU in sync with processor mode
  void set_supervisor(bool sup) { supervisor = sup; }
  void set_vm_enabled(bool en) { vm_enabled = en; }

  // flush the TLB and instruction cache
  void flush_tlb();
  void flush_icache();

private:
  char* mem;
  size_t memsz;
  reg_t badvaddr;

  reg_t ptbr;
  bool supervisor;
  bool vm_enabled;

  // implement a TLB for simulator performance
  static const reg_t TLB_ENTRIES = 256;
  long tlb_data[TLB_ENTRIES];
  reg_t tlb_insn_tag[TLB_ENTRIES];
  reg_t tlb_load_tag[TLB_ENTRIES];
  reg_t tlb_store_tag[TLB_ENTRIES];

  // implement an instruction cache for simulator performance
  static const reg_t ICACHE_ENTRIES = 256;
  insn_t icache_data[ICACHE_ENTRIES];
  insn_func_t icache_func[ICACHE_ENTRIES];
  reg_t icache_tag[ICACHE_ENTRIES];

  // finish translation on a TLB miss and upate the TLB
  void* refill(reg_t addr, bool store, bool fetch);

  // perform a page table walk for a given virtual address
  pte_t walk(reg_t addr);

  // translate a virtual address to a physical address
  void* translate(reg_t addr, bool store, bool fetch)
  {
    reg_t idx = (addr >> PGSHIFT) % TLB_ENTRIES;

    reg_t* tlb_tag = fetch ? tlb_insn_tag : store ? tlb_store_tag :tlb_load_tag;
    reg_t expected_tag = addr & ~(PGSIZE-1);
    if(likely(tlb_tag[idx] == expected_tag))
      return (void*)(((long)addr & (PGSIZE-1)) | tlb_data[idx]);

    return refill(addr, store, fetch);
  }
  
  friend class processor_t;
};

#endif