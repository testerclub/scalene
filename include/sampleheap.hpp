#ifndef SAMPLEHEAP_H
#define SAMPLEHEAP_H

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> // for getpid()

#include <random>
#include <atomic>

#include <signal.h>

#include "common.hpp"
#include "open_addr_hashtable.hpp"
#include "sampler.hpp"
#include "stprintf.h"
#include "tprintf.h"

#define DISABLE_SIGNALS 0 // For debugging purposes only.

#if DISABLE_SIGNALS
#define raise(x)
#endif


#define USE_ATOMICS 0

#if USE_ATOMICS
typedef std::atomic<uint64_t> counterType;
#else
typedef uint64_t counterType;
#endif

template <unsigned long MallocSamplingRateBytes, class SuperHeap> 
class SampleHeap : public SuperHeap {
public:
  
  enum { Alignment = SuperHeap::Alignment };
  enum AllocSignal { MallocSignal = SIGXCPU, FreeSignal = SIGXFSZ };
  enum { CallStackSamplingRate = MallocSamplingRateBytes / 13 };
  
  SampleHeap()
    : _mallocTriggered (0),
      _freeTriggered (0),
      _pythonCount (0),
      _cCount (0)
  {
    // Ignore these signals until they are replaced by a client.
    signal(MallocSignal, SIG_IGN);
    signal(FreeSignal, SIG_IGN);
    // Fill the 0s with the pid.
    auto pid = getpid();
    stprintf::stprintf(scalene_malloc_signal_filename, "/tmp/scalene-malloc-signal@", pid);
  }

  ~SampleHeap() {
    // Delete the signal log files.
    unlink(scalene_malloc_signal_filename);
  }
  
  ATTRIBUTE_ALWAYS_INLINE inline void * malloc(size_t sz) {
    auto ptr = SuperHeap::malloc(sz);
  
    if (likely(ptr != nullptr)) {
      auto realSize = SuperHeap::getSize(ptr);
      assert(realSize >= sz);
      assert((sz < 16) || (realSize <= 2 * sz));
      auto sampleMalloc = _mallocSampler.sample(realSize);
      auto sampleCallStack = _callStackSampler.sample(realSize);
#if 1
      if (unlikely(sampleCallStack)) {
	recordCallStack(realSize);
      }
#endif
      if (unlikely(sampleMalloc)) {
	writeCount(MallocSignal, sampleMalloc * MallocSamplingRateBytes);
	_pythonCount = 0;
	_cCount = 0;
	_mallocTriggered++;
	raise(MallocSignal);
      }
    }
    return ptr;
  }

  ATTRIBUTE_ALWAYS_INLINE inline void free(void * ptr) {
    if (unlikely(ptr == nullptr)) { return; }
    auto realSize = SuperHeap::free(ptr);
    auto sampleFree = _freeSampler.sample(realSize);
    if (unlikely(sampleFree)) {
      writeCount(FreeSignal, sampleFree * MallocSamplingRateBytes);
      _freeTriggered++;
      raise(FreeSignal);
    }
  }

private:

  Sampler<MallocSamplingRateBytes> _mallocSampler;
  Sampler<MallocSamplingRateBytes> _freeSampler;
  Sampler<CallStackSamplingRate>   _callStackSampler;
  char scalene_malloc_signal_filename[255];
  char scalene_free_signal_filename[255];
  counterType _mallocTriggered;
  counterType _freeTriggered;
  counterType _pythonCount;
  counterType _cCount;

  open_addr_hashtable<65536> _table; // Maps call stack entries to function names.
  
