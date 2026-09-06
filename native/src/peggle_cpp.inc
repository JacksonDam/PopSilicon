#include <wchar.h>
#include "peggle_tree.inc"
#include "peggle_cxx.inc"
/* GCC 4 i386 COW string ABI used by PopCap's Mac framework. */
static uint32_t pg_string_set(uint32_t obj, const void *src, size_t n, size_t width, bool init)
{
    uint32_t old = init ? 0 : *(uint32_t *)(uintptr_t)obj;
    uint32_t rep = guest_allocate_at(12 + (n + 1) * width, true, kGuestHeapStringSite);
    if (!rep) return 0;
    uint32_t *r = (void *)(uintptr_t)rep;
    r[0] = r[1] = (uint32_t)n;
    if (src && n) memcpy(r + 3, src, n * width);
    *(uint32_t *)(uintptr_t)obj = rep + 12;
    if (old >= 12) guest_string_dispose_rep(old - 12);
    return obj;
}
static struct { uint32_t obj; uint32_t text; } pg_streams[64];
static int peggle_cpp_dispatch(const char *name, const uint32_t *a, uint64_t *out)
{
#define PG_RETURN(x) do { *out = (uint64_t)(x); return 1; } while (0)
    if (!lp32_title_is_peggle_engine(lp32_profile()->title)) return 0;
    static int trace_cpp = -1;
    if (trace_cpp < 0) trace_cpp = getenv("LP32_TRACE_CPP") ? 1 : 0;
    if (trace_cpp) { fprintf(stderr, "compat32: cpp %s a0=%08x a1=%08x a2=%08x\n", name, a[0], a[1], a[2]); }
    /* std::copy(reverse_iterator first, reverse_iterator last, wchar_t* dest):
       EncodingParser::PutString copies a wstring in reverse into a vector<wchar_t>.
       The iterators (single "current" pointers) are passed by address; operator*
       reads *(current-1), operator++ decrements current. */
    if (strncmp(name,"__ZSt4copyISt16reverse_iterator",31)==0) {
        uint32_t first=*(uint32_t *)(uintptr_t)a[0];
        uint32_t last=*(uint32_t *)(uintptr_t)a[1];
        uint32_t dst=a[2];
        while (first!=last) { first-=4; *(uint32_t *)(uintptr_t)dst=*(uint32_t *)(uintptr_t)first; dst+=4; }
        PG_RETURN(dst);
    }
    /* std::__detail::_List_node_base::transfer(first, last): splice the guest
       list nodes [first, last) in front of `this`.  Node layout on i386 is
       { _M_next @0; _M_prev @4 }; replicate libstdc++'s pointer surgery on the
       guest's own nodes (all pointers are guest 32-bit addresses). */
    if (strcmp(name,"__ZNSt15_List_node_base8transferEPS_S0_")==0 ||
        strcmp(name,"__ZNSt8__detail15_List_node_base9_M_transferEPS0_S1_")==0) {
        uint32_t self=a[0], first=a[1], last=a[2];
        if (self!=last) {
#define PG_LNEXT(n) (*(uint32_t *)(uintptr_t)((n)+0))
#define PG_LPREV(n) (*(uint32_t *)(uintptr_t)((n)+4))
            PG_LNEXT(PG_LPREV(last))  = self;
            PG_LNEXT(PG_LPREV(first)) = last;
            PG_LNEXT(PG_LPREV(self))  = first;
            uint32_t tmp = PG_LPREV(self);
            PG_LPREV(self)  = PG_LPREV(last);
            PG_LPREV(last)  = PG_LPREV(first);
            PG_LPREV(first) = tmp;
#undef PG_LNEXT
#undef PG_LPREV
        }
        PG_RETURN(0);
    }
    if (peggle_tree_dispatch(name, a, out)) return 1;
    if (peggle_cxx_dispatch(name, a, out)) return 1;
    if (strncmp(name,"__ZNSaI",7)==0) PG_RETURN(0); /* empty allocator */
    if (strcmp(name,"__ZNSt6localeC1EPKc")==0) { *(uint32_t *)(uintptr_t)a[0]=0; PG_RETURN(a[0]); }
    if (strcmp(name,"__ZNSt6localeC1ERKS_")==0) { *(uint32_t *)(uintptr_t)a[0]=*(uint32_t *)(uintptr_t)a[1]; PG_RETURN(a[0]); }
    if (strcmp(name,"__ZNSt6localeC1Ev")==0) { *(uint32_t *)(uintptr_t)a[0]=0; PG_RETURN(a[0]); }
    if (strcmp(name,"__ZNSt6localeaSERKS_")==0) { *(uint32_t *)(uintptr_t)a[0]=*(uint32_t *)(uintptr_t)a[1]; PG_RETURN(a[0]); }
    if (strcmp(name,"__ZNSt6localeD1Ev")==0) PG_RETURN(0);
    if (strcmp(name,"__ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale")==0) {
        static uint32_t facet; if(!facet){facet=compat_runtime32_allocate(64,1);uint32_t v=compat_runtime32_allocate(64,1);*(uint32_t *)(uintptr_t)facet=v;for(int i=0;i<16;i++)((uint32_t *)(uintptr_t)v)[i]=compat_runtime32_guest_callback("_pg_decimal_point");} PG_RETURN(facet);
    }
    /* numpunct<wchar_t> (Bejeweled 3): same fake facet; L'.' is also 0x2e. */
    if (strcmp(name,"__ZSt9use_facetISt8numpunctIwEERKT_RKSt6locale")==0) {
        static uint32_t wfacet; if(!wfacet){wfacet=compat_runtime32_allocate(64,1);uint32_t v=compat_runtime32_allocate(64,1);*(uint32_t *)(uintptr_t)wfacet=v;for(int i=0;i<16;i++)((uint32_t *)(uintptr_t)v)[i]=compat_runtime32_guest_callback("_pg_decimal_point");} PG_RETURN(wfacet);
    }
    if (strcmp(name,"_pg_decimal_point")==0) PG_RETURN('.');
    /* ctype<char>/ctype<wchar_t> (Bejeweled 3 number/string streams).  A crude
       facet whose every vtable slot is an identity callback: widen(c)->c,
       narrow(c,dflt)->c, toupper/tolower->c.  Enough for ASCII stream I/O. */
    if (strcmp(name,"__ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale")==0 ||
        strcmp(name,"__ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale")==0) {
        static uint32_t cfacet; if(!cfacet){cfacet=compat_runtime32_allocate(64,1);uint32_t v=compat_runtime32_allocate(64,1);*(uint32_t *)(uintptr_t)cfacet=v;for(int i=0;i<16;i++)((uint32_t *)(uintptr_t)v)[i]=compat_runtime32_guest_callback("_pg_ctype_identity");} PG_RETURN(cfacet);
    }
    if (strcmp(name,"_pg_ctype_identity")==0) PG_RETURN(a[0]);
    if (strstr(name,"basic_stringstream") || strstr(name,"basic_ostringstream") || strstr(name,"basic_stringbuf")) {
        bool init = strstr(name,"C1E") != NULL, destroy = strstr(name,"D1E") != NULL;
        bool stringbuf = strstr(name,"basic_stringbuf") != NULL;
        uint32_t obj = (init || destroy) ? a[0] : a[1];
        int slot=-1;
        for(int i=0;i<64;i++) if(pg_streams[i].obj && obj>=pg_streams[i].obj && obj<pg_streams[i].obj+32){slot=i;break;}
        if(init){for(int i=0;i<64;i++)if(!pg_streams[i].obj){slot=i;break;}if(slot<0)abort();pg_streams[slot].obj=obj;pg_streams[slot].text=compat_runtime32_allocate(4,1);pg_string_set(pg_streams[slot].text,NULL,0,1,true);PG_RETURN(obj);}
        if(slot<0 && stringbuf) return 0;
        if(destroy){if(slot>=0){guest_string_dispose_object(pg_streams[slot].text);compat_runtime32_deallocate(pg_streams[slot].text);pg_streams[slot].obj=0;}PG_RETURN(0);}
        if(strstr(name,"3strEv")){compat_runtime32_struct_return();PG_RETURN(pg_string_set(a[0],slot>=0?guest_string_data(pg_streams[slot].text):"",slot>=0?guest_string_length(pg_streams[slot].text):0,1,true));}
        /* str(const string&) setter: this is a[0], the replacement string a[1]. */
        if(strstr(name,"3strERK")){int s2=-1;for(int i=0;i<64;i++)if(pg_streams[i].obj && a[0]>=pg_streams[i].obj && a[0]<pg_streams[i].obj+32){s2=i;break;}if(s2>=0)pg_string_set(pg_streams[s2].text,guest_string_data(a[1]),guest_string_length(a[1]),1,false);PG_RETURN(0);}
    }
    if (strcmp(name,"__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc")==0 || strcmp(name,"__ZNSolsEi")==0) {
        char number[64];const char *value=(void *)(uintptr_t)a[1];if(strcmp(name,"__ZNSolsEi")==0){snprintf(number,sizeof(number),"%d",(int)a[1]);value=number;}
        for(int i=0;i<64;i++)if(pg_streams[i].obj && a[0]>=pg_streams[i].obj && a[0]<pg_streams[i].obj+32){uint32_t obj=pg_streams[i].text;size_t n=guest_string_length(obj),v=strlen(value);char *b=malloc(n+v+1);memcpy(b,guest_string_data(obj),n);memcpy(b+n,value,v+1);pg_string_set(obj,b,n+v,1,false);free(b);PG_RETURN(a[0]);}
        fputs(value,stdout);PG_RETURN(a[0]);
    }
    /* operator<<(ostream&, const std::string&): append the guest string's bytes
       (which may contain NULs) to the stream buffer. */
    if (strcmp(name,"__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E")==0) {
        const char *value=guest_string_data(a[1]);size_t v=guest_string_length(a[1]);
        for(int i=0;i<64;i++)if(pg_streams[i].obj && a[0]>=pg_streams[i].obj && a[0]<pg_streams[i].obj+32){uint32_t obj=pg_streams[i].text;size_t n=guest_string_length(obj);char *b=malloc(n+v);memcpy(b,guest_string_data(obj),n);memcpy(b+n,value,v);pg_string_set(obj,b,n+v,1,false);free(b);PG_RETURN(a[0]);}
        if(value)fwrite(value,1,v,stdout);PG_RETURN(a[0]);
    }
    /* operator<<(ostream&, manipulator): std::endl/std::flush.  Treat as endl
       (append a newline); std::flush would add nothing but is rare on the
       string streams the game builds. */
    if (strcmp(name,"__ZNSolsEPFRSoS_E")==0) {
        for(int i=0;i<64;i++)if(pg_streams[i].obj && a[0]>=pg_streams[i].obj && a[0]<pg_streams[i].obj+32){uint32_t obj=pg_streams[i].text;size_t n=guest_string_length(obj);char *b=malloc(n+1);memcpy(b,guest_string_data(obj),n);b[n]='\n';pg_string_set(obj,b,n+1,1,false);free(b);PG_RETURN(a[0]);}
        PG_RETURN(a[0]);
    }
    bool wide = strstr(name,"SbIwSt11char_traitsIwESaIwEE") != NULL;
    bool str = wide || strncmp(name,"__ZNSs",6)==0 || strncmp(name,"__ZNKSs",7)==0;
    if (str) {
        const char *op = wide ? strstr(name,"SbIwSt11char_traitsIwESaIwEE")+strlen("SbIwSt11char_traitsIwESaIwEE") : name+(name[4]=='K'?7:6);
        size_t w = wide ? 4 : 1;
        uint32_t obj=a[0];
        if (strcmp(op,"C1Ev")==0 || strcmp(op,"C2Ev")==0) PG_RETURN(pg_string_set(obj,NULL,0,w,true));
        if (strcmp(op,"D1Ev")==0 || strcmp(op,"D2Ev")==0) {guest_string_dispose_object(obj);PG_RETURN(0);}
        if (strncmp(op,"C1EPK",5)==0 || strncmp(op,"C2EPK",5)==0) {
            /* basic_string(const CharT* s, [size_type n,] const Alloc&): the
               counted form ("...EPKwmRKS1_" / "...EPKcmRKSaIcE") must copy
               exactly n units -- the source need not be NUL-terminated (a
               vector<wchar_t> buffer, a substring).  Measuring it with
               wcslen/strlen instead dragged heap/stack garbage into the
               string's tail; Bejeweled 3 then fed those "characters" to its
               glyph cache every frame, which grew without bound and made the
               game slow down steadily. */
            const void *s=(void *)(uintptr_t)a[1];
            bool counted=(op[5]=='w'||op[5]=='c') && op[6]=='m';
            size_t len=counted?a[2]:(wide?wcslen(s):strlen(s));
            PG_RETURN(pg_string_set(obj,s,len,w,true));
        }
        if (strncmp(op,"C1ERK",5)==0) PG_RETURN(pg_string_set(obj,guest_string_data(a[1]),guest_string_length(a[1]),w,true));
        if (strcmp(op,"C1EmcRKSaIcE")==0) {char *s=malloc(a[1]);memset(s,a[2],a[1]);pg_string_set(obj,s,a[1],1,true);free(s);PG_RETURN(obj);}
        /* Wide fill ctor: basic_string(size_type n, wchar_t c, allocator) --
           n copies of the wide character c (Bejeweled 3's keyDown: builds a
           one-character wstring from the typed key). */
        if (strcmp(op,"C1EmwRKS1_")==0 || strcmp(op,"C2EmwRKS1_")==0) {
            size_t len=a[1]; uint32_t ch=a[2];
            char *s=malloc((len+1)*w);
            for(size_t i=0;i<len;i++) memcpy(s+i*w,&ch,w);
            pg_string_set(obj,s,len,w,true); free(s); PG_RETURN(obj);
        }
        if (strcmp(op,"6substrEmm")==0) { /* nontrivial class result: hidden output precedes this */
            compat_runtime32_struct_return();const char *src=guest_string_data(a[1]);size_t len_src=guest_string_length(a[1]);size_t pos=a[2]<len_src?a[2]:len_src,len=a[3]<len_src-pos?a[3]:len_src-pos;PG_RETURN(pg_string_set(a[0],src+pos*w,len,w,true));
        }
        /* reverse_iterator rbegin()/rend(): the iterator is a single pointer
           ("current"), returned via sret (a[0]=out, a[1]=this).  current for
           rbegin() is end() (data+n), for rend() is begin() (data); operator*
           yields *(current-1). */
        if (strcmp(op,"6rbeginEv")==0 || strcmp(op,"4rendEv")==0) {
            compat_runtime32_struct_return();
            uint32_t data=*(uint32_t *)(uintptr_t)a[1];
            uint32_t nn=(uint32_t)guest_string_length(a[1]);
            *(uint32_t *)(uintptr_t)a[0]=(op[0]=='6')?data+nn*w:data;
            PG_RETURN(a[0]);
        }
        const char *s=guest_string_data(obj);size_t n=guest_string_length(obj);
        if (strcmp(op,"4sizeEv")==0 || strcmp(op,"6lengthEv")==0) PG_RETURN(n);
        if (strcmp(op,"5emptyEv")==0) PG_RETURN(n==0);
        if (strcmp(op,"5c_strEv")==0 || strcmp(op,"5beginEv")==0) PG_RETURN(*(uint32_t *)(uintptr_t)obj);
        if (strcmp(op,"3endEv")==0) PG_RETURN(*(uint32_t *)(uintptr_t)obj+n*w);
        if (strcmp(op,"ixEm")==0) PG_RETURN(*(uint32_t *)(uintptr_t)obj+a[1]*w);
        if (strcmp(op,"7reserveEm")==0) { /* Ensure inline writes see the advertised capacity. */
            size_t cap=a[1]>n?a[1]:n;uint32_t rep=guest_allocate_at(12+(cap+1)*w,true,kGuestHeapStringSite);uint32_t *r=(void *)(uintptr_t)rep;r[0]=(uint32_t)n;r[1]=(uint32_t)cap;memcpy(r+3,s,n*w);uint32_t old=*(uint32_t *)(uintptr_t)obj;*(uint32_t *)(uintptr_t)obj=rep+12;if(old>=12)guest_string_dispose_rep(old-12);PG_RETURN(0);
        }
        if (strcmp(op,"5clearEv")==0) PG_RETURN(pg_string_set(obj,NULL,0,w,false));
        if (strncmp(op,"aSEPK",5)==0 || strncmp(op,"6assignEPK",10)==0) {
            const void *v=(void *)(uintptr_t)a[1];
            bool counted=strstr(op,"PKcm")!=NULL||strstr(op,"PKwm")!=NULL;
            PG_RETURN(pg_string_set(obj,v,counted?a[2]:(wide?wcslen(v):strlen(v)),w,false));
        }
        if (strncmp(op,"aSERK",5)==0 || strncmp(op,"6assignERK",10)==0) PG_RETURN(pg_string_set(obj,guest_string_data(a[1]),guest_string_length(a[1]),w,false));
        if (strcmp(op,"aSEc")==0) {char c=a[1];PG_RETURN(pg_string_set(obj,&c,1,1,false));}
        if (strncmp(op,"6assignEm",9)==0) {size_t len=a[1];char *v=malloc((len+1)*w);for(size_t i=0;i<len;i++)memcpy(v+i*w,&a[2],w);pg_string_set(obj,v,len,w,false);free(v);PG_RETURN(obj);}
        if (strncmp(op,"6append",7)==0 || strncmp(op,"pLE",3)==0 || strncmp(op,"9push_backE",11)==0) {
            const void *v=NULL;size_t len=0;bool repeat=false;uint32_t ch=a[1];
            if(strstr(op,"EPK")){v=(void *)(uintptr_t)a[1];len=(strstr(op,"PKcm")||strstr(op,"PKwm"))?a[2]:(wide?wcslen(v):strlen(v));}
            else if(strstr(op,"ERK")){v=guest_string_data(a[1]);len=guest_string_length(a[1]);}
            else if(strstr(op,"Emc")){len=a[1];ch=a[2];repeat=true;}
            else {v=&ch;len=1;}
            char *buf=malloc((n+len+1)*w);memcpy(buf,s,n*w);if(repeat)memset(buf+n*w,ch,len);else memcpy(buf+n*w,v,len*w);pg_string_set(obj,buf,n+len,w,false);free(buf);PG_RETURN(obj);
        }
        if (strncmp(op,"6resizeEm",9)==0) {size_t len=a[1];char *buf=calloc(len+1,w);memcpy(buf,s,(n<len?n:len)*w);if(len>n && strstr(op,"Emc"))memset(buf+n,a[2],len-n);pg_string_set(obj,buf,len,w,false);free(buf);PG_RETURN(0);}
        if (strncmp(op,"7compareE",9)==0) {const void *v=strstr(op,"EPK")?(void *)(uintptr_t)a[1]:guest_string_data(a[1]);PG_RETURN((uint32_t)(wide?wcscmp((void *)s,v):strcmp(s,v)));}
        if (strncmp(op,"4findE",6)==0 || strncmp(op,"5rfindE",7)==0 || strstr(op,"find_first") || strstr(op,"find_last")) {
            bool reverse=strstr(op,"rfind")||strstr(op,"find_last");bool set=strstr(op,"_ofE")!=NULL || strstr(op,"_not_ofE")!=NULL;bool neg=strstr(op,"not_of")!=NULL;bool character=strstr(op,"Ecm")||strstr(op,"Ewm");
            const char *v=character?(void *)&a[1]:(strstr(op,"ERK")?guest_string_data(a[1]):(void *)(uintptr_t)a[1]);size_t vn=character?1:(wide?wcslen((void *)v):strlen(v));size_t pos=a[2];uint32_t found=UINT32_MAX;
            if(reverse){if(pos>=n)pos=n?n-1:0;} else if(pos>n)PG_RETURN(found);
            for(size_t step=0;step<=n;step++){size_t i=reverse?pos-step:pos+step;if(i>n || (reverse&&step>pos))break;bool ok=false;if(set){bool hit=false;for(size_t j=0;j<vn;j++)if(i<n&&!memcmp(s+i*w,v+j*w,w))hit=true;ok=i<n&&(neg?!hit:hit);}else ok=vn<=n-i&&!memcmp(s+i*w,v,vn*w);if(ok){found=(uint32_t)i;break;}}
            PG_RETURN(found);
        }
        if (strncmp(op,"5eraseE",7)==0 || strncmp(op,"7replaceE",9)==0) {
            bool iterator=strstr(op,"normal_iterator")!=NULL;size_t pos=iterator?(a[1]-*(uint32_t *)(uintptr_t)obj)/w:a[1];if(pos>n)pos=n;size_t len=iterator?(a[2]-a[1])/w:a[2];if(len>n-pos)len=n-pos;size_t extra=strncmp(op,"7replace",8)==0?a[4]-a[3]:0;char *buf=malloc((n-len+extra+1)*w);memcpy(buf,s,pos*w);if(extra)memcpy(buf+pos*w,(void *)(uintptr_t)a[3],extra*w);memcpy(buf+(pos+extra)*w,s+(pos+len)*w,(n-pos-len)*w);pg_string_set(obj,buf,n-len+extra,w,false);free(buf);PG_RETURN(iterator?*(uint32_t *)(uintptr_t)obj+pos*w:obj);
        }
    }
    return 0;
#undef PG_RETURN
}
