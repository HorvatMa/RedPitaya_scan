/*
 * Real FPGA backend: the deployed design (vivado/myproj) through the UIO devices created by
 * fpga/dts/fpga.dtso. Register map and hazards: documentation/FPGA_MANUAL.md (sections 5-6, 11).
 *
 * Safety rules enforced here:
 *   - CTRL is only ever written as 3 (clear + keep requesting interrupts; 0/1 lets the DMA
 *     run past its buffer)
 *   - decimations >= 1 (acquisition >= 2), FIFO_MIN_THRESH is never touched
 *   - waveform 1 <= 16384, waveform 2 <= 12288 samples (longer tables alias onto registers),
 *     written as full 32-bit words only
 *   - refuses to run if the register block does not look like our bitstream
 */
#include "hw.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* rp_acquire map0 (0x4000_0000) */
#define R_DEC_FACTOR      0x00
#define R_START_ACQ       0x04
#define R_TRANSFER_RD_CNT 0x10
#define R_FIFO_MIN_THRESH 0x14
#define R_CTRL            0x20
#define R_INTEGRATOR_DEC  0x24
#define R_BUFFER_N        0x28
#define R_INPUT_SEL       0x2C
#define CTRL_CLEAR_AND_REQUEST 3u

/* rp_generate map0 (0x4000_1000) */
#define G_START_GEN       0x00
#define G_DEC1            0x04
#define G_DEC2            0x08
#define G_LEN1            0x0C
#define G_LEN2            0x10

/* rp_generate map1 (0x4000_2000): waveform 1 words, then waveform 2 from +0x8000 */
#define WFM2_OFFSET       0x8000
#define WFM_WINDOW_END    0xE000         /* 0x4001_0000 would alias onto the registers */

#define BUFFER_RECORDS_MAX 32768u        /* largest chunk the FPGA produces (256 KiB) */

typedef struct {
    int      fd;
    volatile uint32_t *map[2];
    size_t   size[2];
} uio_dev_t;

struct hw {
    uio_dev_t acq, gen;
    int       running;
    unsigned  idle_polls;
    unsigned long irq_recovered;
};

/* ------------------------------------------------------------------ UIO helpers */

static int read_sysfs(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int uio_open(uio_dev_t *d, const char *name)
{
    DIR *dir = opendir("/sys/class/uio");
    if (!dir)
        return -1;
    struct dirent *e;
    char path[512], buf[64];
    int found = -1;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "uio", 3))
            continue;
        snprintf(path, sizeof(path), "/sys/class/uio/%s/name", e->d_name);
        if (read_sysfs(path, buf, sizeof(buf)) == 0 && strcmp(buf, name) == 0) {
            found = atoi(e->d_name + 3);
            break;
        }
    }
    closedir(dir);
    if (found < 0) {
        fprintf(stderr, "hw_uio: no UIO device '%s' (overlay applied?)\n", name);
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/uio%d", found);
    d->fd = open(path, O_RDWR);
    if (d->fd < 0) {
        perror(path);
        return -1;
    }
    for (int m = 0; m < 2; m++) {
        snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map%d/size", found, m);
        if (read_sysfs(path, buf, sizeof(buf))) {
            fprintf(stderr, "hw_uio: %s has no map%d\n", name, m);
            return -1;
        }
        d->size[m] = strtoul(buf, NULL, 0);
        void *p = mmap(NULL, d->size[m], PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
                       (off_t)m * getpagesize());
        if (p == MAP_FAILED) {
            perror("hw_uio: mmap");
            return -1;
        }
        d->map[m] = p;
    }
    return 0;
}

static void uio_close(uio_dev_t *d)
{
    for (int m = 0; m < 2; m++)
        if (d->map[m])
            munmap((void *)d->map[m], d->size[m]);
    if (d->fd > 0)
        close(d->fd);
}

static inline void wr(volatile uint32_t *base, unsigned off, uint32_t v)
{
    base[off / 4] = v;
}

static inline uint32_t rd(volatile uint32_t *base, unsigned off)
{
    return base[off / 4];
}

/* ------------------------------------------------------------------ backend API */

hw_t *hw_open(void)
{
    hw_t *hw = calloc(1, sizeof(*hw));
    if (!hw)
        return NULL;
    hw->acq.fd = hw->gen.fd = -1;
    if (uio_open(&hw->acq, "rp_acquire") || uio_open(&hw->gen, "rp_generate"))
        goto fail;
    if (hw->gen.size[1] < WFM_WINDOW_END || hw->acq.size[1] < BUFFER_RECORDS_MAX * sizeof(record_t)) {
        fprintf(stderr, "hw_uio: UIO maps smaller than expected (overlay mismatch?)\n");
        goto fail;
    }
    /* signature of our bitstream: both read 15 after reset and are never written */
    uint32_t t = rd(hw->acq.map[0], R_TRANSFER_RD_CNT), f = rd(hw->acq.map[0], R_FIFO_MIN_THRESH);
    if (t != 15 || f != 15) {
        fprintf(stderr, "hw_uio: register signature %u/%u != 15/15: wrong bitstream loaded?\n", t, f);
        goto fail;
    }
    /* known idle state */
    wr(hw->acq.map[0], R_START_ACQ, 0);
    wr(hw->gen.map[0], G_START_GEN, 0);
    wr(hw->acq.map[0], R_CTRL, CTRL_CLEAR_AND_REQUEST);
    return hw;
fail:
    hw_close(hw);
    return NULL;
}

