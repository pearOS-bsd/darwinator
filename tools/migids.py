# migids.py: the message ids of a MIG subsystem (.defs of xnu), the way mig numbers them: the C preprocessor runs, then every routine, simpleroutine and skip takes one id from the base of the subsystem line.
#   python3 tools/migids.py mach_host.defs   (the files that the .defs include can be empty: put them in ./stub/mach/std_types.defs ... )
import subprocess,re,sys
def ids(path, defs=()):
    out=subprocess.run(['cc','-E','-P','-x','c','-Istub']+['-D'+d for d in defs]+[path],capture_output=True,text=True).stdout
    m=re.search(r'subsystem\s*(?:\w+\s+)*?(\w+)\s+(\d+)\s*;',out,re.S)
    # subsystem [KernelUser|KernelServer] name base;
    base=int(re.search(r'subsystem[^;]*?(\d+)\s*;',out,re.S).group(1))
    body=out[out.index('subsystem'):]
    res={}; n=base
    for st in re.finditer(r'(?:^|;)\s*(simpleroutine|routine|skip)\b\s*([A-Za-z_0-9]*)',body):
        kind,name=st.groups()
        if kind!='skip':
            res[name]=n
        n+=1
    return res
if __name__=='__main__':
    r=ids(sys.argv[1],sys.argv[2:]) 
    for k,v in sorted(r.items(),key=lambda x:x[1]): print(v,k)
