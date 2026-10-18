#include "pvz_save.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    unsigned char fixture[976] = {12}; fixture[816] = 1; fixture[820] = 7;
    size_t n=0, restored_size=0;
    void *mac=pvz_save_convert(fixture,sizeof(fixture),true,&n);
    assert(mac && n==948);
    void *win=pvz_save_convert(mac,n,false,&restored_size);
    assert(win && restored_size==sizeof(fixture) && !memcmp(win,fixture,restored_size));
    free(mac);free(win);
    fixture[840]=1; assert(!pvz_save_convert(fixture,sizeof(fixture),true,&n));fixture[840]=0;
    assert(!pvz_save_convert(fixture,887,true,&n));
    fixture[0]=13;assert(!pvz_save_convert(fixture,sizeof(fixture),true,&n));fixture[0]=12;
    fixture[816]=255;assert(!pvz_save_convert(fixture,sizeof(fixture),true,&n));
    assert(pvz_save_is_profile("user12.dat") && pvz_save_is_profile("USER1.DAT"));
    assert(!pvz_save_is_profile("users.dat") && !pvz_save_is_profile("user.dat") && !pvz_save_is_profile("user1.dat/x"));
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0);rewind(f);
        void *data=malloc((size_t)bytes);assert(data);assert(fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
        mac=pvz_save_convert(data,(size_t)bytes,true,&n);assert(mac);
        win=pvz_save_convert(mac,n,false,&restored_size);
        assert(win && restored_size==(size_t)bytes && !memcmp(win,data,restored_size));
        printf("Existing Windows profile: %ld -> %zu Mac bytes -> byte-identical Windows roundtrip PASS\n",bytes,n);
        free(data);free(mac);free(win);
    }
    puts("PvZ profile conversion tests PASS");
}
