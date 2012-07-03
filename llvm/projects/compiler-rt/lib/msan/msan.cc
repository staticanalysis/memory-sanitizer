#include "msan_interface.h"
#include "msan.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_libc.h"

#include <stdio.h>

#include <interception/interception.h>

#define THREAD_LOCAL __thread

using namespace __sanitizer;

DECLARE_REAL(int, posix_memalign, void **memptr, size_t alignment, size_t size);
DECLARE_REAL(void*, memset, void *s, int c, size_t n);
DECLARE_REAL(void*, memcpy, void* dest, const void* src, size_t n);
DECLARE_REAL(void, free, void *ptr);


// Globals.
static int msan_exit_code = 67;
static int msan_poison_in_malloc = 1;
static THREAD_LOCAL int msan_expect_umr = 0;
static THREAD_LOCAL int msan_expected_umr_found = 0;

static int msan_running_under_pin = 0;
THREAD_LOCAL long long __msan_param_tls[100];
THREAD_LOCAL long long __msan_retval_tls[8];
static long long *main_thread_param_tls;

static const int kMsanMallocMagic = 0xCA4D;


static bool IsRunningUnderPin() {
  return internal_strstr(__msan::GetProcSelfMaps(), "/pinbin") != 0;
}

void __msan_warning() {
  if (msan_expect_umr) {
    // Printf("Expected UMR\n");
    msan_expected_umr_found = 1;
    return;
  }
  Printf("***UMR***\n");
  __msan::GdbBackTrace();
  if (msan_exit_code >= 0) {
    Printf("Exiting\n");
    Die();
  }
}

namespace __msan {
int msan_inited = 0;

void *MsanReallocate(void *oldp, uptr size, uptr alignment, bool zeroise) {
  __msan_init();
  CHECK(msan_inited);
  uptr extra_bytes = 2 * sizeof(u64*);
  if (alignment > extra_bytes)
    extra_bytes = alignment;
  uptr old_size = 0;
  void *real_oldp = 0;
  if (oldp) {
    char *beg = (char*)(oldp);
    u64 *p = (u64 *)beg;
    old_size = p[-2] >> 16;
    CHECK((p[-2] & 0xffffULL) == kMsanMallocMagic);
    char *end = beg + size;
    real_oldp = (void*)p[-1];
  }
  void *mem = NULL;
  int res = REAL(posix_memalign)(&mem, alignment, size + extra_bytes);
  if (res == 0) {
    char *beg = (char*)mem + extra_bytes;
    char *end = beg + size;
    u64 *p = (u64 *)beg;
    p[-2] = (size << 16) | kMsanMallocMagic;
    p[-1] = (u64)mem;
    // Printf("MSAN POISONS on malloc [%p, %p) rbeg: %p\n", beg, end, mem);
    if (zeroise) {
      REAL(memset)(beg, 0, size);
    } else {
      if (msan_poison_in_malloc)
        __msan_poison(beg, end - beg);
    }
    mem = beg;
  }

  if (old_size) {
    uptr min_size = size < old_size ? size : old_size;
    if (mem) {
      REAL(memcpy)(mem, oldp, min_size);
      __msan_copy_poison(mem, oldp, min_size);
    }
    __msan_unpoison(oldp, old_size);
    REAL(free(real_oldp));
  }
  return mem;
}

void MsanDeallocate(void *ptr) {
  __msan_init();
  char *beg = (char*)(ptr);
  u64 *p = (u64 *)beg;
  uptr size = p[-2] >> 16;
  CHECK((p[-2] & 0xffffULL) == kMsanMallocMagic);
  char *end = beg + size;
  // Printf("MSAN UNPOISONS on free [%p, %p)\n", beg, end);
  __msan_unpoison(beg, end - beg);
  REAL(free((void*)p[-1]));
}



}  // namespace __msan


extern "C"
void __msan_init() {
  using namespace __msan;
  if (msan_inited) return;
  main_thread_param_tls = __msan_param_tls;
  msan_running_under_pin = IsRunningUnderPin();
  // Must call it here for PIN to intercept it.
  __msan_clear_on_return();
  if (!msan_running_under_pin) {
    if (!InitShadow(true, true, true)) {
      Printf("FATAL: MemorySanitizer can not mmap the shadow memory\n");
      Printf("FATAL: Make sure to compile with -fPIE and to link with -pie.\n");
      CatProcSelfMaps();
      Die();
    }
  }
  __msan::InitializeInterceptors();
  msan_inited = 1;
  // Printf("MemorySanitizer init done\n");
}

// Interface.

void __msan_unpoison(void *a, uptr size) {
  REAL(memset)((void*)MEM_TO_SHADOW((uptr)a), 0, size);
}
void __msan_poison(void *a, uptr size) {
  REAL(memset)((void*)MEM_TO_SHADOW((uptr)a), -1, size);
}

void __msan_copy_poison(void *dst, const void *src, uptr size) {
  REAL(memcpy)((void*)MEM_TO_SHADOW((uptr)dst),
         (void*)MEM_TO_SHADOW((uptr)src), size);
}

void __msan_set_exit_code(int exit_code) {
  msan_exit_code = exit_code;
}
void __msan_set_expect_umr(int expect_umr) {
  if (expect_umr) {
    msan_expected_umr_found = 0;
  } else if (!msan_expected_umr_found) {
    Printf("Expected UMR not found\n");
    __msan::GdbBackTrace();
    Die();
  }
  msan_expect_umr = expect_umr;
}

void __msan_print_shadow(void *x, int size) {
  unsigned char *s = (unsigned char*)MEM_TO_SHADOW((uptr)x);
  for (uptr i = 0; i < (uptr)size; i++) {
    Printf("%02x ", s[i]);
  }
  Printf("\n");
}

void __msan_print_param_shadow() {
  for (int i = 0; i < 4; i++) {
    Printf("%016llx ", __msan_param_tls[i]);
  }
  Printf("\n");
}


int __msan_set_poison_in_malloc(int do_poison) {
  int old = msan_poison_in_malloc;
  msan_poison_in_malloc = do_poison;
  return old;
}

void __msan_break_optimization(void *x) { }

int  __msan_has_dynamic_component() {
  return msan_running_under_pin;
}

NOINLINE
void __msan_clear_on_return() {
  __msan_param_tls[0] = 0;
}

#include "msan_linux_inl.h"