  void recordCallStack(size_t sz) {
    // Walk the stack to see if this memory was allocated by Python
    // through its object allocation APIs.
    const auto MAX_FRAMES_TO_CHECK = 4; // enough to skip past the replacement_malloc
    void * callstack[MAX_FRAMES_TO_CHECK];
    auto frames = backtrace(callstack, MAX_FRAMES_TO_CHECK);
    char * fn_name;
    // tprintf::tprintf("------- @ -------\n", sz);
    for (auto i = 0; i < frames; i++) {
      fn_name = nullptr;

#define USE_HASHTABLE 1
#if !USE_HASHTABLE
      auto v = nullptr;
#else
      auto v = _table.get(callstack[i]);
#endif
      if (v == nullptr) {
	// Not found. Add to table.
	Dl_info info;
	int r = dladdr(callstack[i], &info);
	if (r) {
#if !USE_HASHTABLE
#else
	  _table.put(callstack[i], (void *) info.dli_sname);
#endif
	  fn_name = (char *) info.dli_sname;
	} else {
	  continue;
	}
      } else {
	// Found it.
	fn_name = (char *) v;
      }
      if (!fn_name) {
	continue;
      }
      // tprintf::tprintf("@\n", fn_name);
      if (strlen(fn_name) < 9) { // length of PySet_New
	continue;
      }
      // Starts with Py, assume it's Python calling.
      if (strstr(fn_name, "Py") == &fn_name[0]) {
	//(strstr(fn_name, "PyList_Append") ||
	//   strstr(fn_name, "_From") ||
	//   strstr(fn_name, "_New") ||
	//   strstr(fn_name, "_Copy"))) {
	if (strstr(fn_name, "PyArray_")) {
	  // Make sure we're not in NumPy, which irritatingly exports some functions starting with "Py"...
	  // tprintf::tprintf("--NO---\n");
	  goto C_CODE;
	}
#if 0
	if (strstr(fn_name, "PyEval") || strstr(fn_name, "PyCompile") || strstr(fn_name, "PyImport")) {
	  // Ignore allocations due to interpreter internal operations, for now.
	  goto C_CODE;
	}
#endif
	// tprintf::tprintf("P\n");
	_pythonCount += sz;
	return;
      }
      if (strstr(fn_name, "_Py") == 0) {
	continue;
      }
      if (strstr(fn_name, "_PyCFunction")) {
	goto C_CODE;
      }
#if 1
      _pythonCount += sz;
      return;
#else
      // TBD: realloc requires special handling.
      // * _PyObject_Realloc
      // * _PyMem_Realloc
      if (strstr(fn_name, "New")) {
	// tprintf::tprintf("P\n");
	_pythonCount += sz;
	return;
      }
      if (strstr(fn_name, "_PyObject_") ) {
	if ((strstr(fn_name, "GC_Alloc") ) ||
	    (strstr(fn_name, "GC_New") ) ||
	    (strstr(fn_name, "GC_NewVar") ) ||
	    (strstr(fn_name, "GC_Resize") ) ||
	    (strstr(fn_name, "Malloc") ) ||
	    (strstr(fn_name, "Calloc") ))	      
	  {
	    // tprintf::tprintf("P\n");
	    _pythonCount += sz;
	    return;
	  }
      }
      if (strstr(fn_name, "_PyMem_") ) {
	if ((strstr(fn_name, "Malloc") ) ||
	    (strstr(fn_name, "Calloc") ) ||
	    (strstr(fn_name, "RawMalloc") ) ||
	    (strstr(fn_name, "RawCalloc") ))
	  {
	    // tprintf::tprintf("p\n");
	    _pythonCount += sz;
	    return;
	  }
      }
      tprintf::tprintf("@\n", fn_name);
#endif	  
    }
    
  C_CODE:
    //    tprintf::tprintf("C");
    _cCount += sz;
  }
  
  static constexpr auto flags = O_WRONLY | O_CREAT | O_SYNC | O_APPEND; // O_TRUNC;
  static constexpr auto perms = S_IRUSR | S_IWUSR;

  void writeCount(AllocSignal sig, unsigned long count) {
    char buf[255];
    if (_pythonCount == 0) {
      _pythonCount = 1; // prevent 0/0
    }
    stprintf::stprintf(buf, "@,@,@,@\n", ((sig == MallocSignal) ? 'M' : 'F'), _mallocTriggered + _freeTriggered, count, (float) _pythonCount / (_pythonCount + _cCount));
    //    sprintf(buf, "%c,%llu,%lu,%f\n", ((sig == MallocSignal) ? 'M' : 'F'), _mallocTriggered + _freeTriggered, count, (float) _pythonCount / (_pythonCount + _cCount));
    int fd = open(scalene_malloc_signal_filename, flags, perms);
    write(fd, buf, strlen(buf));
    close(fd);
  }

};

#endif
