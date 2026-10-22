/* Real filesystem/process regression for the pure C completed-length cache.
 * Every test explicitly redirects cache writes to its own temporary directory.
 * No FLAC, GUI, hardware or full-input hashing is involved. GPL-3.0-or-later. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../misrc_gui/input/gui_playback_length_cache.h"
#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TEST_PATH_CAP 4096u
#define TEST_FILE_BYTES 12288u
#define TEST_SAMPLES UINT64_C(73538732032)
static unsigned char test_original[TEST_FILE_BYTES];
static gui_playback_length_cache_key_t test_concurrent_key;
static atomic_uint test_successful_stores;

static void test_path(char *path,const char *directory,const char *name) {
    int n=snprintf(path,TEST_PATH_CAP,"%s/%s",directory,name);
    assert(n>0 && (size_t)n<TEST_PATH_CAP);
}
static void test_cache_directory(const char *directory) {
#ifdef _WIN32
    assert(SetEnvironmentVariableA("MISRC_PLAYBACK_CACHE_DIR",directory));
    /* CRT _putenv_s(name, "") deletes a variable; an OS empty value is the
     * explicit disabled override, and NULL would mean default location. */
    if(!directory[0]) {
        char value[2]={1,1}; SetLastError(ERROR_SUCCESS);
        assert(GetEnvironmentVariableA("MISRC_PLAYBACK_CACHE_DIR",value,sizeof(value))==0);
        assert(GetLastError()!=ERROR_ENVVAR_NOT_FOUND);
    }
#else
    assert(setenv("MISRC_PLAYBACK_CACHE_DIR",directory,1)==0);
#endif
}
static void test_create_directory(const char *directory) {
#ifdef _WIN32
    assert(CreateDirectoryA(directory,NULL));
#else
    assert(mkdir(directory,0700)==0);
#endif
}
static void test_remove_file(const char *path) {
#ifdef _WIN32
    assert(DeleteFileA(path));
#else
    assert(unlink(path)==0);
#endif
}
static void test_remove_directory(const char *path) {
#ifdef _WIN32
    assert(RemoveDirectoryA(path));
#else
    assert(rmdir(path)==0);
#endif
}
static void test_write(const char *path,const void *bytes,size_t count) {
    FILE *file=fopen(path,"wb"); assert(file);
    assert(fwrite(bytes,1,count,file)==count && fclose(file)==0);
}
static size_t test_read(const char *path,unsigned char *bytes,size_t capacity) {
    FILE *file=fopen(path,"rb"); assert(file);
    size_t count=fread(bytes,1,capacity,file);
    assert(!ferror(file) && fclose(file)==0); return count;
}
static FILE *test_open_share_delete(const char *path) {
#ifdef _WIN32
    /* A decoder's ordinary fopen may deny deletion. Exercise the replacement
     * race with an actual FILE whose underlying handle permits path removal. */
    HANDLE handle=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                              NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(handle!=INVALID_HANDLE_VALUE);
    int descriptor=_open_osfhandle((intptr_t)handle,_O_RDONLY|_O_BINARY);
    assert(descriptor>=0);
    FILE *file=_fdopen(descriptor,"rb"); assert(file);
    return file;
#else
    FILE *file=fopen(path,"rb"); assert(file); return file;
#endif
}
static void test_set_time(const char *path,const gui_playback_length_cache_key_t *key,int delta_seconds) {
#ifdef _WIN32
    HANDLE file=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(file!=INVALID_HANDLE_VALUE);
    uint64_t ticks=(uint64_t)(key->mtime_seconds+INT64_C(11644473600)+delta_seconds)*10000000u+
        key->mtime_nanoseconds/100u;
    FILETIME time={(DWORD)ticks,(DWORD)(ticks>>32)};
    assert(SetFileTime(file,NULL,NULL,&time) && CloseHandle(file));
#else
    struct timespec times[2]={{0,UTIME_OMIT},{(time_t)(key->mtime_seconds+delta_seconds),(long)key->mtime_nanoseconds}};
    assert(utimensat(AT_FDCWD,path,times,0)==0);
#endif
}
static unsigned test_find_records(const char *directory,char (*paths)[TEST_PATH_CAP],unsigned capacity) {
    unsigned count=0;
#ifdef _WIN32
    char pattern[TEST_PATH_CAP]; test_path(pattern,directory,"*.bin");
    WIN32_FIND_DATAA item; HANDLE search=FindFirstFileA(pattern,&item);
    if(search==INVALID_HANDLE_VALUE) return 0;
    do {
        assert(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY));
        assert(count<capacity); test_path(paths[count++],directory,item.cFileName);
    } while(FindNextFileA(search,&item));
    assert(FindClose(search));
