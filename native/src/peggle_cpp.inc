#include <wchar.h>
#include "peggle_tree.inc"
#include "peggle_cxx.inc"
/* Which bridged operation is currently running, so a string probe can name
   the culprit.  Diagnostic only; costs one store per dispatch. */
static const char *pg_current_op = "?";

/* LP32_DEBUG_STRINGS=<text>: report every string whose new value contains
   <text>, naming the operation that wrote it.  A corrupted accumulator is
   otherwise invisible -- the guest never prints it until it fails a
   comparison much later, by which point the operation that spoiled it is
   long gone. */
static void pg_string_watch(uint32_t obj, const void *src, size_t n, size_t width)
{
    static int enabled = -1;
    static const char *prefix;
    static size_t prefix_length;
    if (enabled < 0) {
        prefix = getenv("LP32_DEBUG_STRINGS");
        enabled = prefix != NULL;
        prefix_length = prefix ? strlen(prefix) : 0;
    }
    if (!enabled || !src || n < prefix_length) return;
    /* Flatten to low bytes so one probe covers both string and wstring; the
       text being hunted is ASCII either way. */
    size_t shown = n > 160 ? 160 : n;
    char flat[161];
    for (size_t i = 0; i < shown; ++i) {
        unsigned char byte = ((const unsigned char *)src)[i * width];
        flat[i] = (byte >= 0x20 && byte < 0x7f) ? (char)byte : '.';
    }
    flat[shown] = 0;
    bool hit = false;
    for (size_t i = 0; !hit && i + prefix_length <= shown; ++i)
        hit = memcmp(flat + i, prefix, prefix_length) == 0;
    if (!hit) return;
    fprintf(stderr, "compat32: string %08" PRIx32 " len=%zu w=%zu op=%s value='%s'\n",
            obj, n, width, pg_current_op, flat);
}

