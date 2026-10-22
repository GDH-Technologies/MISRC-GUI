/* Headless recording/preview regression. GPL-3.0-or-later.
 * RF -> production capture ring -> identity extraction -> production record
 * rings -> shared flac_writer. This excludes USB, ADC parsing, gui_record's
 * spill/UI callbacks and rendering. Loop boundaries reset preview acquisition;
 * recording retains every sample and the final partial writer block.
 */
#include "../common/buffer_manager.h"
#include "../common/flac_writer.h"
#include "../common/threading.h"
#include "../misrc_gui/processing/gui_rf_preview_worker.h"
#include "../misrc_gui/processing/gui_rf_preview_pressure.h"
#include <FLAC/stream_decoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#define LOAD_SEEK _fseeki64
#define LOAD_TELL _ftelli64
#else
#include <sys/resource.h>
#include <time.h>
#define LOAD_SEEK fseeko
#define LOAD_TELL ftello
#endif

#define MIB ((size_t)1024*1024)
#define SOURCE_CAP (128u*MIB)
#define RING_BYTES (32u*MIB)
#define PRODUCER_SAMPLES 65536u
/* Same batching as GUI_RECORD_WRITER_BLOCK_BYTES in gui_record.c. */
#define RECORD_SAMPLES 1048576u
#define FRAME_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4)
#define PATH_CAP 1024
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

typedef struct load_state load_state_t;
typedef struct {
    load_state_t *load;
    buffer_id_t id;
    FILE *file;
    flac_writer_t *writer;
    thrd_t thread;
    atomic_uint_fast64_t samples;
    uint64_t cpu_us, wall_us, finish_us, stall_us;
    flac_writer_error_t finish_result;
    char path[PATH_CAP];
} record_t;
struct load_state {
    buffer_manager_t mgr;
    int16_t *source;
    size_t source_samples;
    uint32_t rate;
    uint64_t target, start_us;
    int channels;
    unsigned stall_ms;
    atomic_bool producer_done, extraction_done, failed;
    atomic_uint_fast64_t extracted, submitted[2], rejected[2];
    gui_rf_preview_worker_t *workers[2];
    record_t records[2];
};
typedef struct { uint64_t current, peak; bool available; } memory_t;
static uint64_t now_us(void) {
#ifdef _WIN32
    LARGE_INTEGER f,c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart/f.QuadPart)*1000000u+
           (uint64_t)(c.QuadPart%f.QuadPart)*1000000u/(uint64_t)f.QuadPart;
#else
    return get_time_us();
#endif
}
static uint64_t cpu_us(bool thread) {
#ifdef _WIN32
    FILETIME create,exit,kernel,user;
    bool ok=thread?GetThreadTimes(GetCurrentThread(),&create,&exit,&kernel,&user):
                   GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user);
    if (!ok) return 0;
    ULARGE_INTEGER k,u;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return (k.QuadPart+u.QuadPart)/10u;
#else
#if defined(CLOCK_THREAD_CPUTIME_ID)
    if (thread) { struct timespec t;
        if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))
            return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
    }
