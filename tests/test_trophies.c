/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production trophy service without booting a game or changing saves. */
#include "../src/runtime_services.c"
#include <assert.h>
#include <process.h>

static char test_dir[1024];
static int popup_count;
/* Services outside the tested trophy path must never run in this fixture. */
const char *runtime_symbol(const char *nid) { (void)nid; abort(); }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) { (void)table; (void)count; (void)nid; abort(); }
uintptr_t bbgpu_resolve(const char *name) { (void)name; abort(); }
int64_t runtime_utc_offset(time_t when) { (void)when; abort(); }
int runtime_win_inet_pton4(const char *src,void *dst) { (void)src; (void)dst; abort(); }
const char *runtime_win_inet_ntop4(const void *src,char *dst,uint32_t size) { (void)src; (void)dst; (void)size; abort(); }
void host_sleep_ns(uint64_t ns) { (void)ns; abort(); }
int bbgpu_text_input_begin(const char *initial,const char *prompt) { (void)initial; (void)prompt; abort(); }
int bbgpu_text_input_poll(char *out,uint64_t size) { (void)out; (void)size; abort(); }
int64_t runtime_file_open(const char *path,int flags,int mode) { (void)path; (void)flags; (void)mode; abort(); }
int64_t runtime_file_read(int fd,void *buffer,uint64_t size) { (void)fd; (void)buffer; (void)size; abort(); }
int64_t runtime_file_close(int fd) { (void)fd; abort(); }
const char *runtime_file_user_dir(void) { return test_dir; }
void bbgpu_trophy_popup(const char *name,int grade) {
    assert(name && *name && grade>=BRONZE && grade<=PLATINUM);
    ++popup_count;
}
static void reload(void) {
    trophies_loaded=0; trophies_unlocked=0; memset(trophy_times,0,sizeof(trophy_times));
}
static int64_t unlocked(int id) {
    const char *name,*description; int grade,hidden; int64_t when;
    assert(runtime_trophy(id,&name,&description,&grade,&hidden,&when));
    assert(name && description && grade>=BRONZE && grade<=PLATINUM);
    return when;
}
static unsigned __stdcall award_thread(void *unused) {
    (void)unused;
    for(int i=0;i<100;++i) assert(trophy_unlock(1,1,14,NULL)==0);
    return 0;
}
int main(void) {
    char temp[MAX_PATH];
    assert(GetTempPathA(sizeof(temp),temp));
    assert(snprintf(test_dir,sizeof(test_dir),"%sbbport-trophies-%lu",temp,(unsigned long)GetCurrentProcessId())>0);
    assert(CreateDirectoryA(test_dir,NULL));
    assert(!unlocked(14));
    HANDLE threads[4];
    for(unsigned i=0;i<4;++i) threads[i]=(HANDLE)_beginthreadex(NULL,0,award_thread,NULL,0,NULL);
    for(unsigned i=0;i<4;++i) { assert(threads[i]); assert(WaitForSingleObject(threads[i],10000)==WAIT_OBJECT_0); CloseHandle(threads[i]); }
    assert(popup_count==1 && unlocked(14)>1);
    reload(); assert(unlocked(14)>1 && !unlocked(0));
    assert(!runtime_trophy(-1,NULL,NULL,NULL,NULL,NULL));
    assert(!runtime_trophy(TROPHY_COUNT,NULL,NULL,NULL,NULL,NULL));
    for(int id=1;id<=TROPHY_BASE_LAST;++id) {
        int platinum=-1; assert(!trophy_unlock(1,1,id,&platinum));
        if(id<TROPHY_BASE_LAST) assert(platinum==-1 && !unlocked(0));
        else assert(platinum==0 && unlocked(0)>1);
    }
    assert(popup_count==TROPHY_BASE_LAST+1);
    for(int id=34;id<TROPHY_COUNT;++id) assert(!unlocked(id));
    reload(); assert(unlocked(0)>1 && !unlocked(39));
    char path[1024]; trophy_path(path,sizeof(path));
    FILE *f=fopen(path,"a"); assert(f); fputs("malformed\n-1 123\n9999 456\n",f); assert(!fclose(f));
    reload(); assert(unlocked(0)>1 && !unlocked(39));
    assert(DeleteFileA(path)); assert(RemoveDirectoryA(test_dir));
    puts("PASS: trophy persistence, concurrent duplicate rejection, base platinum, DLC separation and malformed records");
}
