#include <tock.h>

#if defined(STACK_SIZE)
#warning Attempt to compile libtock with a fixed STACK_SIZE.
#warning
#warning Instead, STACK_SIZE should be a variable that is linked in,
#warning usually at compile time via something like this:
#warning   `gcc ... -Xlinker --defsym=STACK_SIZE=2048`
#warning
#warning This allows applications to set their own STACK_SIZE.
#error Fixed STACK_SIZE.
#endif

extern int main(void);

// Allow _start to go undeclared
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

/*
 * The structure populated by the linker script at the very beginning of the
 * text segment. It represents sizes and offsets from the text segment of
 * sections that need some sort of loading and/or relocation.
 */
struct hdr {
  //  0: Offset of GOT symbols in flash
  uint32_t got_sym_start;
  //  4: Offset of GOT section in memory
  uint32_t got_start;
  //  8: Size of GOT section
  uint32_t got_size;
  // 12: Offset of data symbols in flash
  uint32_t data_sym_start;
  // 16: Offset of data section in memory
  uint32_t data_start;
  // 20: Size of data section
  uint32_t data_size;
  // 24: Offset of BSS section in memory
  uint32_t bss_start;
  // 28: Size of BSS section
  uint32_t bss_size;
  // 32: First address offset after program flash, where elf2tab places
  //     .rel.data section
  uint32_t reldata_start;
  // 36: The size of the stack requested by this application
  uint32_t stack_size;
};

struct reldata {
  uint32_t len;
  uint32_t data[];
};

__attribute__ ((section(".start"), used))
__attribute__ ((weak))
__attribute__ ((naked))
__attribute__ ((noreturn))
void _start(void* app_start __attribute__((unused)),
            void* mem_start __attribute__((unused)),
            void* memory_len __attribute__((unused)),
            void* app_heap_break __attribute__((unused))) {
#if defined(__thumb__)
  // Assembly written to adhere to any modern thumb arch

  // Allocate stack and data. `brk` to stack_size + got_size + data_size +
  // bss_size from start of memory. Also make sure that the stack starts on an
  // 8 byte boundary per section 5.2.1.2 here:
  // http://infocenter.arm.com/help/topic/com.arm.doc.ihi0042f/IHI0042F_aapcs.pdf

  asm volatile (
    // Compute the stack top
    //
    // struct hdr* myhdr = (struct hdr*)app_start;
    // uint32_t stacktop = (((uint32_t)mem_start + myhdr->stack_size + 7) & 0xfffffff8);
    "ldr  r4, [r0, #36]\n"      // r4 = myhdr->stack_size
    "add  r4, #7\n"             // r4 = myhdr->stack_size + 7
    "add  r4, r4, r1\n"         // r4 = mem_start + myhdr->stack_size + 7
    "movs r5, #7\n"
    "bic  r4, r4, r5\n"         // r4 = (mem_start + myhdr->stack_size + 7) & ~0x7
    //
    // Compute the app data size and where initial app brk should go.
    // This includes the GOT, data, and BSS sections. However, we can't be sure
    // the linker puts them back-to-back, but we do assume that BSS is last
    // (i.e. myhdr->got_start < myhdr->bss_start && myhdr->data_start <
    // myhdr->bss_start). With all of that true, then the size is equivalent
    // to the end of the BSS section.
    //
    // uint32_t appdata_size = myhdr->bss_start + myhdr->bss_size;
    "ldr  r5, [r0, #24]\n"      // r6 = myhdr->bss_start
    "ldr  r6, [r0, #28]\n"      // r6 = myhdr->bss_size
    "add  r5, r5, r6\n"         // r5 = bss_start + bss_size
    //
    // Move registers we need to keep over to callee-saved locations
    "movs r6, r0\n"
    "movs r7, r1\n"

    // Setup initial stack pointer for normal execution
    "mov  sp, r4\n"
    "mov  r9, sp\n"

    //
    // Call `brk` to set to requested memory
    //
    // memop(0, stacktop + appdata_size);
    "movs r0, #0\n"
    "add  r1, r4, r5\n"
    "svc 4\n"                   // memop
    //
    // Debug support, tell the kernel the stack location
    //
    // memop(10, stacktop);
    "movs r0, #10\n"
    "movs r1, r4\n"
    "svc 4\n"                   // memop
    //
    // Debug support, tell the kernel the heap location
    //
    // memop(11, stacktop + appdata_size);
    "movs r0, #11\n"
    "add  r1, r4, r5\n"
    "svc 4\n"                   // memop
    //

    //
    // Call into the rest of startup.
    // This should never return, if it does, trigger a breakpoint (which will
    // promote to a HardFault in the absence of a debugger)
    "movs r0, r6\n"             // first arg is app_start
    "movs r1, r4\n"             // second arg is stacktop
    "bl _c_start\n"
    "bkpt #255\n"
    );
#else
#error Missing initial stack setup trampoline for current arch.
#endif
}

__attribute__((noreturn))
void _c_start(uint32_t* app_start, uint32_t stacktop) {
  struct hdr* myhdr = (struct hdr*)app_start;

  // fix up GOT
  volatile uint32_t* got_start     = (uint32_t*)(myhdr->got_start + stacktop);
  volatile uint32_t* got_sym_start = (uint32_t*)(myhdr->got_sym_start + (uint32_t)app_start);
  for (uint32_t i = 0; i < (myhdr->got_size / (uint32_t)sizeof(uint32_t)); i++) {
    if ((got_sym_start[i] & 0x80000000) == 0) {
      got_start[i] = got_sym_start[i] + stacktop;
    } else {
      got_start[i] = (got_sym_start[i] ^ 0x80000000) + (uint32_t)app_start;
    }
  }

  // load data section
  void* data_start     = (void*)(myhdr->data_start + stacktop);
  void* data_sym_start = (void*)(myhdr->data_sym_start + (uint32_t)app_start);
  memcpy(data_start, data_sym_start, myhdr->data_size);

  // zero BSS
  char* bss_start = (char*)(myhdr->bss_start + stacktop);
  memset(bss_start, 0, myhdr->bss_size);

  struct reldata* rd = (struct reldata*)(myhdr->reldata_start + (uint32_t)app_start);
  for (uint32_t i = 0; i < (rd->len / (int)sizeof(uint32_t)); i += 2) {
    uint32_t* target = (uint32_t*)(rd->data[i] + stacktop);
    if ((*target & 0x80000000) == 0) {
      *target += stacktop;
    } else {
      *target = (*target ^ 0x80000000) + (uint32_t)app_start;
    }
  }

  main();
  while (1) {
    yield();
  }
}