/* GCC 4 i386 COW string ABI used by PopCap's Mac framework. */
static uint32_t pg_string_set(uint32_t obj, const void *src, size_t n, size_t width, bool init)
{
    pg_string_watch(obj, src, n, width);
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
/* True when a mangled member name carries a second __normal_iterator, i.e.
   names a range.  The second one is always a substitution back reference
   ("S<index>_") sitting immediately after the "EE" that closes the first
   iterator's template arguments, whatever the index happens to be. */
static bool pg_iterator_range(const char *op)
{
    const char *iterator = strstr(op, "normal_iterator");
    if (!iterator) return false;
    const char *close = strstr(iterator, "EE");
    if (!close) return false;
    const char *cursor = close + 2;
    if (*cursor++ != 'S') return false;
    while ((*cursor >= '0' && *cursor <= '9') ||
           (*cursor >= 'A' && *cursor <= 'Z')) ++cursor;
    return *cursor == '_';
}

static struct { uint32_t obj; uint32_t text; } pg_streams[64];

/* std::ifstream/std::fstream over stdio (Bejeweled 2 reads its backdrop list
   with one).  The guest reaches the stream's sub-objects at fixed offsets it
   computed at compile time -- the filebuf a few words in, the basic_ios
   virtual base at the end -- so a stream is matched by the address range its
   object covers rather than by an exact pointer.  Nothing here needs to know
   the layout beyond that it fits in kPgFileSpan bytes. */
enum { kPgFileSpan = 0x240 };
static struct { uint32_t base; FILE *file; bool eof, fail; } pg_files[16];

static int pg_file_slot(uint32_t address)
{
    for (int i = 0; i < 16; ++i) {
        if (pg_files[i].base && address >= pg_files[i].base &&
            address < pg_files[i].base + kPgFileSpan) return i;
    }
    return -1;
}

/* libstdc++'s _Ios_Openmode bits: app=1 ate=2 binary=4 in=8 out=16 trunc=32. */
static const char *pg_file_mode(uint32_t openmode)
{
    if (openmode & 1) return (openmode & 8) ? "a+b" : "ab";
    if ((openmode & 8) && (openmode & 16)) return (openmode & 32) ? "w+b" : "r+b";
    if (openmode & 16) return "wb";
    return "rb";
}

static void pg_file_close(int slot)
{
    if (slot < 0) return;
    if (pg_files[slot].file) fclose(pg_files[slot].file);
    pg_files[slot].file = NULL;
    pg_files[slot].base = 0;
}

static int peggle_file_stream_dispatch(const char *name, const uint32_t *a, uint64_t *out)
{
#define PG_RETURN(x) do { *out = (uint64_t)(x); return 1; } while (0)
    bool file_stream = strstr(name, "basic_ifstream") || strstr(name, "basic_fstream") ||
                       strstr(name, "basic_ofstream");
    if (file_stream && (strstr(name, "C1E") || strstr(name, "C2E"))) {
        int slot = -1;
        for (int i = 0; i < 16; ++i) if (!pg_files[i].base) { slot = i; break; }
        if (slot < 0) PG_RETURN(a[0]);
        /* The (const char*, openmode) form is the only one the games use; the
           default ctor opens nothing. */
        const char *path = strstr(name, "EPKc") ? (const char *)(uintptr_t)a[1] : NULL;
        pg_files[slot].base = a[0];
        pg_files[slot].eof = pg_files[slot].fail = false;
        pg_files[slot].file = path ? fopen(path, pg_file_mode(a[2])) : NULL;
        if (path && !pg_files[slot].file) pg_files[slot].fail = true;
        PG_RETURN(a[0]);
    }
    if (file_stream && (strstr(name, "D1E") || strstr(name, "D2E"))) {
        pg_file_close(pg_file_slot(a[0]));
        PG_RETURN(0);
    }
    if (file_stream && strstr(name, "5closeEv")) {
        pg_file_close(pg_file_slot(a[0]));
        PG_RETURN(0);
    }
    /* Below here the call must belong to a live file stream; anything else
       (a string stream's basic_ios, say) falls through to its own handler. */
    int slot = pg_file_slot(a[0]);
    if (slot < 0) return 0;
    /* is_open() is asked of the stream itself as well as of its filebuf. */
    if (strstr(name, "7is_openEv")) {
        PG_RETURN(pg_files[slot].file != NULL);
    }
    if (strstr(name, "basic_ios") && strstr(name, "3eofEv")) {
        PG_RETURN(pg_files[slot].eof);
    }
    if (strstr(name, "basic_ios") && (strstr(name, "4failEv") || strstr(name, "3badEv"))) {
        PG_RETURN(pg_files[slot].fail || !pg_files[slot].file);
    }
    if (!pg_files[slot].file) return 0;
    FILE *file = pg_files[slot].file;
    /* istream::getline(char*, streamsize): up to n-1 characters, the newline
       consumed but not stored. */
    if (strcmp(name, "__ZNSi7getlineEPci") == 0) {
        char *buffer = (void *)(uintptr_t)a[1];
        int32_t limit = (int32_t)a[2];
        if (!buffer || limit <= 0) PG_RETURN(a[0]);
        if (!fgets(buffer, limit, file)) {
            buffer[0] = '\0';
            pg_files[slot].eof = pg_files[slot].fail = true;
            PG_RETURN(a[0]);
        }
        size_t length = strlen(buffer);
        if (length && buffer[length - 1] == '\n') buffer[length - 1] = '\0';
        if (feof(file)) pg_files[slot].eof = true;
        PG_RETURN(a[0]);
    }
    if (strcmp(name, "__ZNSi4readEPci") == 0) {
        size_t want = (size_t)(int32_t)a[2];
        size_t got = a[1] && want ? fread((void *)(uintptr_t)a[1], 1, want, file) : 0;
        if (got < want) pg_files[slot].eof = pg_files[slot].fail = true;
        PG_RETURN(a[0]);
    }
    if (strcmp(name, "__ZNSirsERi") == 0) {
        int value = 0;
        if (fscanf(file, "%d", &value) != 1) pg_files[slot].eof = pg_files[slot].fail = true;
        else if (a[1]) *(int32_t *)(uintptr_t)a[1] = value;
        PG_RETURN(a[0]);
    }
    if (strcmp(name, "__ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_") == 0) {
        int value = fgetc(file);
        if (value == EOF) pg_files[slot].eof = pg_files[slot].fail = true;
        else if (a[1]) *(char *)(uintptr_t)a[1] = (char)value;
        PG_RETURN(a[0]);
    }
    return 0;
#undef PG_RETURN
}
static int peggle_cpp_dispatch(const char *name, const uint32_t *a, uint64_t *out)
{
#define PG_RETURN(x) do { *out = (uint64_t)(x); return 1; } while (0)
    if (!lp32_title_is_peggle_engine(lp32_profile()->title)) return 0;
    static int trace_cpp = -1;
    if (trace_cpp < 0) trace_cpp = getenv("LP32_TRACE_CPP") ? 1 : 0;
    if (trace_cpp) { fprintf(stderr, "compat32: cpp %s a0=%08x a1=%08x a2=%08x\n", name, a[0], a[1], a[2]); }
    pg_current_op = name;
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
    /* std::__detail::_List_node_base::hook/unhook: the same pointer surgery
       on the guest's own nodes as ::transfer above (Bejeweled 2's lists). */
    if (strcmp(name,"__ZNSt15_List_node_base4hookEPS_")==0) {
        uint32_t self=a[0], position=a[1];
        uint32_t previous=*(uint32_t *)(uintptr_t)(position+4);
        *(uint32_t *)(uintptr_t)(self+0)=position;
        *(uint32_t *)(uintptr_t)(self+4)=previous;
        *(uint32_t *)(uintptr_t)(previous+0)=self;
        *(uint32_t *)(uintptr_t)(position+4)=self;
        PG_RETURN(0);
    }
    if (strcmp(name,"__ZNSt15_List_node_base6unhookEv")==0) {
        uint32_t self=a[0];
        uint32_t next=*(uint32_t *)(uintptr_t)(self+0);
        uint32_t previous=*(uint32_t *)(uintptr_t)(self+4);
        *(uint32_t *)(uintptr_t)(previous+0)=next;
        *(uint32_t *)(uintptr_t)(next+4)=previous;
        PG_RETURN(0);
    }
    /* operator+(string): all three forms return the new string through the
       hidden sret pointer in a[0]. */
    static const char plus[] = "__ZStplIcSt11char_traitsIcESaIcEESbIT_T0_T1_E";
    if (strncmp(name,plus,sizeof(plus)-1)==0) {
        const char *form=name+sizeof(plus)-1;
        const char *left; size_t left_length; char single;
        if (strncmp(form,"PK",2)==0) { left=(void *)(uintptr_t)a[1]; left_length=strlen(left); }
        else if (strncmp(form,"RK",2)==0) { left=guest_string_data(a[1]); left_length=guest_string_length(a[1]); }
        else { single=(char)a[1]; left=&single; left_length=1; }
        const char *right=guest_string_data(a[2]);
        size_t right_length=guest_string_length(a[2]);
        char *joined=malloc(left_length+right_length+1);
        memcpy(joined,left,left_length);
        memcpy(joined+left_length,right,right_length);
        compat_runtime32_struct_return();
        pg_string_set(a[0],joined,left_length+right_length,1,true);
        free(joined);
        PG_RETURN(a[0]);
    }
    if (peggle_file_stream_dispatch(name, a, out)) return 1;
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
    if (strcmp(name,"__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc")==0 || strcmp(name,"__ZNSolsEi")==0 ||
        strcmp(name,"__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c")==0) {
        char number[64];const char *value=(void *)(uintptr_t)a[1];if(strcmp(name,"__ZNSolsEi")==0){snprintf(number,sizeof(number),"%d",(int)a[1]);value=number;}
        else if(name[strlen(name)-1]=='c'&&name[strlen(name)-2]=='_'){number[0]=(char)a[1];number[1]='\0';value=number;}
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
        /* erase/replace/insert.  Which arguments follow depends on the
           overload, and getting that wrong is not survivable: replace(pos, n,
           const char*) read as the iterator form takes the length from a stack
           word past the arguments and copies gigabytes.  The iterator forms
           are the ones whose mangled parameters name __normal_iterator; among
           those, only the ones carrying a second iterator have a range to
           remove, and that second iterator is a substitution back reference
           whose index depends on how many types the mangling already
           substituted -- S2_ for std::string but S6_ for std::wstring -- so
           it has to be matched by shape rather than by number. */
        if (strncmp(op,"5eraseE",7)==0 || strncmp(op,"7replaceE",9)==0 ||
            strncmp(op,"6insertE",8)==0) {
            bool iterator=strstr(op,"normal_iterator")!=NULL;
            bool replace=strncmp(op,"7replace",8)==0;
            bool insert=strncmp(op,"6insert",7)==0;
            size_t pos,len;
            if (iterator) {
                uint32_t data=*(uint32_t *)(uintptr_t)obj;
                pos=a[1]>data?(a[1]-data)/w:0;
                len=pg_iterator_range(op)?(a[2]>a[1]?(a[2]-a[1])/w:0):(insert?0:1);
            } else { pos=a[1]; len=a[2]; }
            if(pos>n)pos=n;
            if(len>n-pos)len=n-pos;
            /* The replacement: a pointer range for the iterator forms, a C
               string or another string for the (pos, count) forms, and one
               character for insert. */
            const char *source=NULL;size_t extra=0;char single[4]={0,0,0,0};
            if(insert){uint32_t c=a[2];memcpy(single,&c,w<4?w:4);source=single;extra=1;}
            else if(replace&&iterator){source=(const void *)(uintptr_t)a[3];extra=a[4]>a[3]?(a[4]-a[3])/w:0;}
            else if(replace&&(strstr(op,"PKc")||strstr(op,"PKw"))){source=(const void *)(uintptr_t)a[3];extra=source?(wide?wcslen((const wchar_t *)(const void *)source):strlen(source)):0;}
            else if(replace){source=guest_string_data(a[3]);extra=guest_string_length(a[3]);}
            char *buf=malloc((n-len+extra+1)*w);
            memcpy(buf,s,pos*w);
            if(extra)memcpy(buf+pos*w,source,extra*w);
            memcpy(buf+(pos+extra)*w,s+(pos+len)*w,(n-pos-len)*w);
            pg_string_set(obj,buf,n-len+extra,w,false);
            free(buf);
            PG_RETURN(iterator?*(uint32_t *)(uintptr_t)obj+pos*w:obj);
        }
    }
    return 0;
#undef PG_RETURN
}
