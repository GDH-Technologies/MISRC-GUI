/* Persistent grammar-validated completed-scan sample counts. GPL-3.0-or-later.
 * Records contain only identity/timestamps, bounded fingerprints and a count.
 * Native input is read-only. A cache failure never needs playback to fail. */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "gui_playback_length_cache.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CACHE_PATH_CAP 4096u
#define CACHE_FINGERPRINT_BYTES 4096u
#define CACHE_RECORD_BYTES 96u
/* V1 could contain counts from the former header-only scanner. V2 accepts
 * only counts from the caller's completed frame-grammar/integrity traversal. */
#define CACHE_VERSION 2u
#define CACHE_FNV_OFFSET UINT64_C(14695981039346656037)
#define CACHE_FNV_PRIME UINT64_C(1099511628211)
static atomic_uint_fast64_t cache_temporary_sequence=ATOMIC_VAR_INIT(0);

static uint64_t cache_hash(const unsigned char *bytes,size_t count) {
    uint64_t hash=CACHE_FNV_OFFSET;
    for(size_t n=0;n<count;++n) { hash^=bytes[n]; hash*=CACHE_FNV_PRIME; }
    return hash;
}
static void cache_put32(unsigned char *bytes,uint32_t value) {
    for(unsigned n=0;n<4;++n) bytes[n]=(unsigned char)(value>>(n*8));
}
static void cache_put64(unsigned char *bytes,uint64_t value) {
    for(unsigned n=0;n<8;++n) bytes[n]=(unsigned char)(value>>(n*8));
}
static uint32_t cache_get32(const unsigned char *bytes) {
    uint32_t value=0;
    for(unsigned n=0;n<4;++n) value|=(uint32_t)bytes[n]<<(n*8);
    return value;
}
static uint64_t cache_get64(const unsigned char *bytes) {
    uint64_t value=0;
    for(unsigned n=0;n<8;++n) value|=(uint64_t)bytes[n]<<(n*8);
    return value;
}
static bool cache_valid_key(const gui_playback_length_cache_key_t *key) {
    if(!key || !key->file_size || key->mtime_nanoseconds>=1000000000u) return false;
    uint32_t count=key->file_size<CACHE_FINGERPRINT_BYTES
        ? (uint32_t)key->file_size:CACHE_FINGERPRINT_BYTES;
    return key->head_bytes==count && key->tail_bytes==count;
}
static bool cache_same_metadata(const gui_playback_length_cache_key_t *a,
                                const gui_playback_length_cache_key_t *b) {
    return a->file_size==b->file_size && a->file_device==b->file_device &&
        a->file_id==b->file_id && a->mtime_seconds==b->mtime_seconds &&
        a->mtime_nanoseconds==b->mtime_nanoseconds;
}
bool gui_playback_length_cache_keys_equal(const gui_playback_length_cache_key_t *a,
                                         const gui_playback_length_cache_key_t *b) {
    return a && b && cache_same_metadata(a,b) && a->head_bytes==b->head_bytes &&
        a->tail_bytes==b->tail_bytes && a->head_hash==b->head_hash && a->tail_hash==b->tail_hash;
}
static void cache_encode_key(unsigned char *bytes,const gui_playback_length_cache_key_t *key) {
    cache_put64(bytes,key->file_size);
    cache_put64(bytes+8,key->file_device);
    cache_put64(bytes+16,key->file_id);
    cache_put64(bytes+24,(uint64_t)key->mtime_seconds);
    cache_put32(bytes+32,key->mtime_nanoseconds);
    cache_put32(bytes+36,key->head_bytes);
    cache_put32(bytes+40,key->tail_bytes);
    cache_put32(bytes+44,0);
    cache_put64(bytes+48,key->head_hash);
    cache_put64(bytes+56,key->tail_hash);
}

