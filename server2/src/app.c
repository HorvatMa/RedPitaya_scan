#include "app.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "recon/recon.h"
#include "util/ring.h"

#define RING_RECORDS  (1u << 22)      /* 4 M records = 32 MB, ~1.7 s at 2.5 Mrec/s */
#define CHUNK         65536
#define N_SLOTS       3               /* reconstructing / ready / sending */

enum { SLOT_FREE, SLOT_READY, SLOT_SENDING };

typedef struct {
    app_frame_t f;
    size_t      cap;                  /* floats allocated in f.data */
    int         state;
} slot_t;

struct app {
    hw_t            *hw;
    pthread_mutex_t  lock;            /* config, state, slots, counters below */
    pthread_cond_t   frame_cv;

    config_t         cfg;
    derived_t       *der;
    int              running;
    int              hw_error;

    /* threads */
    pthread_t        acq_th, rec_th;
    atomic_int       stop_req;
    ring_t           ring;

    /* live reconfiguration (img.roi) */
    atomic_int       recon_pending;
    recon_cfg_t      pending_cfg;     /* under lock */
    float            pending_roi[4];

    /* acquisition statistics */
    atomic_ullong    records_total, records_lost;
    atomic_ulong     chunks_lost, loss_events;

    /* frames */
    slot_t           slots[N_SLOTS];
    int              ready;           /* index of the READY slot or -1 */
    uint32_t         next_frame_id;
    uint64_t         frames_made, frames_dropped, frames_sent;
    uint64_t         last_frame_ns;

    /* recon-thread state */
    recon_t         *recon;
    unsigned long    seen_loss;
    int              lost_in_frame;
    float            cur_roi[4];
    float            slow_freq, fast_freq;
    recon_fcal_t     fcal;            /* fast-axis calibration of this run; written by the recon
                                         thread under the lock, kept across live rebuilds */

    /* status rate */
    uint64_t         st_records;
    double           st_t, st_cpu;
};

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static double cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

app_t *app_create(hw_t *hw)
{
    app_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->der = malloc(sizeof(*a->der));
    if (!a->der || ring_init(&a->ring, RING_RECORDS)) {
        free(a->der);
        free(a);
        return NULL;
    }
    a->hw = hw;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->frame_cv, NULL);
    config_defaults(&a->cfg);
    a->ready = -1;
    a->st_t = mono_s();
    a->st_cpu = cpu_s();
    return a;
}

