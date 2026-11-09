/* probe5: checks the MIG message ids that darwinator implements (computed from the .defs of xnu with tools/migids.py) against the user stubs of the real libSystem: the id is an
 * immediate value in the code of the stub (msgh_id = id), so the 32 bit number is looked for in the first 4000 bytes of every function.  (Sending the messages by hand does not work on
 * a recent macOS: a process that does it is killed with EXC_GUARD.)   Build: cc -o probe5 probe5.c   Run: ./probe5 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

static const struct { const char *fn; int id; } want[] = {
	{ "host_info", 200 }, { "host_kernel_version", 201 }, { "_host_page_size", 202 }, { "host_statistics", 216 }, { "host_statistics64", 219 },
	{ "mach_port_names", 3200 }, { "mach_port_type", 3201 }, { "mach_port_allocate", 3204 }, { "mach_port_destroy", 3205 }, { "mach_port_deallocate", 3206 },
	{ "mach_port_get_refs", 3207 }, { "mach_port_mod_refs", 3208 }, { "mach_port_set_mscount", 3210 }, { "mach_port_request_notification", 3213 },
	{ "mach_port_get_attributes", 3217 }, { "mach_port_get_srights", 3222 }, { "mach_port_get_context", 3228 }, { "mach_port_set_context", 3229 },
	{ "task_threads", 3402 }, { "mach_ports_register", 3403 }, { "mach_ports_lookup", 3404 }, { "task_info", 3405 }, { "task_set_info", 3406 },
	{ "task_suspend", 3407 }, { "task_resume", 3408 }, { "task_get_special_port", 3409 }, { "task_set_special_port", 3410 }, { "semaphore_create", 3418 },
	{ "thread_info", 3612 }, { "thread_get_state", 3603 }, { "thread_suspend", 3605 }, { "thread_resume", 3606 },
	{ "mach_vm_read", 4804 }, { "mach_vm_write", 4806 }, { "mach_vm_read_overwrite", 4808 }, { "mach_vm_region", 4816 },
};

int main(void)
{
	size_t i;
	int bad = 0;

	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		unsigned char *f = NULL;
		int found = 0; size_t k, pass;
		char name[96];
		const char *tried = want[i].fn;
		uint32_t id = (uint32_t)want[i].id;

		for (pass = 0; pass < 2 && !found; pass++) {		/* the plain name, then the stub with the prefix that the .defs give (_kernelrpc_) */
			snprintf(name, sizeof name, pass == 0 ? "%s" : "_kernelrpc_%s", want[i].fn);
			f = dlsym(RTLD_DEFAULT, name);
			if (f == NULL) continue;
			tried = pass == 0 ? want[i].fn : name;
			for (k = 0; k < 4000 && !found; k++)
				if (memcmp(f + k, &id, 4) == 0) found = 1;
		}
		printf("%-34s %-5d %s%s\n", want[i].fn, want[i].id, found ? "id found in the stub" : f == NULL ? "not exported" : "NOT FOUND in the first 4000 bytes", found && tried != want[i].fn ? " (as _kernelrpc_)" : "");
		if (!found) bad++;
	}
	printf("%d of %zu not found\n", bad, sizeof(want) / sizeof(want[0]));
	return 0;
}
