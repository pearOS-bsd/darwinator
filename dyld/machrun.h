/* machrun: shared declarations (loader and symbol shims). */
#ifndef _MACHRUN_H_
#define _MACHRUN_H_
#include <stdint.h>
#include <stddef.h>

extern int machrun_verbose;
#define	VLOG(...)	do { if (machrun_verbose) fprintf(stderr, "machrun: " __VA_ARGS__); } while (0)

/* The result of looking a symbol up: the address that goes into the program's pointer, or NULL. */
void	*shim_lookup(void *libhandle, const char *machsym);	/* machsym as in the Mach-O file: "_fopen$DARWIN_EXTSN" */
void	shim_set_process(int argc, char **argv, char **envp, const char *progpath);
void	*shim_open_library(const char *installname);		/* "/usr/lib/libz.1.dylib" -> a dlopen handle (or RTLD_DEFAULT) */

/* what the stubs of unresolved symbols call (set by machrun.c) */
void	machrun_unresolved(const char *const *name);

#endif
