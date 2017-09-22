#ifndef __PERF_CLOEXEC_H
#define __PERF_CLOEXEC_H
#if defined(_WIN32)
#elif defined(_SX)
#else // linux ...

unsigned long perf_event_open_cloexec_flag(void);

#ifdef __GLIBC_PREREQ
#if !__GLIBC_PREREQ(2, 6) /*&& !defined(__UCLIBC__)*/
extern int sched_getcpu(void) __THROW;
#endif
#endif

#endif // linux
#endif /* __PERF_CLOEXEC_H */