void hw_close(hw_t *hw)
{
    if (!hw)
        return;
    if (hw->acq.map[0]) {
        wr(hw->acq.map[0], R_START_ACQ, 0);
        wr(hw->acq.map[0], R_CTRL, CTRL_CLEAR_AND_REQUEST);
    }
    if (hw->gen.map[0])
        wr(hw->gen.map[0], G_START_GEN, 0);
    uio_close(&hw->acq);
    uio_close(&hw->gen);
    free(hw);
}

const char *hw_name(const hw_t *hw)
{
    (void)hw;
    return "uio";
}

static void write_waveform(volatile uint32_t *dst, const int16_t *s, uint32_t len)
{
    for (uint32_t k = 0; k < (len + 1) / 2; k++) {
        uint16_t lo = (uint16_t)s[2 * k];
        uint16_t hi = (uint16_t)(2 * k + 1 < len ? s[2 * k + 1] : s[2 * k]);
        dst[k] = (uint32_t)lo | (uint32_t)hi << 16;     /* full 32-bit store */
    }
}

int hw_configure(hw_t *hw, const hw_setup_t *s)
{
    if (hw->running || s->len1 < 2 || s->len1 > WFM1_MAX_LEN || s->len2 < 2 ||
        s->len2 > WFM2_MAX_LEN || s->dec1 < 1 || s->dec2 < 1 || s->acq_dec < 2 || s->int_dec < 1)
        return -1;
    volatile uint32_t *g = hw->gen.map[0], *a = hw->acq.map[0];

    wr(g, G_START_GEN, 0);
    write_waveform(hw->gen.map[1], s->wfm1, s->len1);
    write_waveform(hw->gen.map[1] + WFM2_OFFSET / 4, s->wfm2, s->len2);
    wr(g, G_DEC1, s->dec1);
    wr(g, G_DEC2, s->dec2);
    wr(g, G_LEN1, s->len1 & 0x3FFF);           /* 14-bit: 16384 -> 0 = full table */
    wr(g, G_LEN2, s->len2 & 0x3FFF);

    wr(a, R_START_ACQ, 0);
    wr(a, R_DEC_FACTOR, s->acq_dec);
    wr(a, R_INTEGRATOR_DEC, s->int_dec);
    wr(a, R_INPUT_SEL, 1);                     /* y slot = IN2 (fast position sensor) */
    return 0;
}

int hw_start(hw_t *hw)
{
    volatile uint32_t *a = hw->acq.map[0];
    wr(a, R_CTRL, CTRL_CLEAR_AND_REQUEST);     /* buffer pointer back to the start */
    uint32_t one = 1;
    if (write(hw->acq.fd, &one, sizeof(one)) != sizeof(one))   /* enable the UIO interrupt */
        return -1;
    wr(hw->gen.map[0], G_START_GEN, 1);
    wr(a, R_START_ACQ, 1);
    hw->running = 1;
    hw->idle_polls = 0;
    return 0;
}

void hw_stop(hw_t *hw)
{
    wr(hw->acq.map[0], R_START_ACQ, 0);
    wr(hw->gen.map[0], G_START_GEN, 0);         /* outputs go to 0 V */
    wr(hw->acq.map[0], R_CTRL, CTRL_CLEAR_AND_REQUEST);
    hw->running = 0;
}

/* copy the chunk the FPGA announced, then release the buffer */
static long take_chunk(hw_t *hw, record_t *dst, size_t max)
{
    volatile uint32_t *a = hw->acq.map[0];
    uint32_t n = rd(a, R_BUFFER_N);
    if (n > BUFFER_RECORDS_MAX)
        n = BUFFER_RECORDS_MAX;
    size_t copy = n < max ? n : max;
    const volatile uint64_t *buf = (const volatile uint64_t *)hw->acq.map[1];
    for (size_t i = 0; i < copy; i++) {        /* uncached memory: one 64-bit load per record */
        uint64_t w = buf[i];
        memcpy(&dst[i], &w, sizeof(w));
    }
    wr(a, R_CTRL, CTRL_CLEAR_AND_REQUEST);
    return (long)copy;
}

long hw_read(hw_t *hw, record_t *dst, size_t max, int timeout_ms)
{
    if (!hw->running)
        return -1;
    struct pollfd p = { .fd = hw->acq.fd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    uint32_t one = 1;
    if (r == 0) {
        /* an edge lost between clear and re-enable would stall the FPGA forever: after ~1 s
           without an interrupt, service a pending buffer anyway */
        if (++hw->idle_polls * (unsigned)timeout_ms >= 1000 && rd(hw->acq.map[0], R_BUFFER_N) > 0) {
            hw->idle_polls = 0;
            hw->irq_recovered++;
            long n = take_chunk(hw, dst, max);
            if (write(hw->acq.fd, &one, sizeof(one)) != sizeof(one))
                return -1;
            return n;
        }
        return 0;
    }
    uint32_t count;
    if (read(hw->acq.fd, &count, sizeof(count)) != sizeof(count))
        return -1;
    hw->idle_polls = 0;
    long n = take_chunk(hw, dst, max);
    if (write(hw->acq.fd, &one, sizeof(one)) != sizeof(one))    /* re-enable for the next one */
        return -1;
    return n;
}