void app_destroy(app_t *a)
{
    if (!a)
        return;
    app_stop(a);
    recon_destroy(a->recon);
    for (int i = 0; i < N_SLOTS; i++)
        free(a->slots[i].f.data);
    ring_free(&a->ring);
    free(a->der);
    pthread_cond_destroy(&a->frame_cv);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

/* ------------------------------------------------------------------ threads */

static void *acq_thread(void *arg)
{
    app_t    *a = arg;
    record_t *buf = malloc(CHUNK * sizeof(*buf));
    if (!buf)
        return NULL;
    while (!atomic_load(&a->stop_req)) {
        long n = hw_read(a->hw, buf, CHUNK, 100);
        if (n < 0) {
            pthread_mutex_lock(&a->lock);
            a->hw_error = 1;
            pthread_mutex_unlock(&a->lock);
            break;
        }
        if (n == 0)
            continue;
        atomic_fetch_add(&a->records_total, (unsigned long long)n);
        if (ring_write(&a->ring, buf, (size_t)n) == 0) {     /* reconstruction fell behind */
            atomic_fetch_add(&a->chunks_lost, 1);
            atomic_fetch_add(&a->records_lost, (unsigned long long)n);
            atomic_fetch_add(&a->loss_events, 1);
        }
    }
    free(buf);
    return NULL;
}

/* recon callback: publish the frame as READY, replacing (dropping) an unsent one */
static void on_frame(void *ctx, const frame_t *fr)
{
    app_t *a = ctx;
    uint64_t now = mono_ns();
    size_t   n = (size_t)fr->n_images * fr->width * fr->height;

    pthread_mutex_lock(&a->lock);
    uint32_t id = a->next_frame_id++;
    a->frames_made++;
    int s = -1;
    for (int i = 0; i < N_SLOTS; i++)
        if (a->slots[i].state == SLOT_FREE) {
            s = i;
            break;
        }
    if (s < 0) {                                  /* cannot happen with 3 slots; be safe */
        a->frames_dropped++;
        pthread_mutex_unlock(&a->lock);
        return;
    }
    slot_t *sl = &a->slots[s];
    if (sl->cap < n) {
        float *p = realloc(sl->f.data, n * sizeof(float));
        if (!p) {
            a->frames_dropped++;
            pthread_mutex_unlock(&a->lock);
            return;
        }
        sl->f.data = p;
        sl->cap = n;
    }
    memcpy(sl->f.data, fr->data, n * sizeof(float));
    sl->f.frame_id = id;
    sl->f.width = fr->width;
    sl->f.height = fr->height;
    sl->f.n_images = fr->n_images;
    sl->f.flags = a->lost_in_frame ? FRAME_FLAG_DATA_LOST : 0;
    sl->f.dtype = (uint8_t)a->cfg.dtype;          /* live key: read at publication time */
    sl->f.t_start_ns = a->last_frame_ns;
    sl->f.t_end_ns = now;
    sl->f.n_records = (uint32_t)fr->n_records;
    sl->f.n_discarded = (uint32_t)(fr->n_records - fr->n_used);
    sl->f.n_empty = fr->n_empty;
    sl->f.slow_freq = a->slow_freq;
    sl->f.fast_freq = a->fast_freq;
    memcpy(sl->f.roi, a->cur_roi, sizeof(sl->f.roi));
    a->last_frame_ns = now;
    a->lost_in_frame = 0;

    if (a->ready >= 0) {
        a->slots[a->ready].state = SLOT_FREE;     /* sender too slow: drop the older frame */
        a->frames_dropped++;
    }
    sl->state = SLOT_READY;
    a->ready = s;
    pthread_cond_signal(&a->frame_cv);
    pthread_mutex_unlock(&a->lock);
}

static void *recon_thread(void *arg)
{
    app_t    *a = arg;
    record_t *buf = malloc(CHUNK * sizeof(*buf));
    if (!buf)
        return NULL;
    while (!atomic_load(&a->stop_req)) {
        if (atomic_exchange(&a->recon_pending, 0)) {          /* new window: rebuild */
            pthread_mutex_lock(&a->lock);
            recon_cfg_t rc = a->pending_cfg;
            memcpy(a->cur_roi, a->pending_roi, sizeof(a->cur_roi));
            if (a->fcal.state == FCAL_DONE) {               /* reuse this run's fit */
                rc.fast_auto = 0;
                rc.fast_gain = a->fcal.gain;
                rc.fast_offset = a->fcal.offset;
                rc.fast_cycles = a->fcal.fast_hz / rc.sample_rate;   /* measured if external */
            }
            pthread_mutex_unlock(&a->lock);
            recon_t *nr = recon_create(&rc, on_frame, a);
            if (nr) {
                recon_destroy(a->recon);
                a->recon = nr;
                recon_fcal_t f;
                recon_fast_cal(nr, &f);
                pthread_mutex_lock(&a->lock);
                if (a->fcal.state == FCAL_DONE) {           /* the window sets levels and width */
                    a->fcal.levels = f.levels;
                    a->fcal.width = f.width;
                } else {
                    a->fcal = f;
                }
                pthread_mutex_unlock(&a->lock);
            }
        }
        size_t n = ring_read(&a->ring, buf, CHUNK);
        if (n == 0) {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        unsigned long loss = atomic_load(&a->loss_events);
        if (loss != a->seen_loss) {
            a->seen_loss = loss;
            a->lost_in_frame = 1;          /* records missing inside the current frame */
        }
        recon_push(a->recon, buf, n);
        /* only this thread changes it; external drive retries after FAILED */
        if (a->fcal.state == FCAL_PENDING || a->fcal.state == FCAL_FAILED) {
            recon_fcal_t f;
            recon_fast_cal(a->recon, &f);
            if (f.state != FCAL_PENDING) {
                pthread_mutex_lock(&a->lock);
                a->fcal = f;
                if (f.state == FCAL_DONE)
                    a->fast_freq = (float)f.fast_hz;       /* frame headers: measured if external */
                pthread_mutex_unlock(&a->lock);
            }
        }
    }
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ start / stop */

static void roi_to_float(const config_t *c, float out[4])
{
    for (int i = 0; i < 4; i++)
        out[i] = (float)c->roi[i];
}

/* called with the lock held */
static int start_locked(app_t *a, char *out, size_t outlen)
{
    char err[256];
    if (a->running) {
        snprintf(out, outlen, "ERR 3 already running");
        return -1;
    }
    if (config_derive(&a->cfg, a->der, err, sizeof(err))) {
        snprintf(out, outlen, "ERR 2 %s", err);
        return -1;
    }
    if (hw_configure(a->hw, &a->der->hw)) {
        snprintf(out, outlen, "ERR 4 hardware configuration failed");
        return -1;
    }
    recon_t *r = recon_create(&a->der->recon, on_frame, a);
    if (!r) {
        snprintf(out, outlen, "ERR 5 cannot create reconstruction (%ux%u)",
                 a->der->recon.width, a->der->recon.height);
        return -1;
    }
    recon_destroy(a->recon);
    a->recon = r;
    recon_fast_cal(r, &a->fcal);
    ring_reset(&a->ring);
    atomic_store(&a->records_total, 0);
    atomic_store(&a->records_lost, 0);
    atomic_store(&a->chunks_lost, 0);
    atomic_store(&a->loss_events, 0);
    atomic_store(&a->recon_pending, 0);
    atomic_store(&a->stop_req, 0);
    a->seen_loss = 0;
    a->lost_in_frame = 0;
    a->next_frame_id = 0;
    a->frames_made = a->frames_dropped = a->frames_sent = 0;
    a->hw_error = 0;
    a->slow_freq = (float)a->der->slow.freq_actual;
    a->fast_freq = (float)a->der->fast.freq_actual;
    roi_to_float(&a->cfg, a->cur_roi);
    a->st_records = 0;
    a->st_t = mono_s();

    if (hw_start(a->hw)) {
        snprintf(out, outlen, "ERR 4 hardware start failed");
        return -1;
    }
    a->last_frame_ns = mono_ns();
    if (pthread_create(&a->acq_th, NULL, acq_thread, a)) {
        hw_stop(a->hw);
        snprintf(out, outlen, "ERR 5 thread creation failed");
        return -1;
    }
    if (pthread_create(&a->rec_th, NULL, recon_thread, a)) {
        atomic_store(&a->stop_req, 1);
        pthread_join(a->acq_th, NULL);
        hw_stop(a->hw);
        snprintf(out, outlen, "ERR 5 thread creation failed");
        return -1;
    }
    a->running = 1;
    snprintf(out, outlen, "OK");
    return 0;
}

void app_stop(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    int was = a->running;
    a->running = 0;
    pthread_mutex_unlock(&a->lock);
    if (!was)
        return;
    atomic_store(&a->stop_req, 1);
    pthread_join(a->acq_th, NULL);              /* not under the lock: on_frame() needs it */
    pthread_join(a->rec_th, NULL);
    hw_stop(a->hw);
    pthread_mutex_lock(&a->lock);
    if (a->ready >= 0) {                         /* an unsent frame of this run is discarded */
        a->slots[a->ready].state = SLOT_FREE;
        a->ready = -1;
    }
    pthread_mutex_unlock(&a->lock);
}

/* ------------------------------------------------------------------ commands */

static void set_locked(app_t *a, const char *key, const char *val, char *out, size_t outlen)
{
    char err[256];
    int  live = config_is_live(key);
    if (a->running && !live) {
        snprintf(out, outlen, "ERR 3 %s can only be changed while stopped", key);
        return;
    }
    config_t trial = a->cfg;
    if (config_set(&trial, key, val, err, sizeof(err))) {
        snprintf(out, outlen, "ERR 2 %s", err);
        return;
    }
    if (a->running && strcmp(key, "img.roi") == 0) {   /* live window: validate and hand to recon */
        derived_t *d = malloc(sizeof(*d));
        if (!d) {
            snprintf(out, outlen, "ERR 5 out of memory");
            return;
        }
        if (config_derive(&trial, d, err, sizeof(err))) {
            snprintf(out, outlen, "ERR 2 %s", err);
            free(d);
            return;
        }
        a->pending_cfg = d->recon;
        roi_to_float(&trial, a->pending_roi);
        atomic_store(&a->recon_pending, 1);
        free(d);
    }
    a->cfg = trial;
    snprintf(out, outlen, "OK");
}

static void get_config_locked(app_t *a, char *out, size_t outlen)
{
    char err[256];
    derived_t *d = malloc(sizeof(*d));
    int ok = d && config_derive(&a->cfg, d, err, sizeof(err)) == 0;
    int n = snprintf(out, outlen, "OK\n");
    config_format(&a->cfg, ok ? d : NULL, out + n, outlen - n);
    if (!ok && d) {
        size_t len = strlen(out);
        snprintf(out + len, outlen - len, "\nconfig_error=%s", err);
    }
    free(d);
}

void app_command(app_t *a, const char *line, char *out, size_t outlen)
{
    char cmd[32] = "", key[64] = "";
    int  pos = 0;
    while (*line == ' ' || *line == '\t')
        line++;
    sscanf(line, "%31s %n", cmd, &pos);

    if (strcmp(cmd, "ping") == 0) {
        snprintf(out, outlen, "OK pong");
    } else if (strcmp(cmd, "set") == 0) {
        int kpos = 0;
        const char *rest = line + pos;
        if (sscanf(rest, "%63s %n", key, &kpos) != 1 || !rest[kpos]) {
            snprintf(out, outlen, "ERR 2 usage: set <key> <value>");
            return;
        }
        char val[256];
        snprintf(val, sizeof(val), "%s", rest + kpos);
        for (size_t l = strlen(val); l && (val[l - 1] == ' ' || val[l - 1] == '\n' || val[l - 1] == '\r'); l--)
            val[l - 1] = 0;
        pthread_mutex_lock(&a->lock);
        set_locked(a, key, val, out, outlen);
        pthread_mutex_unlock(&a->lock);
    } else if (strcmp(cmd, "get") == 0) {
        const char *what = line + pos;
        if (strncmp(what, "config", 6) == 0) {
            pthread_mutex_lock(&a->lock);
            get_config_locked(a, out, outlen);
            pthread_mutex_unlock(&a->lock);
        } else if (strncmp(what, "status", 6) == 0) {
            int n = snprintf(out, outlen, "OK\n");
            app_status(a, out + n, outlen - n);
        } else {
            snprintf(out, outlen, "ERR 2 usage: get config|status");
        }
    } else if (strcmp(cmd, "start") == 0) {
        pthread_mutex_lock(&a->lock);
        start_locked(a, out, outlen);
        pthread_mutex_unlock(&a->lock);
    } else if (strcmp(cmd, "stop") == 0) {
        app_stop(a);
        snprintf(out, outlen, "OK");
    } else {
        snprintf(out, outlen, "ERR 1 unknown command '%s'", cmd);
    }
}

/* ------------------------------------------------------------------ sender side */

const app_frame_t *app_frame_wait(app_t *a, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&a->lock);
    while (a->ready < 0)
        if (pthread_cond_timedwait(&a->frame_cv, &a->lock, &ts))
            break;
    const app_frame_t *f = NULL;
    if (a->ready >= 0) {
        a->slots[a->ready].state = SLOT_SENDING;
        f = &a->slots[a->ready].f;
        a->ready = -1;
    }
    pthread_mutex_unlock(&a->lock);
    return f;
}

void app_frame_release(app_t *a, const app_frame_t *f, int sent)
{
    pthread_mutex_lock(&a->lock);
    for (int i = 0; i < N_SLOTS; i++)
        if (&a->slots[i].f == f)
            a->slots[i].state = SLOT_FREE;
    if (sent)
        a->frames_sent++;
    pthread_mutex_unlock(&a->lock);
}

void app_status(app_t *a, char *buf, size_t len)
{
    pthread_mutex_lock(&a->lock);
    double   t = mono_s(), cpu = cpu_s();
    uint64_t rec = atomic_load(&a->records_total);
    double   dt = t - a->st_t;
    double   rate = dt > 0 && a->running ? (rec - a->st_records) / dt : 0.0;
    double   load = dt > 0 ? (cpu - a->st_cpu) / dt : 0.0;
    a->st_t = t;
    a->st_cpu = cpu;
    a->st_records = rec;
    const char *state = a->hw_error ? "error" : a->running ? "running" : "idle";
    static const char *const FCAL_NAMES[] = { [FCAL_MANUAL] = "manual", [FCAL_PENDING] = "pending",
                                              [FCAL_DONE] = "done", [FCAL_FAILED] = "failed" };
    const recon_fcal_t *f = &a->fcal;
    snprintf(buf, len,
             "state=%s\nhw=%s\nrate_rec_s=%.0f\nrecords_total=%llu\nframes_made=%llu\n"
             "frames_sent=%llu\nframes_dropped=%llu\nchunks_lost=%lu\nrecords_lost=%llu\n"
             "ring_fill=%.4f\ncpu=%.3f\n"
             "fast_cal=%s\nfast_cal_gain=%.6g\nfast_cal_offset=%.6g\nin2_amp_v=%.6g\nin2_mid_v=%.6g\n"
             "fast_levels=%.0f\nimg_width_run=%u\nfast_source=%s\nfast_hz=%.8g",
             state, hw_name(a->hw), rate, (unsigned long long)rec,
             (unsigned long long)a->frames_made, (unsigned long long)a->frames_sent,
             (unsigned long long)a->frames_dropped, (unsigned long)atomic_load(&a->chunks_lost),
             (unsigned long long)atomic_load(&a->records_lost),
             (double)ring_used(&a->ring) / ring_capacity(&a->ring), load,
             FCAL_NAMES[f->state],
             f->gain * a->cfg.hw_fast_gain / a->cfg.hw_sensor_gain,   /* as cal.fast_gain */
             f->offset * a->cfg.hw_fast_gain / COUNTS_PER_VOLT,       /* as cal.fast_offset */
             f->sensor_amp / COUNTS_PER_VOLT, f->sensor_mid / COUNTS_PER_VOLT,
             f->levels, (unsigned)f->width, a->cfg.fast_external ? "external" : "internal",
             f->state == FCAL_DONE || f->state == FCAL_MANUAL ? f->fast_hz : 0.0);
    pthread_mutex_unlock(&a->lock);
}
