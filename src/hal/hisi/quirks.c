#ifdef __arm__

void* (*fnIsp_Malloc)(unsigned long);
int   (*fnISP_AlgRegisterAcs)(int);
int   (*fnISP_AlgRegisterDehaze)(int);
int   (*fnISP_AlgRegisterDrc)(int);
int   (*fnISP_AlgRegisterLdci)(int);
int   (*fnMPI_ISP_IrAutoRunOnce)(int, void*);

void *isp_malloc(unsigned long size) {
    return fnIsp_Malloc(size);
}
int isp_alg_register_acs(int pipeId) {
    return fnISP_AlgRegisterAcs(pipeId);
}
int isp_alg_register_dehaze(int pipeId) {
    return fnISP_AlgRegisterDehaze(pipeId);
}
int ISP_AlgRegisterDehaze(int pipeId) {
    return fnISP_AlgRegisterDehaze(pipeId);
}
int isp_alg_register_drc(int pipeId) {
    return fnISP_AlgRegisterDrc(pipeId);
}
int ISP_AlgRegisterDrc(int pipeId) {
    return fnISP_AlgRegisterDrc(pipeId);
}
int isp_alg_register_ldci(int pipeId) {
    return fnISP_AlgRegisterLdci(pipeId);
}
int ISP_AlgRegisterLdci(int pipeId) {
    return fnISP_AlgRegisterLdci(pipeId);
}
int isp_ir_auto_run_once(int pipeId, void *irAttr) {
    return fnMPI_ISP_IrAutoRunOnce(pipeId, irAttr);
}
int MPI_ISP_IrAutoRunOnce(int pipeId, void *irAttr) {
    return fnMPI_ISP_IrAutoRunOnce(pipeId, irAttr);
}

/* ---- [divinus-142] hi_i2c VTS-write hardening: ROOT CAUSE of the SC1035 wedge ----
 * SC1035 hangs SILENTLY when VTS (0x320e:0x320f) is momentarily < ~active lines:
 * row logic locks, VI IntCnt freezes, no dmesg, no i2c error -- exactly the field
 * wedge (dead 1-3h) and the boot flake. The AE slow-framerate path ("YF change FPS")
 * and the sensor init table write the two VTS bytes back-to-back hi then lo, so a
 * DOWNSIZE passes the transient (hi_new<<8 | lo_old) = 768..1023 -- deep slow-fr
 * values (lo_old small, e.g. VTS 5000..6000 at night) push it below the floor ->
 * dead sensor. "VTS=667 kills sensor" from the .168 KB is the same law.
 *
 * Fix (no group registers, no vendor-semantics guessing): intercept the pair and
 * rewrite it as THREE writes with only safe intermediates:
 *     1) 0x320e = 0xff        (VTS jumps huge: any transient >= 65280 = harmless)
 *     2) 0x320f = lo_target   (still huge)
 *     3) 0x320e = hi_target   (lands exactly on target, >= 1000 via clamp)
 * Target is clamped to >= 1000 (full-lines std). Everything else passes through
 * untouched (via syscall() so there is no recursion). Covers init + AE runtime. */
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define I2C_CMD_WRITE 0x01
#define VTS_HI_REG    0x320e
#define VTS_LO_REG    0x320f
#define VTS_FLOOR     1000u

struct hi_i2c_w {            /* /dev/hi_i2c CMD_I2C_WRITE payload (venc-sample wi2c) */
    unsigned char dev_addr;  /* +0  */
    unsigned char pad_[3];
    unsigned int  reg_addr;  /* +4  (2 bytes used) */
    unsigned int  addr_byte_num; /* +8 */
    unsigned int  data;      /* +12 (1 byte used) */
    unsigned int  data_byte_num; /* +16 */
};

static unsigned vts_pairs, vts_clamps;
static int      vts_pending;
static unsigned vts_hi_val = 0x03, vts_lo_val = 0xe8;   /* nominal VTS=1000 */

static int raw_ioctl(int fd, unsigned long req, void *arg)
{
    return (int)syscall(SYS_ioctl, fd, req, arg);
}

static int i2c_wr(int fd, unsigned char dev, unsigned int reg, unsigned int val)
{
    struct hi_i2c_w d;
    memset(&d, 0, sizeof d);
    d.dev_addr = dev;
    d.reg_addr = reg;
    d.addr_byte_num = 2;
    d.data = val & 0xff;
    d.data_byte_num = 1;
    return raw_ioctl(fd, I2C_CMD_WRITE, &d);
}

static void vts_apply(int fd, unsigned char dev, unsigned int hi, unsigned int lo)
{
    unsigned int target = ((hi & 0xff) << 8) | (lo & 0xff);
    if (target < VTS_FLOOR) {
        fprintf(stderr, "[vts] CLAMP %u -> %u (hi=0x%02x lo=0x%02x)\n",
                target, VTS_FLOOR, hi & 0xff, lo & 0xff);
        target = VTS_FLOOR;
        hi = target >> 8;
        lo = target & 0xff;
        vts_clamps++;
    }
    /* park huge first: every intermediate stays >= 65280 or is the final target */
    i2c_wr(fd, dev, VTS_HI_REG, 0xff);
    i2c_wr(fd, dev, VTS_LO_REG, lo & 0xff);
    i2c_wr(fd, dev, VTS_HI_REG, hi & 0xff);
    vts_hi_val = hi & 0xff;
    vts_lo_val = lo & 0xff;
    vts_pending = 0;
    vts_pairs++;
    fprintf(stderr, "[vts] VTS=%u (0x%04x) applied (pairs=%u clamps=%u)\n",
            target, target, vts_pairs, vts_clamps);
}

int ioctl(int fd, int req, ...)
{
    va_list ap;
    void *arg;
    struct hi_i2c_w *p;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (req == I2C_CMD_WRITE && arg) {
        p = (struct hi_i2c_w *)arg;
        if (p->addr_byte_num == 2 && p->data_byte_num == 1) {
            if (p->reg_addr == VTS_HI_REG) {
                vts_hi_val = p->data & 0xff;   /* defer: lo half applies the pair */
                vts_pending = 1;
                return 1;
            }
            if (p->reg_addr == VTS_LO_REG) {
                vts_apply(fd, p->dev_addr, vts_hi_val, p->data & 0xff);
                return 1;
            }
        }
    }
    /* any other call: flush a half-pending VTS first so it is never lost */
    if (vts_pending) {
        unsigned char dev = (req == I2C_CMD_WRITE && arg)
            ? ((struct hi_i2c_w *)arg)->dev_addr : 0x60;
        vts_apply(fd, dev, vts_hi_val, vts_lo_val);
    }
    return raw_ioctl(fd, (unsigned long)req, arg);
}

void vts_quirk_dump(void)
{
    fprintf(stderr, "[vts] dump: pairs=%u clamps=%u pending=%d last=0x%02x%02x\n",
            vts_pairs, vts_clamps, vts_pending, vts_hi_val, vts_lo_val);
}

#endif
