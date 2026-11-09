import re,os
import sys
O=sys.argv[1] if len(sys.argv)>1 else '/root/oss'; S=O+'/shim'
os.system('rm -rf %s; mkdir -p %s/mach'%(S,S))
defs=[]
for f in ['osfmk/mach/message.h','osfmk/mach/port.h','osfmk/mach/kern_return.h','osfmk/mach/boolean.h']:
    for l in open(O+'/xnu/'+f,errors='replace'):
        m=re.match(r'#define\s+((?:MACH_|KERN_)[A-Z0-9_]+)\s+(\S.*?)\s*(?:/\*.*)?$',l)
        if m and not m.group(2).endswith('\\') and m.group(2).count('(')==m.group(2).count(')') and '#' not in m.group(2): defs.append((m.group(1),m.group(2),f))
fl=[]
for f in ['osfmk/mach/message.h','osfmk/mach/port.h']:
    txt=open(O+'/xnu/'+f,errors='replace').read().replace('\\\n',' ')
    for l in txt.split('\n'):
        m=re.match(r'#define\s+((?:MACH_)[A-Z0-9_]+\([a-z, _]*\))\s+(\S.*?)\s*(?:/\*.*)?$',l)
        if m and m.group(2).count('(')==m.group(2).count(')') and '#' not in m.group(2): fl.append('#define %s %s'%(m.group(1),m.group(2)))
seen=set(); lines=[]
for n,v,f in defs:
    if n in seen: continue
    seen.add(n); lines.append('#ifndef %s\n#define %s %s\n#endif'%(n,n,v))
hdr='''/* shim for building migcom on FreeBSD: the types and constants of xnu's mach headers that migcom uses */
#ifndef _SHIM_MACH_H
#define _SHIM_MACH_H
#include <stdint.h>
#include <stddef.h>
typedef unsigned int natural_t; typedef int integer_t; typedef int kern_return_t; typedef int boolean_t;
typedef unsigned int mach_port_t, mach_port_name_t, mach_msg_bits_t, mach_msg_size_t, mach_msg_id_t, mach_msg_option_t, mach_msg_timeout_t, mach_msg_type_name_t, mach_msg_type_size_t, mach_msg_type_number_t, mach_msg_return_t, mach_msg_descriptor_type_t, mach_msg_trailer_type_t, mach_msg_trailer_size_t, mach_msg_priority_t;
typedef unsigned int mach_port_right_t; typedef int mach_msg_copy_options_t;
typedef struct { mach_msg_size_t msgh_descriptor_count; } mach_msg_body_t;
typedef struct { mach_msg_bits_t msgh_bits; mach_msg_size_t msgh_size; mach_port_t msgh_remote_port, msgh_local_port; mach_port_name_t msgh_voucher_port; int msgh_id; } mach_msg_header_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
'''
import shutil
shutil.copy(O+'/xnu/osfmk/mach/ndr.h',S+'/mach/ndr.h')
for n in ['message.h','boolean.h','kern_return.h','std_types.h','port.h']:
    open(S+'/mach/'+n,'w').write(hdr+'\n'.join(lines)+'\n'+'\n'.join(fl)+'\n#endif\n' if n=='message.h' else '#include <mach/message.h>\n')
for n in ['mig_errors.h']:
    pass
print(len(lines),'constants')

os.system('mkdir -p %s/libkern'%S)
open(S+'/libkern/OSByteOrder.h','w').write('#ifndef _SHIM_OSBO\n#define _SHIM_OSBO\n#include <sys/endian.h>\n#define OSSwapInt16(x) bswap16(x)\n#define OSSwapInt32(x) bswap32(x)\n#define OSSwapInt64(x) bswap64(x)\n#endif\n')