#endif
    struct rusage r;
    if (getrusage(RUSAGE_SELF,&r)) return 0;
    return (uint64_t)(r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000000u+
           (uint64_t)r.ru_utime.tv_usec+(uint64_t)r.ru_stime.tv_usec;
#endif
}
static memory_t memory(void) {
    memory_t m={0};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS p; memset(&p,0,sizeof(p)); p.cb=sizeof(p);
    if (GetProcessMemoryInfo(GetCurrentProcess(),&p,sizeof(p))) {
        m.current=p.WorkingSetSize; m.peak=p.PeakWorkingSetSize; m.available=true;
    }
#else
    struct rusage r;
    if (!getrusage(RUSAGE_SELF,&r)) {
#ifdef __APPLE__
        m.peak=(uint64_t)r.ru_maxrss;
#else
        m.peak=(uint64_t)r.ru_maxrss*1024u;
#endif
        m.available=true;
    }
#endif
    return m;
}
static void json_string(const char *s) {
    putchar('"');
    for (;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if (c=='"'||c=='\\') { putchar('\\'); putchar(c); }
        else if (c<32) printf("\\u%04x",(unsigned)c);
        else putchar(c);
    }
    putchar('"');
}
static void ju(const char *key,uint64_t value) { printf(",\"%s\":%llu",key,(unsigned long long)value); }
static void jd(const char *key,double value) { printf(",\"%s\":%.6f",key,value); }
static void jb(const char *key,bool value) { printf(",\"%s\":%s",key,value?"true":"false"); }
static void js(const char *key,const char *value) { printf(",\"%s\":",key); json_string(value); }
static void jhash(const char *key,uint64_t value) { printf(",\"%s\":\"%016llx\"",key,(unsigned long long)value); }
static bool finite_double(double value) {
    /* Keep argument validation effective with the GUI's -ffast-math flags. */
    uint64_t bits;
    _Static_assert(sizeof(bits)==sizeof(value),"IEEE-754 double required");
    memcpy(&bits,&value,sizeof(bits));
    return ((bits>>52)&0x7ffu)!=0x7ffu;
}
static uint64_t hash_sample(uint64_t hash,int16_t sample) {
    uint16_t u=(uint16_t)sample;
    hash=(hash^(u&255u))*FNV_PRIME;
    return (hash^(u>>8))*FNV_PRIME;
}
static void flac_error(void *context,flac_writer_error_t code,const char *message) {
    record_t *r=context;
    fprintf(stderr,"FLAC channel %d error %d: %s\n",r->id-BUF_RECORD_A,(int)code,message);
    atomic_store(&r->load->failed,true);
}
/* Ring head/tail wrap is updated in multiple stores. Pressure/peak observation
 * uses monotonic production byte counters, loading read before written, so a
 * transient wrap snapshot cannot appear to be a full or overflowing buffer. */
