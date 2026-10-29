// Paired x86 GNU interpreter overlay. Only native FEX's guest interpreter is
// redirected; the original game ELF, argv, auxv and /proc/self/exe stay intact.
#pragma once
static const char *SteamOhosGuestInterpreter(const char *path,char mapped[PATH_MAX]) {
    const char *active=getenv("STEAMOHOS_NATIVE_FEX"),*root=getenv("STEAMOHOS_NATIVE_FEX_ROOTFS");
    const char *core=getenv("STEAMOHOS_NATIVE_GUEST_CORE"),*code=getenv("STEAMOHOS_CODE_ROOT");
    if(!path||!active||strcmp(active,"1")||!root||root[0]!='/'||!core||!code||code[0]!='/')return path;
    char expected[PATH_MAX];
    int count=snprintf(expected,sizeof(expected),"%s/native-guest-core-x86_64",code);
    if(count<=0||count>=(int)sizeof(expected)||strcmp(core,expected))return path;
    size_t length=strlen(root);
    if(strncmp(path,root,length)||path[length]!='/')return path;
    const char *relative=path+length,*leaf=NULL;
    const char *prefixes[]={"/lib/x86_64-linux-gnu/","/usr/lib/x86_64-linux-gnu/","/lib64/","/usr/lib64/"};
    for(unsigned i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);++i)
        if(!strncmp(relative,prefixes[i],strlen(prefixes[i])))leaf=relative+strlen(prefixes[i]);
    if(!leaf||strchr(leaf,'/'))return path;
    int matched=!strcmp(leaf,"ld-linux-x86-64.so.2");
    if(!matched&&!strncmp(leaf,"ld-",3)) {
        const char *p=leaf+3;unsigned digits=0;
        while((*p>='0'&&*p<='9')||*p=='.'){digits+=*p!='.';++p;}
        // The final .so begins with the dot already visited above.
        matched=digits&&p>leaf+3&&p[-1]=='.'&&!strcmp(p,"so");
    }
    if(!matched)return path;
    count=snprintf(mapped,PATH_MAX,"%s/ld-linux-x86-64.so.2",core);
    return count>0&&count<PATH_MAX?mapped:path;
}