#else
    DIR *search=opendir(directory); if(!search) return 0;
    struct dirent *item;
    while((item=readdir(search))) {
        size_t n=strlen(item->d_name);
        if(n<4 || strcmp(item->d_name+n-4,".bin")) continue;
        assert(count<capacity); test_path(paths[count++],directory,item->d_name);
    }
    assert(closedir(search)==0);
#endif
    return count;
}
static void test_expect_miss(const gui_playback_length_cache_key_t *key) {
    uint64_t samples=UINT64_MAX;
    assert(!gui_playback_length_cache_lookup(key,&samples) && samples==0);
}
static void test_default_v2_directory(const char *directory,const char *cache,
                                      const gui_playback_length_cache_key_t *key) {
    char absolute[TEST_PATH_CAP],base[TEST_PATH_CAP],product[TEST_PATH_CAP],versioned[TEST_PATH_CAP];
#ifdef _WIN32
    DWORD count=GetFullPathNameA(directory,sizeof(absolute),absolute,NULL);
    assert(count>0 && count<sizeof(absolute));
    char previous[TEST_PATH_CAP]; SetLastError(ERROR_SUCCESS);
    count=GetEnvironmentVariableA("LOCALAPPDATA",previous,sizeof(previous));
    assert(count<sizeof(previous));
    bool absent=count==0 && GetLastError()==ERROR_ENVVAR_NOT_FOUND;
    if(count==0) previous[0]='\0';
#else
    if(directory[0]=='/') {
        assert(strlen(directory)<sizeof(absolute)); strcpy(absolute,directory);
    } else {
        char cwd[TEST_PATH_CAP]; assert(getcwd(cwd,sizeof(cwd)));
        test_path(absolute,cwd,directory);
    }
    const char *current=getenv("XDG_CACHE_HOME");
    char *previous=current?strdup(current):NULL; assert(!current || previous);
#endif
    test_path(base,absolute,"default-cache-root");
    test_path(product,base,"MISRC"); test_path(versioned,product,"playback-length-v2");
#ifdef _WIN32
    assert(SetEnvironmentVariableA("LOCALAPPDATA",base));
    assert(SetEnvironmentVariableA("MISRC_PLAYBACK_CACHE_DIR",NULL));
#else
    assert(setenv("XDG_CACHE_HOME",base,1)==0 && unsetenv("MISRC_PLAYBACK_CACHE_DIR")==0);
#endif
    assert(gui_playback_length_cache_store(key,TEST_SAMPLES));
    uint64_t samples=0; assert(gui_playback_length_cache_lookup(key,&samples) && samples==TEST_SAMPLES);
    char records[1][TEST_PATH_CAP]; assert(test_find_records(versioned,records,1)==1);
    test_remove_file(records[0]); test_remove_directory(versioned);
    test_remove_directory(product); test_remove_directory(base);
#ifdef _WIN32
    assert(SetEnvironmentVariableA("LOCALAPPDATA",absent?NULL:previous));
#else
    if(previous) { assert(setenv("XDG_CACHE_HOME",previous,1)==0); free(previous); }
    else assert(unsetenv("XDG_CACHE_HOME")==0);
#endif
    test_cache_directory(cache);
    puts("Length cache: unset override uses actual default playback-length-v2 directory PASS");
}
static void test_legacy_record(unsigned char *record,size_t bytes) {
    /* Start from an actual emitted record with the current filesystem key.
     * Produce the old documented V1 record, including its valid FNV checksum,
     * so a miss proves version isolation rather than corruption detection. */
    assert(bytes==96 && record[8]==2 && record[9]==0 && record[10]==0 && record[11]==0);
    record[8]=1;
    uint64_t checksum=UINT64_C(14695981039346656037);
    for(size_t n=0;n<88;++n) { checksum^=record[n]; checksum*=UINT64_C(1099511628211); }
    for(unsigned n=0;n<8;++n) record[88+n]=(unsigned char)(checksum>>(n*8));
}
static void test_child(const char *program,const char *input,const char *cache) {
#ifdef _WIN32
    char executable[TEST_PATH_CAP];
    DWORD count=GetModuleFileNameA(NULL,executable,sizeof(executable));
    assert(count>0 && count<sizeof(executable)); (void)program;
    char command[TEST_PATH_CAP*3];
    int n=snprintf(command,sizeof(command),"\"%s\" --child-lookup \"%s\" \"%s\"",executable,input,cache);
    assert(n>0 && (size_t)n<sizeof(command));
    STARTUPINFOA startup={0}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION process={0};
    assert(CreateProcessA(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
    assert(WaitForSingleObject(process.hProcess,10000)==WAIT_OBJECT_0);
    DWORD exit_code=1; assert(GetExitCodeProcess(process.hProcess,&exit_code) && exit_code==0);
    assert(CloseHandle(process.hThread) && CloseHandle(process.hProcess));
#else
    pid_t child=fork(); assert(child>=0);
    if(!child) { execlp(program,program,"--child-lookup",input,cache,(char *)NULL); _exit(127); }
    int status=0; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
#endif
    puts("Length cache: fresh process sees persisted >2^36 count PASS");
}
#ifdef _WIN32
static DWORD WINAPI test_writer(void *context) {
#else
static void *test_writer(void *context) {
#endif
    (void)context;
    for(unsigned n=0;n<16;++n) {
        if(gui_playback_length_cache_store(&test_concurrent_key,TEST_SAMPLES))
            atomic_fetch_add(&test_successful_stores,1);
        uint64_t samples=0;
        assert(gui_playback_length_cache_lookup(&test_concurrent_key,&samples) && samples==TEST_SAMPLES);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_concurrent(const gui_playback_length_cache_key_t *key) {
    test_concurrent_key=*key; atomic_store(&test_successful_stores,0);
#ifdef _WIN32
    HANDLE threads[2];
    for(unsigned n=0;n<2;++n) { threads[n]=CreateThread(NULL,0,test_writer,NULL,0,NULL); assert(threads[n]); }
    assert(WaitForMultipleObjects(2,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned n=0;n<2;++n) assert(CloseHandle(threads[n]));
#else
    pthread_t threads[2];
    for(unsigned n=0;n<2;++n) assert(pthread_create(&threads[n],NULL,test_writer,NULL)==0);
    for(unsigned n=0;n<2;++n) assert(pthread_join(threads[n],NULL)==0);
#endif
    assert(atomic_load(&test_successful_stores)>0);
    puts("Length cache: concurrent atomic replacement keeps complete records PASS");
}

int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"--child-lookup")) {
        test_cache_directory(argv[3]);
        gui_playback_length_cache_key_t key; uint64_t samples=0;
        assert(gui_playback_length_cache_snapshot(argv[2],&key));
        assert(gui_playback_length_cache_lookup(&key,&samples) && samples==TEST_SAMPLES);
        return 0;
    }
    assert(argc<=2);
    char directory[TEST_PATH_CAP],cache[TEST_PATH_CAP],input[TEST_PATH_CAP],replacement[TEST_PATH_CAP];
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    int n=snprintf(directory,sizeof(directory),"%s/length-cache-test-%lu",argc==2?argv[1]:".",pid);
    assert(n>0 && (size_t)n<sizeof(directory)); test_create_directory(directory);
    test_path(cache,directory,"cache"); test_cache_directory(cache);
    test_path(input,directory,"input.bin"); test_path(replacement,directory,"replacement.bin");
    for(unsigned k=0;k<TEST_FILE_BYTES;++k) test_original[k]=(unsigned char)(k*29u+17u);
    test_write(input,test_original,sizeof(test_original));
    gui_playback_length_cache_key_t original,key;
    assert(gui_playback_length_cache_snapshot(input,&original));
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(gui_playback_length_cache_keys_equal(&original,&key));
    assert(original.head_bytes==4096 && original.tail_bytes==4096);
    test_expect_miss(&original);
    assert(!gui_playback_length_cache_store(&original,0));
    assert(gui_playback_length_cache_store(&original,TEST_SAMPLES));
    uint64_t samples=0; assert(gui_playback_length_cache_lookup(&key,&samples) && samples==TEST_SAMPLES);
    test_child(argv[0],input,cache); test_concurrent(&original);
    test_default_v2_directory(directory,cache,&original);

    char records[4][TEST_PATH_CAP]; assert(test_find_records(cache,records,4)==1);
    unsigned char record[256]; size_t record_bytes=test_read(records[0],record,sizeof(record));
    assert(record_bytes>0 && record_bytes<sizeof(record));
    unsigned char damaged[256]; memcpy(damaged,record,record_bytes);
    test_legacy_record(damaged,record_bytes);
    test_write(records[0],damaged,record_bytes); test_expect_miss(&original);
    assert(gui_playback_length_cache_store(&original,TEST_SAMPLES));
    assert(gui_playback_length_cache_lookup(&original,&samples) && samples==TEST_SAMPLES);
    puts("Length cache: checksum-valid V1 header-scan record rejected; V2 grammar count persisted PASS");
    memcpy(damaged,record,record_bytes); damaged[0]^=1;
    test_write(records[0],damaged,record_bytes); test_expect_miss(&original);
    memcpy(damaged,record,record_bytes); damaged[80]^=1;
    test_write(records[0],damaged,record_bytes); test_expect_miss(&original);
    test_write(records[0],record,record_bytes-1); test_expect_miss(&original);
    memcpy(damaged,record,record_bytes); damaged[record_bytes]=0;
    test_write(records[0],damaged,record_bytes+1); test_expect_miss(&original);
    assert(gui_playback_length_cache_store(&original,TEST_SAMPLES));
    gui_playback_length_cache_key_t invalid=original; invalid.mtime_nanoseconds=1000000000u;
    assert(!gui_playback_length_cache_store(&invalid,TEST_SAMPLES)); test_expect_miss(&invalid);
    puts("Length cache: corruption, truncation, trailing bytes, zero and invalid time fail closed PASS");

#ifdef _WIN32
    assert(SetFileAttributesA(records[0],FILE_ATTRIBUTE_READONLY));
    assert(!gui_playback_length_cache_store(&original,TEST_SAMPLES+1));
    assert(gui_playback_length_cache_lookup(&original,&samples) && samples==TEST_SAMPLES);
    assert(SetFileAttributesA(records[0],FILE_ATTRIBUTE_NORMAL));
#else
    if(geteuid()!=0) {
        assert(chmod(cache,0500)==0);
        assert(!gui_playback_length_cache_store(&original,TEST_SAMPLES+1));
        assert(gui_playback_length_cache_lookup(&original,&samples) && samples==TEST_SAMPLES);
        assert(chmod(cache,0700)==0);
    }
#endif
    char blocked[TEST_PATH_CAP],unusable[TEST_PATH_CAP]; test_path(blocked,directory,"not-a-directory");
    test_write(blocked,"x",1); test_path(unusable,blocked,"cache"); test_cache_directory(unusable);
    assert(!gui_playback_length_cache_store(&original,TEST_SAMPLES)); test_expect_miss(&original);
    test_cache_directory(""); assert(!gui_playback_length_cache_store(&original,TEST_SAMPLES));
    test_expect_miss(&original); test_cache_directory(cache);
    puts("Length cache: unwritable/unusable destination and explicit empty override stay optional PASS");

    test_set_time(input,&original,2);
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(!gui_playback_length_cache_keys_equal(&original,&key)); test_expect_miss(&key);
    gui_playback_length_cache_key_t subsecond=original;
    subsecond.mtime_nanoseconds+=100u;
    if(subsecond.mtime_nanoseconds>=1000000000u) {
        subsecond.mtime_nanoseconds-=1000000000u; ++subsecond.mtime_seconds;
    }
    test_set_time(input,&subsecond,0);
    assert(gui_playback_length_cache_snapshot(input,&key));
    if(!gui_playback_length_cache_keys_equal(&original,&key)) {
        test_expect_miss(&key);
        puts("Length cache: subsecond mtime change invalidates on this filesystem PASS");
    } else puts("Length cache: filesystem rounds the subsecond mtime; content fingerprints still checked");
    test_set_time(input,&original,0);
    FILE *file=fopen(input,"ab"); assert(file && fputc(0,file)!=EOF && fclose(file)==0);
    test_set_time(input,&original,0);
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(key.file_size==original.file_size+1 && !gui_playback_length_cache_keys_equal(&original,&key));
    test_expect_miss(&key);
    test_write(input,test_original,sizeof(test_original)); test_set_time(input,&original,0);
    file=fopen(input,"r+b"); assert(file && fseek(file,1,SEEK_SET)==0);
    assert(fputc(test_original[1]^1,file)!=EOF && fclose(file)==0); test_set_time(input,&original,0);
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(key.file_id==original.file_id && key.file_size==original.file_size &&
           key.mtime_seconds==original.mtime_seconds && key.mtime_nanoseconds==original.mtime_nanoseconds);
    assert(key.head_hash!=original.head_hash && !gui_playback_length_cache_keys_equal(&original,&key));
    test_expect_miss(&key);
    test_write(input,test_original,sizeof(test_original)); test_set_time(input,&original,0);
    file=fopen(input,"r+b"); assert(file && fseek(file,TEST_FILE_BYTES-1,SEEK_SET)==0);
    assert(fputc(test_original[TEST_FILE_BYTES-1]^1,file)!=EOF && fclose(file)==0); test_set_time(input,&original,0);
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(key.tail_hash!=original.tail_hash); test_expect_miss(&key);
    puts("Length cache: size, precise mtime, head/tail content with preserved mtime invalidate PASS");

    gui_playback_length_cache_key_t held_key=key;
    FILE *held=test_open_share_delete(input);
    assert(fseek(held,TEST_FILE_BYTES-2,SEEK_SET)==0);
    assert(fgetc(held)==test_original[TEST_FILE_BYTES-2]);
    long held_position=ftell(held); assert(held_position==TEST_FILE_BYTES-1);
    assert(gui_playback_length_cache_matches_open_file(held,&held_key));
    assert(!gui_playback_length_cache_matches_open_file(NULL,&held_key));
    assert(!gui_playback_length_cache_matches_open_file(held,NULL));
    assert(!gui_playback_length_cache_matches_open_file(held,&invalid));
    gui_playback_length_cache_key_t changed_metadata=held_key;
    ++changed_metadata.file_size;
    assert(!gui_playback_length_cache_matches_open_file(held,&changed_metadata));
    changed_metadata=held_key; ++changed_metadata.mtime_seconds;
    assert(!gui_playback_length_cache_matches_open_file(held,&changed_metadata));
    assert(ftell(held)==held_position);
    test_write(replacement,test_original,sizeof(test_original)); test_set_time(replacement,&original,0);
#ifdef _WIN32
    /* Windows rejects replacing this open destination through MoveFileEx.
     * Renaming it away then installing a new path still exercises a held old
     * decoder FILE and a pathname snapshot of a different actual file. */
    char retired[TEST_PATH_CAP]; test_path(retired,directory,"retired.bin");
    assert(MoveFileExA(input,retired,MOVEFILE_WRITE_THROUGH));
    assert(MoveFileExA(replacement,input,MOVEFILE_WRITE_THROUGH));
#else
    assert(rename(replacement,input)==0);
#endif
    assert(gui_playback_length_cache_snapshot(input,&key));
    assert(key.file_id!=original.file_id || key.file_device!=original.file_device);
    assert(!gui_playback_length_cache_matches_open_file(held,&key));
    assert(gui_playback_length_cache_matches_open_file(held,&held_key));
    assert(ftell(held)==held_position);
    assert(fgetc(held)==(test_original[TEST_FILE_BYTES-1]^1));
    assert(fgetc(held)==EOF && !ferror(held));
    assert(fclose(held)==0);
#ifdef _WIN32
    test_remove_file(retired);
#endif
    puts("Length cache: held old FILE rejects replaced pathname key without moving buffered position PASS");
    assert(!gui_playback_length_cache_keys_equal(&original,&key)); test_expect_miss(&key);
    assert(gui_playback_length_cache_store(&key,TEST_SAMPLES));
    assert(test_find_records(cache,records,4)==2);
    /* A well-formed record at the wrong key filename must still miss. Reuse the
     * old valid bytes without mirroring serialization or checksum generation. */
    for(unsigned k=0;k<2;++k) test_write(records[k],record,record_bytes);
    test_expect_miss(&key);
    assert(gui_playback_length_cache_store(&key,TEST_SAMPLES));
    test_child(argv[0],input,cache);
    puts("Length cache: same path/size/time replacement identity and full stored key validation PASS");

    assert(!gui_playback_length_cache_snapshot(directory,&key));
    test_write(replacement,"",0); assert(!gui_playback_length_cache_snapshot(replacement,&key));
    unsigned char untouched[TEST_FILE_BYTES];
    assert(test_read(input,untouched,sizeof(untouched))==sizeof(untouched) &&
           memcmp(untouched,test_original,sizeof(untouched))==0);
    test_remove_file(replacement); test_remove_file(input); test_remove_file(blocked);
    assert(test_find_records(cache,records,4)==2);
    for(unsigned k=0;k<2;++k) test_remove_file(records[k]);
    test_remove_directory(cache); test_remove_directory(directory);
    puts("Length cache filesystem/process regression passed; original input unchanged by cache APIs");
    return 0;
}