static size_t observed_fill(buffer_manager_t *m,buffer_id_t id) {
    uint64_t read=atomic_load(&m->stats[id].bytes_read);
    uint64_t written=atomic_load(&m->stats[id].bytes_written);
    uint64_t capacity=m->configs[id].size;
    uint64_t fill=written>=read?written-read:0;
    return (size_t)(fill>capacity?capacity:fill);
}
static unsigned percent(buffer_manager_t *m,buffer_id_t id) {
    uint64_t read=atomic_load(&m->stats[id].bytes_read);
    uint64_t written=atomic_load(&m->stats[id].bytes_written);
    return gui_rf_preview_pressure_fill_percent(written,read,m->configs[id].size);
}
static uint64_t drops(load_state_t *s) {
    uint64_t count=atomic_load(&s->mgr.stats[BUF_CAPTURE_RF].write_drops);
    for (int i=0;i<s->channels;i++) count+=atomic_load(&s->mgr.stats[BUF_RECORD_A+i].write_drops);
    return count;
}
static int encoder_thread(void *context) {
    record_t *r=context; load_state_t *s=r->load;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    uint64_t start=now_us(),cpu=cpu_us(true); bool stalled=false;
    for (;;) {
        if (!stalled&&s->stall_ms&&now_us()-s->start_us>=2000000u) {
            uint64_t before=now_us(); thrd_sleep_ms((int)s->stall_ms);
            r->stall_us=now_us()-before; stalled=true;
        }
        bool done=atomic_load(&s->extraction_done);
        size_t fill=bufmgr_fill_level(&s->mgr,r->id);
        if (done&&fill==0) break;
        size_t count=RECORD_SAMPLES;
        if (done&&fill<count*sizeof(int16_t)) count=fill/sizeof(int16_t);
        if (!count) continue;
        int16_t *in=bufmgr_read_begin(&s->mgr,r->id,count*sizeof(int16_t),20);
        if (!in) { if (atomic_load(&s->failed)) break; continue; }
        if (flac_writer_process_int16(r->writer,in,(uint32_t)count)!=(int)count) {
            atomic_store(&s->failed,true); break;
        }
        atomic_fetch_add(&r->samples,count);
        bufmgr_read_end(&s->mgr,r->id,count*sizeof(int16_t));
    }
    r->wall_us=now_us()-start; r->cpu_us=cpu_us(true)-cpu;
    uint64_t before=now_us();
    r->finish_result=flac_writer_finish(r->writer); r->writer=NULL;
    r->finish_us=now_us()-before;
    if (r->finish_result!=FLAC_WRITER_OK) atomic_store(&s->failed,true);
    if (fclose(r->file)) atomic_store(&s->failed,true);
    r->file=NULL;
    return atomic_load(&s->failed)?1:0;
}
static int extract_thread(void *context) {
    load_state_t *s=context; uint64_t consumed=0;
    for (;;) {
        bool done=atomic_load(&s->producer_done);
        size_t fill=bufmgr_fill_level(&s->mgr,BUF_CAPTURE_RF);
        if (done&&fill==0) break;
        size_t count=PRODUCER_SAMPLES;
        if (done&&fill<count*sizeof(int16_t)) count=fill/sizeof(int16_t);
        if (!count) continue;
        int16_t *in=bufmgr_read_begin(&s->mgr,BUF_CAPTURE_RF,count*sizeof(int16_t),20);
        if (!in) { if (atomic_load(&s->failed)) break; continue; }
        for (int i=0;i<s->channels;i++) {
            if (bufmgr_write(&s->mgr,BUF_RECORD_A+i,in,count*sizeof(int16_t))) {
                atomic_store(&s->failed,true); break;
            }
        }
        /* Record first; preview submit never waits for decoding or queue space. */
        for (size_t offset=0;offset<count;) {
            uint64_t position=consumed+offset;
            size_t within=(size_t)(position%s->source_samples);
            size_t segment=s->source_samples-within;
            if (segment>count-offset) segment=count-offset;
            gui_rf_block_info_t info={s->rate,position,position/s->source_samples+1,within==0};
            for (int i=0;i<s->channels;i++) if (s->workers[i]) {
                if (gui_rf_preview_worker_submit(s->workers[i],in+offset,segment,&info))
                    atomic_fetch_add(&s->submitted[i],1);
                else atomic_fetch_add(&s->rejected[i],1);
            }
            offset+=segment;
        }
        bufmgr_read_end(&s->mgr,BUF_CAPTURE_RF,count*sizeof(int16_t));
        consumed+=count; atomic_store(&s->extracted,consumed);
        if (atomic_load(&s->failed)) break;
    }
    atomic_store(&s->extraction_done,true);
    for (int i=0;i<s->channels;i++) bufmgr_signal_data(&s->mgr,BUF_RECORD_A+i);
    return atomic_load(&s->failed)?1:0;
}
typedef struct {
    const int16_t *source;
    size_t source_samples;
    uint64_t samples,hash,mismatches;
    bool error;
} verify_t;
static FLAC__StreamDecoderWriteStatus decode_write(const FLAC__StreamDecoder *d,
    const FLAC__Frame *frame,const FLAC__int32 *const data[],void *context) {
    (void)d; verify_t *v=context;
    if (frame->header.channels!=1||frame->header.bits_per_sample!=16) {
        v->error=true; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    size_t index=(size_t)(v->samples%v->source_samples);
    for (uint32_t i=0;i<frame->header.blocksize;i++) {
        if (data[0][i]!=(int32_t)v->source[index]) v->mismatches++;
        v->hash=hash_sample(v->hash,(int16_t)data[0][i]);
        if (++index==v->source_samples) index=0;
    }
    v->samples+=frame->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void decode_error(const FLAC__StreamDecoder *d,FLAC__StreamDecoderErrorStatus status,void *context) {
    (void)d; (void)status; ((verify_t *)context)->error=true;
}
static bool verify_file(const char *path,verify_t *v) {
    FLAC__StreamDecoder *d=FLAC__stream_decoder_new(); if (!d) return false;
    FLAC__stream_decoder_set_md5_checking(d,true);
    bool ok=FLAC__stream_decoder_init_file(d,path,decode_write,NULL,decode_error,v)==FLAC__STREAM_DECODER_INIT_STATUS_OK;
    if (ok) ok=FLAC__stream_decoder_process_until_end_of_stream(d);
    if (!FLAC__stream_decoder_finish(d)) ok=false;
    FLAC__stream_decoder_delete(d);
    return ok&&!v->error&&!v->mismatches;
}
static bool save_ppm(const char *path,const uint8_t *rgba,int width,int height) {
    FILE *f=fopen(path,"wb"); if (!f) return false;
    bool ok=fprintf(f,"P6\n%d %d\n255\n",width,height)>0;
    for (int y=0;y<height&&ok;y++) for (int x=0;x<width&&ok;x++)
        ok=fwrite(rgba+((size_t)y*width+x)*4,1,3,f)==3;
    if (fclose(f)) ok=false;
    return ok;
}
static int setup_record(load_state_t *s,int i,const char *prefix,int threads,bool preview,gui_rf_format_t format) {
    record_t *r=&s->records[i]; r->load=s; r->id=BUF_RECORD_A+i;
    snprintf(r->path,sizeof(r->path),"%s.%c.flac",prefix,'A'+i);
    if (bufmgr_ensure_init(&s->mgr,r->id)) return 1;
    r->file=fopen(r->path,"w+b"); if (!r->file) return 1;
    flac_writer_config_t config=flac_writer_default_config();
    config.sample_rate=s->rate/1000u; config.bits_per_sample=16;
    config.compression_level=8; config.num_threads=(uint32_t)threads; config.verify=false;
    config.error_cb=flac_error; config.callback_user_data=r;
    r->writer=flac_writer_create_stream(r->file,&config); if (!r->writer) return 1;
    if (preview) { s->workers[i]=gui_rf_preview_worker_create(format); if (!s->workers[i]) return 1; }
    return 0;
}
int main(int argc,char **argv) {
    if (argc<9||argc>11) {
        fprintf(stderr,"Usage: %s source.s16le FsHz pal|ntsc prefix seconds on|off channels flac_threads [core_budget] [consumer_stall_ms]\n",argv[0]); return 1;
    }
    char *end=NULL; unsigned long rate=strtoul(argv[2],&end,10);
    if (!end||*end||rate<16000000u||rate>200000000u) return 1;
    bool preview=!strcmp(argv[6],"on");
    if (!preview&&strcmp(argv[6],"off")) return 1;
    int channels=atoi(argv[7]),threads=atoi(argv[8]);
    double seconds=strtod(argv[5],&end),budget=1.0;
    if (end==argv[5]||*end||!finite_double(seconds)||seconds<0.1||seconds>120||channels<1||channels>2||threads<1||threads>16) return 1;
    if (argc>=10) {
        budget=strtod(argv[9],&end);
        if (end==argv[9]||*end) return 1;
    }
    gui_rf_format_t format=!strcmp(argv[3],"pal")?GUI_RF_PAL:GUI_RF_NTSC;
    if (strcmp(argv[3],"pal")&&strcmp(argv[3],"ntsc")) return 1;
    if (!gui_rf_preview_worker_set_budget(budget)) return 1;
    unsigned stall_ms=argc==11?(unsigned)strtoul(argv[10],NULL,10):0;
    if (stall_ms>1500) return 1;
    load_state_t s; memset(&s,0,sizeof(s));
    atomic_init(&s.producer_done,false);
    atomic_init(&s.extraction_done,false);
    atomic_init(&s.failed,false);
    atomic_init(&s.extracted,0);
    for (int i=0;i<2;i++) {
        atomic_init(&s.submitted[i],0);
        atomic_init(&s.rejected[i],0);
        atomic_init(&s.records[i].samples,0);
    }
    s.rate=(uint32_t)rate; s.channels=channels; s.stall_ms=stall_ms;
    s.target=(uint64_t)(seconds*rate+0.5);
    FILE *input=fopen(argv[1],"rb"); if (!input) { perror("RF input"); return 1; }
    LOAD_SEEK(input,0,SEEK_END); int64_t length=LOAD_TELL(input); LOAD_SEEK(input,0,SEEK_SET);
    if (length<=0||(length&1)||(uint64_t)length>SOURCE_CAP) {
        fprintf(stderr,"Source must be s16le, at most 128 MiB; use a 2-second RF window\n"); fclose(input); return 1;
    }
    s.source=malloc((size_t)length); s.source_samples=(size_t)length/2;
    if (!s.source||fread(s.source,1,(size_t)length,input)!=(size_t)length) { fclose(input); free(s.source); return 1; }
    fclose(input);
    uint8_t *raw=(uint8_t *)s.source;
    for (size_t i=0;i<s.source_samples;i++) s.source[i]=(int16_t)((uint16_t)raw[2*i]|((uint16_t)raw[2*i+1]<<8));
    memory_t source_memory=memory();
    buffer_config_t configs[BUF_COUNT];
    const char *names[BUF_COUNT]={"load_capture_rf","load_capture_audio","load_record_a","load_record_b","load_display"};
    for (int i=0;i<BUF_COUNT;i++) configs[i]=(buffer_config_t){names[i],RING_BYTES,true};
    configs[BUF_CAPTURE_RF].lazy_init=false;
    if (bufmgr_init_custom(&s.mgr,configs)) { free(s.source); return 1; }
    uint8_t *frame=malloc(FRAME_BYTES); int16_t *block=malloc(PRODUCER_SAMPLES*sizeof(int16_t));
    char csv_path[PATH_CAP]; snprintf(csv_path,sizeof(csv_path),"%s.trace.csv",argv[4]);
    FILE *csv=fopen(csv_path,"wb");
    if (!frame||!block||!csv) { fprintf(stderr,"Harness allocation failed\n"); return 1; }
    fputs("seconds,current_rss,process_cpu_us,raw_fill,record_a_fill,record_b_fill,preview_paused,frames_a,drops_a\r\n",csv);
    for (int i=0;i<channels;i++) if (setup_record(&s,i,argv[4],threads,preview,format)) return 1;
    memory_t ready_memory=memory();
    uint64_t cpu_start=cpu_us(false),start=now_us(); s.start_us=start;
    for (int i=0;i<channels;i++)
        if (thrd_create_with_priority(&s.records[i].thread,encoder_thread,&s.records[i],THRD_PRIORITY_CRITICAL)!=thrd_success) return 1;
    thrd_t extraction;
    if (thrd_create_with_priority(&extraction,extract_thread,&s,THRD_PRIORITY_NORMAL)!=thrd_success) return 1;
#ifdef _WIN32
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,0x00000002u,TIMER_ALL_ACCESS);
    if (!timer) timer=CreateWaitableTimerW(NULL,FALSE,NULL);
#endif
    uint64_t produced=0,next_trace=0,generation[2]={0,0},pause_start=0,pause_us=0;
    uint64_t pauses=0,resumes=0; size_t raw_peak=0,record_peak[2]={0,0};
    gui_rf_preview_pressure_t pressure={0};
    gui_rf_preview_stats_t stats[2]; memset(stats,0,sizeof(stats));
    int width[2]={0,0},height[2]={0,0};
    while (produced<s.target&&!atomic_load(&s.failed)) {
        size_t count=PRODUCER_SAMPLES;
        if (s.target-produced<count) count=(size_t)(s.target-produced);
        for (size_t copied=0;copied<count;) {
            size_t offset=(size_t)((produced+copied)%s.source_samples),n=s.source_samples-offset;
            if (n>count-copied) n=count-copied;
            memcpy(block+copied,s.source+offset,n*sizeof(int16_t)); copied+=n;
        }
        if (bufmgr_write(&s.mgr,BUF_CAPTURE_RF,block,count*sizeof(int16_t))) { atomic_store(&s.failed,true); break; }
        produced+=count; uint64_t now=now_us(); unsigned record_percent=0;
        size_t fill=observed_fill(&s.mgr,BUF_CAPTURE_RF); if (fill>raw_peak) raw_peak=fill;
        for (int i=0;i<channels;i++) {
            fill=observed_fill(&s.mgr,BUF_RECORD_A+i); if (fill>record_peak[i]) record_peak[i]=fill;
            unsigned value=percent(&s.mgr,BUF_RECORD_A+i); if (value>record_percent) record_percent=value;
        }
        bool old_paused=pressure.paused;
        bool paused=gui_rf_preview_pressure_update(&pressure,(now-start)/1000u,
            percent(&s.mgr,BUF_CAPTURE_RF),record_percent,drops(&s),true);
        if (paused&&!old_paused) { pauses++; pause_start=now; }
        if (!paused&&old_paused) { resumes++; pause_us+=now-pause_start; }
        for (int i=0;i<channels;i++) if (s.workers[i]) {
            gui_rf_preview_worker_set_paused(s.workers[i],paused);
            (void)gui_rf_preview_worker_take_frame(s.workers[i],frame,FRAME_BYTES,
                &generation[i],&width[i],&height[i],&stats[i]);
        }
        if (now-start>=next_trace) {
            memory_t m=memory();
            fprintf(csv,"%.6f,%llu,%llu,%zu,%zu,%zu,%u,%llu,%llu\r\n",
                (double)(now-start)/1000000.0,(unsigned long long)m.current,
                (unsigned long long)(cpu_us(false)-cpu_start),observed_fill(&s.mgr,BUF_CAPTURE_RF),
                observed_fill(&s.mgr,BUF_RECORD_A),channels==2?observed_fill(&s.mgr,BUF_RECORD_B):0,
                paused?1u:0u,(unsigned long long)stats[0].total_frames,(unsigned long long)stats[0].blocks_dropped);
            next_trace+=1000000u;
        }
        uint64_t deadline=start+(produced/s.rate)*1000000u+(produced%s.rate)*1000000u/s.rate;
        for (;;) {
            now=now_us(); if (now>=deadline) break;
#ifdef _WIN32
            if (timer) { LARGE_INTEGER due; due.QuadPart=-(LONGLONG)(deadline-now)*10;
                SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE); WaitForSingleObject(timer,1000); }
            else thrd_sleep_ms(1);
#else
            thrd_sleep_ms(deadline-now>=2000?(int)((deadline-now)/1000u)-1:1);
#endif
        }
    }
    if (pressure.paused) pause_us+=now_us()-pause_start;
    atomic_store(&s.producer_done,true); bufmgr_signal_data(&s.mgr,BUF_CAPTURE_RF);
    uint64_t producer_end=now_us(); int thread_result=0;
    thrd_join(extraction,&thread_result);
    for (int i=0;i<channels;i++) thrd_join(s.records[i].thread,&thread_result);
    uint64_t record_end=now_us();
    for (int i=0;i<channels;i++) if (s.workers[i]) {
        if (!gui_rf_preview_worker_wait_idle(s.workers[i],30000)) atomic_store(&s.failed,true);
        gui_rf_preview_worker_get_stats(s.workers[i],&stats[i]);
        uint64_t copy_generation=0;
        if (gui_rf_preview_worker_take_frame(s.workers[i],frame,FRAME_BYTES,
                &copy_generation,&width[i],&height[i],&stats[i])) {
            char path[PATH_CAP]; snprintf(path,sizeof(path),"%s.%c.ppm",argv[4],'A'+i);
            if (!save_ppm(path,frame,width[i],height[i])) atomic_store(&s.failed,true);
        }
    }
    uint64_t processing_end=now_us(),process_cpu=cpu_us(false)-cpu_start;
    memory_t final_memory=memory();
#ifdef _WIN32
    if (timer) CloseHandle(timer);
#endif
    fclose(csv);
    /* Hashing and libFLAC decode verification are outside measured recording. */
    uint64_t expected_hash=FNV_OFFSET;
    for (uint64_t n=0;n<produced;) {
        size_t count=s.source_samples; if (produced-n<count) count=(size_t)(produced-n);
        for (size_t j=0;j<count;j++) expected_hash=hash_sample(expected_hash,s.source[j]);
        n+=count;
    }
    verify_t verified[2]; memset(verified,0,sizeof(verified));
    bool verify_ok[2]={false,false}; uint64_t encoded_bytes[2]={0,0};
    uint64_t verify_start=now_us();
    for (int i=0;i<channels;i++) {
        verified[i].source=s.source; verified[i].source_samples=s.source_samples; verified[i].hash=FNV_OFFSET;
        verify_ok[i]=verify_file(s.records[i].path,&verified[i])&&verified[i].samples==produced&&verified[i].hash==expected_hash;
        FILE *f=fopen(s.records[i].path,"rb");
        if (f) { LOAD_SEEK(f,0,SEEK_END); int64_t bytes=LOAD_TELL(f); if (bytes>0) encoded_bytes[i]=(uint64_t)bytes; fclose(f); }
        if (!verify_ok[i]) atomic_store(&s.failed,true);
    }
    printf("{\"source\":"); json_string(argv[1]);
    js("boundary","headless production bufmgr/flac_writer/worker with identity extraction; excludes USB, parser, GUI spill and renderer");
    ju("sample_rate_hz",s.rate); ju("source_preloaded_bytes",(uint64_t)length);
    jd("source_seconds",(double)s.source_samples/s.rate); ju("target_samples_per_channel",s.target);
    ju("produced_samples",produced); ju("extracted_samples",atomic_load(&s.extracted));
    jb("preview_enabled",preview); ju("channels",(unsigned)channels); ju("flac_threads_per_channel",(unsigned)threads);
    jb("channels_duplicate_same_rf_source",channels==2);
    ju("identity_capture_s16le_bytes_per_second",(uint64_t)s.rate*2u);
    ju("record_s16le_bytes_per_second_total",(uint64_t)s.rate*2u*(unsigned)channels);
    ju("flac_level",8); ju("container_sample_rate_hz",s.rate/1000u); jd("shared_preview_core_budget",budget);
    ju("source_loop_discontinuities",produced?((produced-1)/s.source_samples):0);
    jd("producer_wall_ms",(double)(producer_end-start)/1000.0);
    jd("recording_wall_ms",(double)(record_end-start)/1000.0);
    jd("processing_wall_ms",(double)(processing_end-start)/1000.0);
    jd("process_cpu_ms",(double)process_cpu/1000.0);
    jd("average_logical_cores",(double)process_cpu/(processing_end-start));
    jd("verification_wall_ms",(double)(now_us()-verify_start)/1000.0);
    jb("memory_available",final_memory.available);
    ju("source_baseline_rss_bytes",source_memory.current); ju("ready_rss_bytes",ready_memory.current);
    ju("processing_end_rss_bytes",final_memory.current); ju("peak_rss_bytes",final_memory.peak);
    ju("peak_increment_vs_source_baseline",final_memory.peak>source_memory.current?final_memory.peak-source_memory.current:0);
    ju("capture_buffer_bytes",RING_BYTES); ju("record_buffer_bytes_per_channel",RING_BYTES);
    ju("raw_peak_fill_bytes",raw_peak); ju("capture_write_drops",atomic_load(&s.mgr.stats[BUF_CAPTURE_RF].write_drops));
    ju("pause_transitions",pauses); ju("resume_transitions",resumes); jd("paused_wall_ms",(double)pause_us/1000.0);
    ju("consumer_stall_requested_ms",stall_ms); jhash("expected_s16le_fnv1a64",expected_hash);
    printf(",\"record\":[");
    for (int i=0;i<channels;i++) {
        if (i) putchar(',');
        record_t *r=&s.records[i];
        printf("{\"path\":"); json_string(r->path);
        ju("samples_encoded",atomic_load(&r->samples)); ju("samples_decoded",verified[i].samples);
        ju("decoded_mismatches",verified[i].mismatches); jhash("decoded_fnv1a64",verified[i].hash);
        jb("md5_and_exact_sample_match",verify_ok[i]); ju("encoded_bytes",encoded_bytes[i]);
        ju("write_drops",atomic_load(&s.mgr.stats[r->id].write_drops));
        ju("write_waits",atomic_load(&s.mgr.stats[r->id].write_waits)); ju("peak_fill_bytes",record_peak[i]);
        jd("consumer_cpu_ms_excludes_libflac_threads",(double)r->cpu_us/1000.0);
        jd("consumer_wall_ms",(double)r->wall_us/1000.0); jd("finish_ms",(double)r->finish_us/1000.0);
        jd("stall_actual_ms",(double)r->stall_us/1000.0); putchar('}');
    }
    printf("],\"preview\":[");
    for (int i=0;i<channels;i++) {
        if (i) putchar(',');
        gui_rf_preview_stats_t *p=&stats[i];
        printf("{\"channel\":%d",i);
        ju("submitted",atomic_load(&s.submitted[i])); ju("rejected",atomic_load(&s.rejected[i]));
        ju("blocks_dropped",p->blocks_dropped); ju("pause_drops",p->pause_drops);
        ju("frames",p->total_frames); ju("fields",p->total_fields);
        ju("rejected_fields",p->total_fields_rejected); ju("color_losses",p->total_color_losses);
        jd("processing_cpu_ms",p->processing_cpu_ms); jd("processing_wall_ms",p->processing_wall_ms);
        jd("budget_wait_ms",p->budget_wait_ms);
        jb("h_locked",p->decode.h_locked); jb("v_locked",p->decode.v_locked); jb("color_locked",p->decode.color_locked);
        js("reason",p->decode.reason); putchar('}');
    }
    printf("]"); bool passed=!atomic_load(&s.failed)&&produced==s.target;
    jb("passed",passed); puts("}");
    for (int i=0;i<channels;i++) gui_rf_preview_worker_destroy(s.workers[i]);
    bufmgr_cleanup(&s.mgr); free(s.source); free(frame); free(block);
    return passed?0:1;
}