#ifdef _WIN32
static wchar_t *cache_wide_path(const char *path) {
    if(!path || !path[0]) return NULL;
    UINT code_page=CP_UTF8;
    int count=MultiByteToWideChar(code_page,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    if(!count) { code_page=CP_ACP; count=MultiByteToWideChar(code_page,0,path,-1,NULL,0); }
    if(count<=0 || (size_t)count>SIZE_MAX/sizeof(wchar_t)) return NULL;
    wchar_t *wide=malloc((size_t)count*sizeof(*wide));
    if(wide && !MultiByteToWideChar(code_page,code_page==CP_UTF8?MB_ERR_INVALID_CHARS:0,
                                  path,-1,wide,count)) { free(wide); wide=NULL; }
    return wide;
}
static bool cache_metadata(HANDLE file,gui_playback_length_cache_key_t *key) {
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(file,&info) ||
       (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return false;
    key->file_size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
    key->file_device=info.dwVolumeSerialNumber;
    key->file_id=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow;
    uint64_t ticks=((uint64_t)info.ftLastWriteTime.dwHighDateTime<<32)|info.ftLastWriteTime.dwLowDateTime;
    key->mtime_seconds=(int64_t)(ticks/10000000u)-INT64_C(11644473600);
    key->mtime_nanoseconds=(uint32_t)(ticks%10000000u)*100u;
    return key->file_size>0;
}
static bool cache_read_at(HANDLE file,uint64_t offset,unsigned char *bytes,size_t count) {
    LARGE_INTEGER position; position.QuadPart=(LONGLONG)offset;
    if(offset>INT64_MAX || !SetFilePointerEx(file,position,NULL,FILE_BEGIN)) return false;
    size_t read_count=0;
    while(read_count<count) {
        DWORD n=0;
        if(!ReadFile(file,bytes+read_count,(DWORD)(count-read_count),&n,NULL) || !n) return false;
        read_count+=n;
    }
    return true;
}
#else
static bool cache_metadata(int file,gui_playback_length_cache_key_t *key) {
    struct stat info;
    if(fstat(file,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size<=0) return false;
    key->file_size=(uint64_t)info.st_size;
    key->file_device=(uint64_t)info.st_dev;
    key->file_id=(uint64_t)info.st_ino;
#ifdef __APPLE__
    key->mtime_seconds=(int64_t)info.st_mtimespec.tv_sec;
    key->mtime_nanoseconds=(uint32_t)info.st_mtimespec.tv_nsec;
#else
    key->mtime_seconds=(int64_t)info.st_mtim.tv_sec;
    key->mtime_nanoseconds=(uint32_t)info.st_mtim.tv_nsec;
#endif
    return key->mtime_nanoseconds<1000000000u;
}
static bool cache_read_at(int file,uint64_t offset,unsigned char *bytes,size_t count) {
    size_t read_count=0;
    while(read_count<count) {
        uint64_t next=offset+read_count;
        off_t position=(off_t)next;
        if(position<0 || (uint64_t)position!=next) return false;
        ssize_t n=pread(file,bytes+read_count,count-read_count,position);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return false;
        read_count+=(size_t)n;
    }
    return true;
}
#endif

bool gui_playback_length_cache_matches_open_file(FILE *file,
                                                const gui_playback_length_cache_key_t *key) {
    if(!file || !cache_valid_key(key)) return false;
    gui_playback_length_cache_key_t held={0};
#ifdef _WIN32
    int descriptor=_fileno(file);
    if(descriptor<0) return false;
    intptr_t native=_get_osfhandle(descriptor);
    if(native==-1) return false;
    HANDLE handle=(HANDLE)native;
    if(GetFileType(handle)!=FILE_TYPE_DISK) return false;
    return cache_metadata(handle,&held) && cache_same_metadata(&held,key);
#else
    int descriptor=fileno(file);
    return descriptor>=0 && cache_metadata(descriptor,&held) && cache_same_metadata(&held,key);
#endif
}

bool gui_playback_length_cache_snapshot(const char *filepath,
                                       gui_playback_length_cache_key_t *key) {
    if(!key) return false;
    memset(key,0,sizeof(*key));
    if(!filepath || !filepath[0]) return false;
#ifdef _WIN32
    wchar_t *wide=cache_wide_path(filepath);
    if(!wide) return false;
    HANDLE file=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,NULL);
    free(wide);
    if(file==INVALID_HANDLE_VALUE || GetFileType(file)!=FILE_TYPE_DISK) {
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
        return false;
    }
#else
    int file=open(filepath,O_RDONLY|O_NONBLOCK);
    if(file<0) return false;
#endif
    gui_playback_length_cache_key_t before={0},after={0};
    unsigned char bytes[CACHE_FINGERPRINT_BYTES];
    bool ok=cache_metadata(file,&before);
    if(ok) {
        before.head_bytes=before.file_size<CACHE_FINGERPRINT_BYTES
            ? (uint32_t)before.file_size:CACHE_FINGERPRINT_BYTES;
        before.tail_bytes=before.head_bytes;
        ok=cache_read_at(file,0,bytes,before.head_bytes);
        if(ok) before.head_hash=cache_hash(bytes,before.head_bytes);
        if(ok) ok=cache_read_at(file,before.file_size-before.tail_bytes,bytes,before.tail_bytes);
        if(ok) before.tail_hash=cache_hash(bytes,before.tail_bytes);
        if(ok) ok=cache_metadata(file,&after) && cache_same_metadata(&before,&after);
    }
#ifdef _WIN32
    CloseHandle(file);
#else
    close(file);
#endif
    if(ok) *key=before;
    return ok;
}

static bool cache_copy_path(char *output,size_t capacity,const char *base,const char *suffix) {
    int n=snprintf(output,capacity,"%s%s",base,suffix);
    return n>=0 && (size_t)n<capacity;
}
#ifdef _WIN32
static int cache_environment(const char *name,char *value,DWORD capacity) {
    wchar_t wide_name[64],wide_value[CACHE_PATH_CAP];
    if(!MultiByteToWideChar(CP_UTF8,0,name,-1,wide_name,sizeof(wide_name)/sizeof(*wide_name))) return -1;
    SetLastError(ERROR_SUCCESS);
    DWORD count=GetEnvironmentVariableW(wide_name,wide_value,CACHE_PATH_CAP);
    if(count>=CACHE_PATH_CAP) return -1;
    if(count>0) return WideCharToMultiByte(CP_UTF8,0,wide_value,-1,value,(int)capacity,NULL,NULL)?1:-1;
    value[0]=0;
    return GetLastError()==ERROR_ENVVAR_NOT_FOUND?0:1;
}
#endif
static bool cache_directory(char *directory,size_t capacity) {
#ifdef _WIN32
    char value[CACHE_PATH_CAP];
    int found=cache_environment("MISRC_PLAYBACK_CACHE_DIR",value,sizeof(value));
    if(found<0) return false;
    if(found) return value[0] && cache_copy_path(directory,capacity,value,"");
    const char *names[]={"LOCALAPPDATA","USERPROFILE","TEMP"};
    const char *suffixes[]={"/MISRC/playback-length-v2","/AppData/Local/MISRC/playback-length-v2",
                           "/MISRC/playback-length-v2"};
    for(unsigned n=0;n<3;++n) {
        found=cache_environment(names[n],value,sizeof(value));
        if(found<0) return false;
        if(found && value[0]) return cache_copy_path(directory,capacity,value,suffixes[n]);
    }
    return false;
#else
    const char *override=getenv("MISRC_PLAYBACK_CACHE_DIR");
    if(override) return override[0] && cache_copy_path(directory,capacity,override,"");
    const char *base=getenv("XDG_CACHE_HOME");
    if(base && base[0]=='/') return cache_copy_path(directory,capacity,base,"/MISRC/playback-length-v2");
    base=getenv("HOME");
    if(base && base[0]) return cache_copy_path(directory,capacity,base,"/.cache/MISRC/playback-length-v2");
    base=getenv("TMPDIR");
    if(!base || base[0]!='/') base="/tmp";
    int n=snprintf(directory,capacity,"%s/misrc-playback-length-v2-%lu",base,(unsigned long)geteuid());
    return n>=0 && (size_t)n<capacity;
#endif
}
static bool cache_ensure_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide=cache_wide_path(path);
    if(!wide) return false;
    DWORD attributes=GetFileAttributesW(wide);
    bool ok=attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
    if(!ok) {
        ok=CreateDirectoryW(wide,NULL)!=0;
        if(!ok && GetLastError()==ERROR_ALREADY_EXISTS) {
            attributes=GetFileAttributesW(wide);
            ok=attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    free(wide); return ok;
#else
    struct stat info;
    if(stat(path,&info)==0) return S_ISDIR(info.st_mode);
    if(mkdir(path,0700)==0) return true;
    return errno==EEXIST && stat(path,&info)==0 && S_ISDIR(info.st_mode);
#endif
}
static bool cache_separator(char c) {
#ifdef _WIN32
    return c=='/' || c=='\\';
#else
    return c=='/';
#endif
}
static bool cache_make_directory(const char *path) {
    char prefix[CACHE_PATH_CAP];
    if(!cache_copy_path(prefix,sizeof(prefix),path,"")) return false;
    size_t length=strlen(prefix),skip=0;
#ifdef _WIN32
    if(length>=2 && cache_separator(prefix[0]) && cache_separator(prefix[1])) {
        skip=2;
        while(skip<length && !cache_separator(prefix[skip])) ++skip;
        ++skip; /* A UNC server alone is not a directory to create. */
    }
#endif
    for(size_t n=1;n<length;++n) {
        if(n<skip || !cache_separator(prefix[n]) || cache_separator(prefix[n-1])) continue;
        char saved=prefix[n]; prefix[n]=0;
        bool ok=cache_ensure_directory(prefix); prefix[n]=saved;
        if(!ok) return false;
    }
    return cache_ensure_directory(prefix);
}
static bool cache_path(const gui_playback_length_cache_key_t *key,char *directory,
                       char *path,size_t capacity) {
    if(!cache_valid_key(key) || !cache_directory(directory,CACHE_PATH_CAP)) return false;
    unsigned char encoded[64]; cache_encode_key(encoded,key);
    int n=snprintf(path,capacity,"%s/%016" PRIx64 ".bin",directory,cache_hash(encoded,sizeof(encoded)));
    return n>=0 && (size_t)n<capacity;
}
static bool cache_read_record(const char *path,unsigned char *record) {
#ifdef _WIN32
    wchar_t *wide=cache_wide_path(path);
    if(!wide) return false;
    HANDLE file=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    free(wide);
    if(file==INVALID_HANDLE_VALUE) return false;
    gui_playback_length_cache_key_t info={0};
    if(GetFileType(file)!=FILE_TYPE_DISK || !cache_metadata(file,&info) || info.file_size!=CACHE_RECORD_BYTES) {
        CloseHandle(file); return false;
    }
    DWORD count=0; unsigned char extra=0;
    bool ok=ReadFile(file,record,CACHE_RECORD_BYTES,&count,NULL)!=0 && count==CACHE_RECORD_BYTES;
    if(ok) ok=ReadFile(file,&extra,1,&count,NULL)!=0 && count==0;
    CloseHandle(file); return ok;
#else
    int file=open(path,O_RDONLY|O_NONBLOCK);
    if(file<0) return false;
    struct stat info;
    if(fstat(file,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size!=CACHE_RECORD_BYTES) {
        close(file); return false;
    }
    size_t total=0; bool ok=true;
    while(total<CACHE_RECORD_BYTES) {
        ssize_t n=read(file,record+total,CACHE_RECORD_BYTES-total);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ok=false; break; }
        total+=(size_t)n;
    }
    if(ok) {
        unsigned char extra;
        ssize_t n;
        do { n=read(file,&extra,1); } while(n<0 && errno==EINTR);
        ok=n==0;
    }
    close(file); return ok;
#endif
}
bool gui_playback_length_cache_lookup(const gui_playback_length_cache_key_t *key,
                                     uint64_t *samples) {
    if(!samples) return false;
    *samples=0;
    char directory[CACHE_PATH_CAP],path[CACHE_PATH_CAP];
    unsigned char record[CACHE_RECORD_BYTES],encoded[64];
    if(!cache_path(key,directory,path,sizeof(path)) || !cache_read_record(path,record)) return false;
    cache_encode_key(encoded,key);
    if(memcmp(record,"MISRCPLC",8) || cache_get32(record+8)!=CACHE_VERSION ||
       cache_get32(record+12)!=CACHE_RECORD_BYTES || memcmp(record+16,encoded,sizeof(encoded)) ||
       cache_get64(record+88)!=cache_hash(record,88)) return false;
    uint64_t count=cache_get64(record+80);
    if(!count) return false;
    *samples=count; return true;
}
static bool cache_write_record(const char *temporary,const unsigned char *record,bool *created) {
    *created=false;
#ifdef _WIN32
    wchar_t *wide=cache_wide_path(temporary);
    if(!wide) return false;
    HANDLE file=CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    free(wide);
    if(file==INVALID_HANDLE_VALUE) return false;
    *created=true;
    DWORD count=0;
    bool ok=WriteFile(file,record,CACHE_RECORD_BYTES,&count,NULL)!=0 && count==CACHE_RECORD_BYTES;
    if(ok) ok=FlushFileBuffers(file)!=0;
    if(!CloseHandle(file)) ok=false;
    return ok;
#else
    int file=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(file<0) return false;
    *created=true;
    size_t total=0; bool ok=true;
    while(total<CACHE_RECORD_BYTES) {
        ssize_t n=write(file,record+total,CACHE_RECORD_BYTES-total);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ok=false; break; }
        total+=(size_t)n;
    }
    if(ok) ok=fsync(file)==0;
    if(close(file)!=0) ok=false;
    return ok;
#endif
}
static void cache_remove_temporary(const char *path) {
#ifdef _WIN32
    wchar_t *wide=cache_wide_path(path);
    if(wide) { DeleteFileW(wide); free(wide); }
#else
    (void)unlink(path);
#endif
}
static bool cache_replace(const char *temporary,const char *path) {
#ifdef _WIN32
    wchar_t *from=cache_wide_path(temporary),*to=cache_wide_path(path);
    bool ok=from && to && MoveFileExW(from,to,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    free(from); free(to); return ok;
#else
    return rename(temporary,path)==0;
#endif
}
bool gui_playback_length_cache_store(const gui_playback_length_cache_key_t *key,
                                    uint64_t samples) {
    char directory[CACHE_PATH_CAP],path[CACHE_PATH_CAP],temporary[CACHE_PATH_CAP];
    if(!samples || !cache_path(key,directory,path,sizeof(path)) || !cache_make_directory(directory)) return false;
    unsigned char record[CACHE_RECORD_BYTES]={0};
    memcpy(record,"MISRCPLC",8); cache_put32(record+8,CACHE_VERSION);
    cache_put32(record+12,CACHE_RECORD_BYTES); cache_encode_key(record+16,key);
    cache_put64(record+80,samples); cache_put64(record+88,cache_hash(record,88));
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    uint64_t sequence=atomic_fetch_add_explicit(&cache_temporary_sequence,1,memory_order_relaxed);
    int n=snprintf(temporary,sizeof(temporary),"%s.tmp-%lu-%016" PRIx64,path,pid,sequence);
    if(n<0 || (size_t)n>=sizeof(temporary)) return false;
    /* CREATE_NEW/O_EXCL prevents sharing or truncating another writer's temp.
     * Publication replaces the complete record, never removes the old one. */
    bool created=false;
    if(!cache_write_record(temporary,record,&created)) {
        /* Remove only the temp this call created, never another writer's name. */
        if(created) cache_remove_temporary(temporary);
        return false;
    }
    bool ok=cache_replace(temporary,path);
    if(!ok) cache_remove_temporary(temporary);
    return ok;
}
