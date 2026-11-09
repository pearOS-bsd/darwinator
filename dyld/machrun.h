/* machrun: shared declarations (loader and symbol shims). */
#ifndef _MACHRUN_H_
#define _MACHRUN_H_
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Variadic calls: on arm64 Darwin passes the variable arguments on the stack (not in x1..x7 like the AAPCS64 of FreeBSD), so a shim that takes "..." starts its va_list at the
 * incoming stack pointer (the CFA of the function): a va_list of AAPCS64 with no register save area.  On x86-64 both systems agree. */
#if defined(__aarch64__)
#define	DW_VA_START(ap, last)	do { (ap).__stack = __builtin_dwarf_cfa(); (ap).__gr_top = 0; (ap).__vr_top = 0; (ap).__gr_offs = 0; (ap).__vr_offs = 0; } while (0)
#else
#define	DW_VA_START(ap, last)	va_start(ap, last)
#endif

extern int machrun_verbose;
#define	VLOG(...)	do { if (machrun_verbose) fprintf(stderr, "machrun: " __VA_ARGS__); } while (0)

/* The result of looking a symbol up: the address that goes into the program's pointer, or NULL. */
void	*shim_lookup(void *libhandle, const char *machsym);	/* machsym as in the Mach-O file: "_fopen$DARWIN_EXTSN" */
void	shim_set_process(int argc, char **argv, char **envp, const char *progpath);
void	*shim_open_library(const char *installname);		/* "/usr/lib/libz.1.dylib" -> a dlopen handle (or RTLD_DEFAULT) */

/* what the stubs of unresolved symbols call (set by machrun.c) */
void	machrun_unresolved(const char *const *name);

#endif
