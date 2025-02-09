/* sigma_dp.c: moving head disk pack controller

   Copyright (c) 2008-2022, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   dp           moving head disk pack controller

   09-Dec-22    RMS     Invalid address must set a TDV-visible error flag (Ken Rector)
   23-Jul-22    RMS     SEEK(I), RECAL(I) should be fast operations (Ken Rector)
   02-Jul-22    RMS     Fixed bugs in multi-unit operation
   28-Jun-22    RMS     Fixed off-by-1 error in DP_SEEK definition (Ken Rector)
   07-Jun-22    RMS     Removed unused variables (V4)
   06-Jun-22    RMS     Fixed incorrect return in TIO status (Ken Rector)
   06-Jun-22    RMS     Fixed missing loop increment in TDV (Ken Rector)
   13-Mar-17    RMS     Fixed bug in selecting 3281 unit F (COVERITY)

   Transfers are always done a sector at a time.

   This module simulates five Sigma controller/disk pack pairs (7240, 7270;
   7260, 7265, 7275) and one Telefile controller that supported different
   disk models on the same controller (T3281/3282/3283/3286/3288). Due to
   ambiguities in the documentation, the T3286 disk is not implemented.

   Broadly speaking, the controllers fall into two families: the 7240/7270,
   which returned 10 bytes for sense status; and the others, which returned
   16 bytes. Each disk has two units: one for timing channel operations, and
   one for timing asynchronous seek completions. The controller will not
   start a new operation is it is busy (any of the main units active) or if
   the target device is busy (its seek unit is active).
 
   The DP's seek interrupt has a unique feature: it comes and goes, lasting only
   a sector's time; and it gets "knocked down" by any SIO to a different unit.
   Therefore, the SIO interrupt check is complicated.

   1. If there's a controller interrupt, the SIO fails.
   2. If there's a seek interrupt on the selected unit, the SIO fails.
   3. All other seek interrupts are "knocked down" and rescheduled for
      some time "in the future."
   4. The SIO completes normally.
*/

#include "sigma_io_defs.h"
#include <math.h>

#define UNIT_V_AUTO     (UNIT_V_UF + 1)                 /* autosize */
#define UNIT_AUTO       (1u << UNIT_V_AUTO)
#define UNIT_V_DTYPE    (UNIT_V_UF + 2)                 /* drive type */
#define UNIT_M_DTYPE    0xF
#define UNIT_DTYPE      (UNIT_M_DTYPE << UNIT_V_DTYPE)
#define UDA             u3                              /* disk addr */
#define UCMD            u4                              /* current command */
#define UCTX            u5                              /* ctrl/ctx index */
#define GET_DTYPE(x)    (((x) >> UNIT_V_DTYPE) & UNIT_M_DTYPE)

/* Constants */

#define DP_NUMCTL       2                               /* number of controllers */
#define DP_CTYPE        6                               /* total ctrl types */
#define DP_C7240        0                               /* 7240 ctrl */
#define DP_C7270        1                               /* 7270 ctrl */
#define DP_C7260        2                               /* 7260 ctrl */
#define DP_C7265        3                               /* 7265 ctrl */
#define DP_C7275        4                               /* 7275 ctrl */
#define DP_C3281        5                               /* T3281 ctrl */
#define DP_Q10B(x)      ((x) <= DP_C7270)               /* 10B or 16B? */
#define DP_NUMDR_10B    8                               /* drives/ctrl */
#define DP_NUMDR_16B    15
#define DP_CONT         0xF                             /* ctrl's drive # */
#define DP_WDSC         256                             /* words/sector */
#define DP_BYHD         8                               /* byte/header */
#define DP_NUMDR        ((uint32) ((DP_Q10B (ctx->dp_ctype))? DP_NUMDR_10B: DP_NUMDR_16B))
#define DP_SEEK         (DP_CONT + 1)                   /* offset to seek units */

/* Address bytes */

#define DPA_V_CY        16                              /* cylinder offset */
#define DPA_M_CY        0x3FF
#define DPA_V_HD        8                               /* head offset */
#define DPA_M_HD        0x1F
#define DPA_V_SC        0                               /* sector offset */
#define DPA_M_SC        0x1F
#define DPA_GETCY(x)    (((x) >> DPA_V_CY) & DPA_M_CY)
#define DPA_GETHD(x)    (((x) >> DPA_V_HD) & DPA_M_HD)
#define DPA_GETSC(x)    (((x) >> DPA_V_SC) & DPA_M_SC)

/* Sense order */

#define DPS_NBY_10B     10
#define DPS_NBY_16B     16
#define DPS_NBY         ((uint32) (DP_Q10B (ctx->dp_ctype)? DPS_NBY_10B: DPS_NBY_16B))

/* Test mode */

#define DPT_NBY_10B     1                               /* bytes/test mode spec */
#define DPT_NBY_16B     2
#define DPT_NBY         ((uint32) (DP_Q10B (ctx->dp_ctype)? DPT_NBY_10B: DPT_NBY_16B))

/* Commands */

#define DPS_INIT        0x100
#define DPS_END         0x101
#define DPS_WRITE       0x01
#define DPS_READ        0x02
#define DPS_SEEK        0x03
#define DPS_SEEKI       0x83
#define DPS_SENSE       0x04
#define DPS_CHECK       0x05
#define DPS_RSRV        0x07
#define DPS_WHDR        0x09
#define DPS_RHDR        0x0A
#define DPS_CRIOF       0x0F
#define DPS_RDEES       0x12
#define DPS_TEST        0x13
#define DPS_RLS         0x17
#define DPS_CRION       0x1F
#define DPS_RLSA        0x23
#define DPS_RECAL       0x33
#define DPS_RECALI      0xB3

/* Seek completion states */

#define DSC_SEEK        0x00                            /* seeking */
#define DSC_SEEKI       0x80                            /* seeking, then int */
#define DSC_SEEKW       0x01                            /* waiting to int */

/* Device status - note that these are device independent */

#define DPF_V_WCHK      0
#define DPF_V_DPE       1
#define DPF_V_SNZ       2
#define DPF_V_EOC       3
#define DPF_V_IVA       4
#define DPF_V_PGE       5
#define DPF_V_WPE       6
#define DPF_V_AIM       7
#define DPF_WCHK        (1u << DPF_V_WCHK)              /* wrt chk error */
#define DPF_DPE         (1u << DPF_V_DPE)               /* data error */
#define DPF_SNZ         (1u << DPF_V_SNZ)               /* sec# != 0 @ whdr */
#define DPF_EOC         (1u << DPF_V_EOC)               /* end cylinder */
#define DPF_IVA         (1u << DPF_V_IVA)               /* invalid addr */
#define DPF_PGE         (1u << DPF_V_PGE)               /* prog error */
#define DPF_WPE         (1u << DPF_V_WPE)               /* wrt prot err */
#define DPF_AIM         (1u << DPF_V_AIM)               /* arm in motion */
#define DPF_V_DIFF      16
#define DPF_M_DIFF      0xFFFFu
#define DPF_DIFF        (DPF_M_DIFF << DPF_V_DIFF)

/* Drive types */

/* These controllers support many different disk drive types:

   type         #sectors/       #surfaces/      #cylinders/
                 surface         cylinder        drive

   7242         6               20              203         =24MB
   7261         11              20              203         =45MB
   7271         6               20              406         =48MB
   3288         17              5               822         =67MB
   7276         11              19              411         =86MB
   7266         11              20              411         =90MB
   3282         11              19              815         =170MB
   3283         17              19              815         =263MB

   On the T3281, each drive can be a different type.  The size field
   in each unit selects the drive capacity for each drive and thus
   the drive type.  DISKS MUST BE DECLARED IN ASCENDING SIZE.
*/

#define DP_SZ(x)        ((DPCY_##x) * (DPHD_##x) * (DPSC_##x) * DP_WDSC)
#define DP_ENT(x,y)     (DP_##x), (DPCY_##x), (DPHD_##x), (DPSC_##x), (DP_C##y), (DPSZ_##x), (DPID_##x)

#define DP_7242         0
#define DPCY_7242       203
#define DPHD_7242       20
#define DPSC_7242       6
#define DPID_7242       0
#define DPSZ_7242       DP_SZ(7242)

#define DP_7261         1
#define DPCY_7261       203
#define DPHD_7261       20
#define DPSC_7261       11
#define DPID_7261       (5u << 5)
#define DPSZ_7261       DP_SZ(7261)

#define DP_7271         2
#define DPCY_7271       406
#define DPHD_7271       20
#define DPSC_7271       6
#define DPID_7271       0
#define DPSZ_7271       DP_SZ(7271)

#define DP_3288         3
#define DPCY_3288       822
#define DPHD_3288       5
#define DPSC_3288       17
#define DPID_3288       0
#define DPSZ_3288       DP_SZ(3288)

#define DP_7276         4
#define DPCY_7276       411
#define DPHD_7276       19
#define DPSC_7276       11
#define DPID_7276       (7 << 5)
#define DPSZ_7276       DP_SZ(7276)

#define DP_7266         5
#define DPCY_7266       411
#define DPHD_7266       20
#define DPSC_7266       11
#define DPID_7266       (6 << 5)
#define DPSZ_7266       DP_SZ(7276)

#define DP_3282         6
#define DPCY_3282       815
#define DPHD_3282       19
#define DPSC_3282       11
#define DPID_3282       0
#define DPSZ_3282       DP_SZ(3282)

#define DP_3283         7
#define DPCY_3283       815
#define DPHD_3283       19
#define DPSC_3283       17
#define DPID_3283       0
#define DPSZ_3283       DP_SZ(3283)

#define GET_PSC(x,s)    ((int32) fmod (sim_gtime() / ((double) (x * DP_WDSC)), \
                        ((double) (s))))

typedef struct {
    uint32 dp_ctype;                                    /* controller type */
    uint32 dp_time;                                     /* inter-word time */
    uint32 dp_stime;                                    /* inter-track time */
    uint32 dp_flags;                                    /* status flags */
    uint32 dp_ski;                                      /* seek interrupts */
    uint32 dp_stopioe;                                  /* stop on I/O error */
    uint32 dp_test;                                     /* test mode */
    } DP_CTX;

typedef struct {
    uint32 dtype;                                       /* drive type */
    uint32 cy;                                          /* cylinders */
    uint32 hd;                                          /* heads */
    uint32 sc;                                          /* sectors */
    uint32 ctype;                                       /* controller */
    uint32 capac;                                       /* capacity */
    uint32 id;                                          /* ID */
    } DP_TYPE;

typedef struct {
    uint32 byte;                                        /* offset in array */
    uint32 mask;                                        /* test mask */
    uint32 fpos;                                        /* from position */
    uint32 tpos;                                        /* to position */
    } DP_SNSTAB;

static const char *dp_cname[] = {
    "7240", "7270", "7260", "7275", "7265", "T3281"
    };

static uint32 dp_buf[DP_WDSC];

extern uint32 chan_ctl_time;

uint32 dpa_disp (uint32 op, uint32 dva, uint32 *dvst);
uint32 dpb_disp (uint32 op, uint32 dva, uint32 *dvst);
uint32 dp_disp (uint32 cidx, uint32 op, uint32 dva, uint32 *dvst);
uint32 dp_tio_status (uint32 cidx, uint32 un);
uint32 dp_tdv_status (uint32 cidx, uint32 un);
uint32 dp_aio_status (uint32 cidx, uint32 un);
void dp_set_sense (UNIT *uptr, uint32 *c);
t_stat dp_chan_err (uint32 dva, uint32 st);
t_stat dp_svc (UNIT *uptr);
t_stat dps_svc (UNIT *uptr);
t_stat dp_reset (DEVICE *dptr);
t_bool dp_inv_ad (UNIT *uptr, uint32 *da);
t_bool dp_inc_ad (UNIT *uptr);
t_stat dp_read (UNIT *uptr, uint32 da);
t_stat dp_write (UNIT *uptr, uint32 da);
t_stat dp_ioerr (UNIT *uptr);
t_bool dp_test_mode (uint32 cidx);
t_bool dp_end_sec (UNIT *uptr, uint32 lnt, uint32 exp, uint32 st);
int32 dp_clr_int (uint32 cidx);
void dp_set_ski (uint32 cidx, uint32 un);
void dp_clr_ski (uint32 cidx, uint32 un);
t_stat dp_attach (UNIT *uptr, CONST char *cptr);
t_stat dp_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dp_set_auto (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dp_set_ctl (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dp_show_ctl (FILE *st, UNIT *uptr, int32 val, CONST void *desc);

static DP_TYPE dp_tab[] = {
    { DP_ENT (7242, 7240) },
    { DP_ENT (7261, 7260) },
    { DP_ENT (7271, 7270) },
    { DP_ENT (3288, 3281) },
    { DP_ENT (7276, 7275) },
    { DP_ENT (7266, 7265) },
    { DP_ENT (3282, 3281) },
    { DP_ENT (3283, 3281) },
    { 0, 0, 0, 0, 0, 0 },
    };

static DP_SNSTAB dp_sense_10B[] = {
    { 7, 0x00FF0000, 16, 0 },
    { 8, DPF_WCHK, DPF_V_WCHK, 6 },
    { 8, DPF_SNZ, DPF_V_SNZ, 2 },
    { 9, 0x01000000, 24, 0 },
    { 0, 0, 0, 0 }
    };

static DP_SNSTAB dp_sense_16B[] = {
    { 8, DPF_WCHK, DPF_V_WCHK, 7 },
    { 8, DPF_EOC, DPF_V_EOC, 3},
    { 8, DPF_AIM, DPF_V_AIM, 2},
    { 14, 0xFF000000, 24, 0 },
    { 15, 0x00FF0000, 16, 0 },
    { 0, 0, 0, 0 }
    };

/* Command table, indexed by command */

#define C_10B           (1u << DP_C7240) | (1u << DP_C7270)
#define C_16B           (1u << DP_C7260) | (1u << DP_C7275) | \
                        (1u << DP_C7265) | (1u << DP_C3281)
#define C_A             (C_10B|C_16B)                   /* all */
#define C_F             (1u << (DP_CTYPE))              /* fast */
#define C_C             (1u << (DP_CTYPE + 1))          /* ctrl cmd */

static uint16 dp_cmd[256] = {
   0, C_A, C_A, C_A|C_F, C_A|C_F, C_A, 0, C_16B|C_F,
   0, C_A, C_A, 0, 0, 0, 0, C_16B|C_F|C_C,
   0, 0, C_A, C_A|C_F, 0, 0, 0, C_16B|C_F,
   0, 0, 0, 0, 0, 0, 0, C_16B|C_F|C_C,
   0, 0, 0, C_10B|C_F, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, C_A|C_F, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, C_A|C_F, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, C_16B|C_F, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0
   };

/* DP data structures

   dp_dev       DP device descriptor
   dp_unit      DP unit descriptor
   dp_reg       DP register list
*/

dib_t dp_dib[] = {
    { DVA_DPA, &dpa_disp },
    { DVA_DPB, &dpb_disp }
    };

DP_CTX dp_ctx[] = {
    { DP_C7270, 1, 20 },
    { DP_C7275, 1, 20 }
    };

UNIT dpa_unit[] = {
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, (DP_7271 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DIS, DPSZ_7271) },
    { UDATA (&dp_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    };

UNIT dpb_unit[] = {
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, (DP_7276 << UNIT_V_DTYPE)+UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, DPSZ_7276) },
    { UDATA (&dp_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    { UDATA (&dps_svc, UNIT_DIS, 0) },
    };

REG dpa_reg[] = {
    { HRDATA (CTYPE, dp_ctx[0].dp_ctype, 1), REG_HRO },
    { HRDATA (FLAGS, dp_ctx[0].dp_flags, 8) },
    { GRDATA (DIFF, dp_ctx[0].dp_flags, 16, 16, 16) },
    { HRDATA (SKI, dp_ctx[0].dp_ski, 16) },
    { HRDATA (TEST, dp_ctx[0].dp_test, 16) },
    { URDATA (ADDR, dpa_unit[0].UDA, 16, 32, 0, DP_NUMDR_16B, 0) },
    { URDATA (CMD, dpa_unit[0].UCMD, 16, 10, 0, DP_NUMDR_16B, 0) },
    { DRDATA (TIME, dp_ctx[0].dp_time, 24), PV_LEFT+REG_NZ },
    { DRDATA (STIME, dp_ctx[0].dp_stime, 24), PV_LEFT+REG_NZ },
    { FLDATA (STOP_IOE, dp_ctx[0].dp_stopioe, 0) },
    { HRDATA (DEVNO, dp_dib[0].dva, 12), REG_HRO },
    { NULL }
    };

REG dpb_reg[] = {
    { HRDATA (CTYPE, dp_ctx[1].dp_ctype, 1), REG_HRO },
    { HRDATA (FLAGS, dp_ctx[1].dp_flags, 8) },
    { GRDATA (DIFF, dp_ctx[1].dp_flags, 16, 16, 16) },
    { HRDATA (SKI, dp_ctx[1].dp_ski, 16) },
    { HRDATA (TEST, dp_ctx[1].dp_test, 16) },
    { URDATA (ADDR, dpa_unit[1].UDA, 16, 32, 0, DP_NUMDR_16B, 0) },
    { URDATA (CMD, dpa_unit[1].UCMD, 16, 10, 0, DP_NUMDR_16B, 0) },
    { DRDATA (TIME, dp_ctx[1].dp_time, 24), PV_LEFT+REG_NZ },
    { DRDATA (STIME, dp_ctx[1].dp_stime, 24), PV_LEFT+REG_NZ },
    { FLDATA (STOP_IOE, dp_ctx[1].dp_stopioe, 0) },
    { HRDATA (DEVNO, dp_dib[1].dva, 12), REG_HRO },
    { NULL }
    };

MTAB dp_mod[] = {
    { MTAB_XTD|MTAB_VDV, DP_C7240, NULL, "7240", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, DP_C7270, NULL, "7270", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, DP_C7260, NULL, "7260", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, DP_C7275, NULL, "7275", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, DP_C7265, NULL, "7265", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, DP_C3281, NULL, "T3281", &dp_set_ctl, NULL, NULL },
    { MTAB_XTD|MTAB_VDV, 0, "controller", NULL, NULL, &dp_show_ctl },
    { UNIT_DTYPE, (DP_7242 << UNIT_V_DTYPE), "7242", NULL, NULL },
    { UNIT_DTYPE, (DP_7261 << UNIT_V_DTYPE), "7261", NULL, NULL },
    { UNIT_DTYPE, (DP_7271 << UNIT_V_DTYPE), "7271", NULL, NULL },
    { UNIT_DTYPE, (DP_7276 << UNIT_V_DTYPE), "7276", NULL, NULL },
    { UNIT_DTYPE, (DP_7266 << UNIT_V_DTYPE), "7266", NULL, NULL },
    { (UNIT_DTYPE+UNIT_ATT), (DP_3288 << UNIT_V_DTYPE) + UNIT_ATT,
      "3288", NULL, NULL },
    { (UNIT_DTYPE+UNIT_ATT), (DP_3282 << UNIT_V_DTYPE) + UNIT_ATT,
      "3282", NULL, NULL },
    { (UNIT_DTYPE+UNIT_ATT), (DP_3282 << UNIT_V_DTYPE) + UNIT_ATT,
      "3282", NULL, NULL },
    { (UNIT_AUTO+UNIT_DTYPE+UNIT_ATT), (DP_3288 << UNIT_V_DTYPE),
      "3288", NULL, NULL },
    { (UNIT_AUTO+UNIT_DTYPE+UNIT_ATT), (DP_3282 << UNIT_V_DTYPE),
      "3282", NULL, NULL },
    { (UNIT_AUTO+UNIT_DTYPE+UNIT_ATT), (DP_3283 << UNIT_V_DTYPE),
      "3283", NULL, NULL },
    { (UNIT_AUTO+UNIT_ATT), UNIT_AUTO, "autosize", NULL, NULL },
    { UNIT_AUTO, UNIT_AUTO, NULL, "AUTOSIZE", &dp_set_auto, NULL },
    { (UNIT_AUTO+UNIT_DTYPE), (DP_3288 << UNIT_V_DTYPE),
      NULL, "3288", &dp_set_size },
    { (UNIT_AUTO+UNIT_DTYPE), (DP_3282 << UNIT_V_DTYPE),
      NULL, "3282", &dp_set_size },
    { (UNIT_AUTO+UNIT_DTYPE), (DP_3283 << UNIT_V_DTYPE),
      NULL, "3283", &dp_set_size },
    { MTAB_XTD|MTAB_VUN, 0, "write enabled", "WRITEENABLED", 
        &set_writelock, &show_writelock,   NULL, "Write enable disk drive" },
    { MTAB_XTD|MTAB_VUN, 1, NULL, "LOCKED", 
        &set_writelock, NULL,   NULL, "Write lock disk drive" },
    { MTAB_XTD|MTAB_VDV, 0, "CHAN", "CHAN",
      &io_set_dvc, &io_show_dvc, NULL },
    { MTAB_XTD|MTAB_VDV, 0, "DVA", "DVA",
      &io_set_dva, &io_show_dva, NULL },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 0, "CSTATE", NULL,
      NULL, &io_show_cst, NULL },
    { 0 }
    };

DEVICE dp_dev[] = {
    {
    "DPA", dpa_unit, dpa_reg, dp_mod,
    (2 * DP_NUMDR_16B) + 1, 16, 28, 1, 16, 32,
    NULL, NULL, &dp_reset,
    &io_boot, &dp_attach, NULL,
    &dp_dib[0], DEV_DISABLE
    },
    {
    "DPB", dpb_unit, dpb_reg, dp_mod,
    (2 * DP_NUMDR_16B) + 1, 16, 28, 1, 16, 32,
    NULL, NULL, &dp_reset,
    &io_boot, &dp_attach, NULL,
    &dp_dib[1], DEV_DISABLE
    }
    };

/* DP: IO dispatch routine

   For all calls except AIO, dva is the full channel/device/unit address
   For AIO, the handler must return the unit number
*/

uint32 dpa_disp (uint32 op, uint32 dva, uint32 *dvst)
{
return dp_disp (0, op, dva, dvst);
}

uint32 dpb_disp (uint32 op, uint32 dva, uint32 *dvst)
{
return dp_disp (1, op, dva, dvst);
}

uint32 dp_disp (uint32 cidx, uint32 op, uint32 dva, uint32 *dvst)
{
uint32 un = DVA_GETUNIT (dva);
UNIT *dp_unit = dp_dev[cidx].units;
UNIT *uptr;
int32 iu;
uint32 i;
DP_CTX *ctx;

if (cidx >= DP_NUMCTL)                                  /* inv ctrl num? */
    return DVT_NODEV;
ctx = &dp_ctx[cidx];
if (((un < DP_NUMDR) &&                                 /* un valid and */
    ((dp_unit[un].flags & UNIT_DIS) == 0)) ||           /* not disabled OR */
    ((un == 0xF) && (ctx->dp_ctype == DP_C3281)))       /* 3281 unit F? */
    uptr = dp_unit + un;                                /* un exists */
else return DVT_NODEV;

switch (op) {                                           /* case on op */

    case OP_SIO:                                        /* start I/O */
        *dvst = dp_tio_status (cidx, un);               /* get status */
        if ((chan_chk_chi (dva) >= 0) ||                /* channel int pending? */
            (dp_ctx[cidx].dp_ski & (1u << un))) {       /* seek int on sel unit? */
            *dvst |= (CC2 << DVT_V_CC);                 /* SIO fails */
            break;
            }
        for (i = 0; i < DP_NUMDR; i++) {                /* knock down other seek ints */
            if (dp_ctx[cidx].dp_ski & (1u << i)) {      /* seek int on unit? */
                dp_clr_ski (cidx, i);                   /* knock it down */
                sim_activate (&dp_unit[i + DP_SEEK], chan_ctl_time * 10);
                dp_unit[i + DP_SEEK].UCMD = DSC_SEEKW;  /* resched interrupt */
                }
            }
        if ((*dvst & (DVS_CST|DVS_DST)) == 0) {         /* ctrl + dev idle? */
            uptr->UCMD = DPS_INIT;                      /* start dev thread */
            sim_activate (uptr, chan_ctl_time);
            }
        break;

    case OP_TIO:                                        /* test status */
        *dvst = dp_tio_status (cidx, un);               /* return status */
        break;

    case OP_TDV:                                        /* test status */
        *dvst = dp_tdv_status (cidx, un);               /* return status */
        break;

    case OP_HIO:                                        /* halt I/O */
        *dvst = dp_tio_status (cidx, un);               /* return status */
        if (un != 0xF) {                                /* not controller */
            if ((int32) un == chan_chk_chi (dva))       /* halt active ctlr int? */
                chan_clr_chi (dva);                     /* clear ctlr int */
            if (sim_is_active (uptr)) {                 /* chan active? */
                sim_cancel (uptr);                      /* stop unit */
                chan_uen (dva);                         /* uend */
                }
            dp_clr_ski (cidx, un);                      /* clear seek int */
            sim_cancel (uptr + DP_SEEK);                /* cancel seek compl */
            }
        else {
            for (i = 0; i < DP_NUMDR; i++) {            /* do every unit */
                if (sim_is_active (&dp_unit[i])) {      /* chan active? */
                    sim_cancel (&dp_unit[i]);           /* cancel */
                    chan_uen ((dva & ~DVA_M_UNIT) | i); /* uend */
                    }
                dp_clr_ski (cidx, i);                   /* clear seek int */
                sim_cancel (&dp_unit[i + DP_SEEK]);     /* cancel seek compl */
                }
            chan_clr_chi (dva);                         /* clear chan int */
            }
        break;

    case OP_AIO:                                        /* acknowledge int */
        iu = dp_clr_int (cidx);                         /* clear int */
        *dvst = dp_aio_status (cidx, iu) |              /* get status */
                (iu << DVT_V_UN);
        break;

    default:
        *dvst = 0;
        return SCPE_IERR;
        }

return 0;
}

/* Unit service - reconstruct full device address on entry */

t_stat dp_svc (UNIT *uptr)
{
uint32 i, da, wd, wd1, c[DPS_NBY_16B];
uint32 cidx = uptr->UCTX;
UNIT *dp_unit = dp_dev[cidx].units;
uint32 un = uptr - dp_unit;
uint32 dva = dp_dib[cidx].dva | un;
uint32 dtype = GET_DTYPE (uptr->flags);
DP_CTX *ctx = &dp_ctx[cidx];
int32 t, dc;
uint32 st, cmd, sc;
t_stat r;

if (uptr->UCMD == DPS_INIT) {                           /* init state? */
    st = chan_get_cmd (dva, &cmd);                      /* get command */
    if (CHS_IFERR (st))                                 /* channel error? */
        return dp_chan_err (dva, st);
    ctx->dp_flags = 0;                                  /* clear status */
    if (!(dp_cmd[cmd] & (1u << ctx->dp_ctype))) {       /* cmd valid for dev? */
        ctx->dp_flags |= DPF_PGE;
        chan_uen (dva);                                 /* uend */
        return SCPE_OK;
        }
    if ((un == 0xF) &&                                  /* to controller? */
        !(dp_cmd[cmd] & C_C)) {                         /* not valid? */
        ctx->dp_flags |= DPF_PGE;
        chan_uen (dva);                                 /* uend */
        return SCPE_OK;
        }
    uptr->UCMD = cmd;                                   /* save state */
    if (dp_cmd[cmd] & C_F)                              /* fast command? */
        sim_activate_abs (uptr, chan_ctl_time);         /* schedule soon */
    else {                                              /* data transfer */
        sc = DPA_GETSC (uptr->UDA);                     /* new sector */
        t = sc - GET_PSC (ctx->dp_time, dp_tab[dtype].sc); /* delta to new */
        if (t < 0)                                      /* wrap around? */
            t = t + dp_tab[dtype].sc;
        sim_activate_abs (uptr, t * ctx->dp_time * DP_WDSC); /* schedule op */
        }
    sim_cancel (uptr + DP_SEEK);                        /* cancel rest of seek */
    return SCPE_OK;
    }
else if (uptr->UCMD == DPS_END) {                       /* end state? */
    st = chan_end (dva);                                /* set channel end */
    if (CHS_IFERR (st))                                 /* channel error? */
        return dp_chan_err (dva, st);
    if (st == CHS_CCH) {                                /* command chain? */
        uptr->UCMD = DPS_INIT;                          /* restart thread */
        sim_activate (uptr, chan_ctl_time);
        }
    return SCPE_OK;                                     /* done */
    }

da = 0;
dc = 0;
switch (uptr->UCMD) {

    case DPS_SEEK:                                      /* seek */
    case DPS_SEEKI:
        for (i = 0; i < 4; i++)
            c[i] = 0;
        for (i = 0, st = 0; (i < 4) && (st != CHS_ZBC); i++) {
            st = chan_RdMemB (dva, &c[i]);              /* get byte */
            if (CHS_IFERR (st))                         /* channel error? */
                return dp_chan_err (dva, st);
            }
        da = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
        if (c[0] & 0xFC)                                /* hi 6b non-zero? */
            ctx->dp_flags |= DPF_PGE;                   /* set prog err */
        if (((i != 4) || (st != CHS_ZBC)) &&            /* length error? */
            chan_set_chf (dva, CHF_LNTE)) {             /* care? */
            ctx->dp_flags |= DPF_PGE;                   /* set prog err */
            return SCPE_OK;
            }
        if (i < 4) {                                    /* at least 4? */
            chan_uen (dva);
            return SCPE_OK;
            }
        dc = DPA_GETCY (da);                            /* desired cyl */
    case DPS_RECAL:
    case DPS_RECALI:
        t = abs (DPA_GETCY (uptr->UDA) - dc);           /* get cyl diff */
        ctx->dp_flags = (ctx->dp_flags & ~DPF_DIFF) |
            ((t & DPF_M_DIFF) << DPF_V_DIFF);           /* save difference */
        if (t == 0)
            t = 1;
        uptr->UDA = da;                                 /* save addr */
        sim_activate (uptr + DP_SEEK, t * ctx->dp_stime);
        dp_unit[un + DP_SEEK].UCMD =                    /* sched seek */
            (chan_tst_cmf (dva, CMF_CCH)? DSC_SEEK: uptr->UCMD & 0x80);
        break;                                          /* sched end */

    case DPS_SENSE:                                     /* sense */
        for (i = 0; i < DPS_NBY_16B; i++)
            c[i] = 0;
        c[0] = (uptr->UDA >> 24) & 0xFF;                /* 0-3 = disk addr */
        c[1] = (uptr->UDA >> 16) & 0xFF;
        c[2] = (uptr->UDA >> 8) & 0xFF;
        c[3] = uptr->UDA & 0xFF;
        c[4] = GET_PSC (ctx->dp_time, dp_tab[dtype].sc) | /* curr sector */
            ((sim_is_active (uptr) && ((uptr->UCMD & 0x7F) == DPS_SEEK))? 0x80: 0);
        if (!DP_Q10B (ctx->dp_ctype)) {                 /* 16B only */
            c[5] = un | dp_tab[dtype].id;               /* unit # + id */
            if (ctx->dp_ctype == DP_C3281)              /* 3281 only */
                c[7] = un;                              /* unique id */
            c[10] = (ctx->dp_ski >> 8) & 0xFF;          /* seek intr */
            c[11] = ctx->dp_ski & 0xFF;
            }
        dp_set_sense (uptr, &c[0]);                     /* other bytes */
        for (i = 0, st = 0; (i < DPS_NBY) && (st != CHS_ZBC); i++) {
            st = chan_WrMemB (dva, c[i]);               /* store char */
            if (CHS_IFERR (st))                         /* channel error? */
                return dp_chan_err (dva, st);
            }
        if ((i != DPS_NBY) || (st != CHS_ZBC)) {        /* length error? */
            ctx->dp_flags |= DPF_PGE;                   /* set prog err */
            if (chan_set_chf (dva, CHF_LNTE))           /* do we care? */
                return SCPE_OK;
            }
        break;

    case DPS_WRITE:                                     /* write */
        if (uptr->flags & UNIT_RO) {                    /* write locked? */
            ctx->dp_flags |= DPF_WPE;
            chan_uen (dva);                             /* uend */
            return SCPE_OK;
            }
        if (dp_inv_ad (uptr, &da)) {                    /* invalid addr? */
            ctx->dp_flags |= DPF_PGE;
            chan_uen (dva);                             /* uend */
            return SCPE_OK;
            }
        for (i = 0, st = 0; i < DP_WDSC; i++) {         /* sector loop */
            if (st != CHS_ZBC) {                        /* chan not done? */
                st = chan_RdMemW (dva, &wd);            /* read word */
                if (CHS_IFERR (st)) {                   /* channel error? */
                    dp_inc_ad (uptr);                   /* da increments */
                    return dp_chan_err (dva, st);
                    }
                }
            else wd = 0;
            dp_buf[i] = wd;                             /* store in buffer */
            }
        if ((r = dp_write (uptr, da)))                  /* write buf, err? */
            return r;
        if (dp_end_sec (uptr, DP_WDSC, DP_WDSC, st))    /* transfer done? */
            return SCPE_OK;                             /* err or cont */
        break;

/* Write header "writes" eight bytes per sector and throws them in the bit bucket */

    case DPS_WHDR:
        if (uptr->flags & UNIT_RO) {                    /* write locked? */
            ctx->dp_flags |= DPF_WPE;
            chan_uen (dva);                             /* uend */
            return SCPE_OK;
            }
        if (dp_inv_ad (uptr, &da)) {                    /* invalid addr? */
            ctx->dp_flags |= DPF_PGE;
            chan_uen (dva);                             /* uend */
            return SCPE_OK;
            }
        if (DPA_GETSC (uptr->UDA) != 0) {               /* must start at sec 0 */
            ctx->dp_flags |= DPF_SNZ;
            chan_uen (dva);
            return SCPE_OK;
            }
        for (i = 0, st = 0; (i < 8) && (st != CHS_ZBC); i++) { /* sector loop */
            if (st != CHS_ZBC) {                        /* chan not done? */
                st = chan_RdMemB (dva, &wd);            /* read word */
                if (CHS_IFERR (st)) {                   /* channel error? */
                    dp_inc_ad (uptr);                   /* da increments */
                    return dp_chan_err (dva, st);
                    }
                }
            }
        if (dp_end_sec (uptr, i, 8, st))                /* transfer done? */
            return SCPE_OK;                             /* err or cont */

/* Write check must be done by bytes to get precise miscompare */

    case DPS_CHECK:                                     /* write check */
        if (dp_inv_ad (uptr, &da)) {                    /* invalid addr? */
            ctx->dp_flags |= DPF_PGE;
            chan_uen (dva);                             /* uend */
            return SCPE_OK;
            }
        if ((r = dp_read (uptr, da)))                   /* read buf, error? */
            return r;
        for (i = 0, st = 0; (i < (DP_WDSC * 4)) && (st != CHS_ZBC); i++) {
            st = chan_RdMemB (dva, &wd);                /* read byte */
            if (CHS_IFERR (st)) {                       /* channel error? */
                dp_inc_ad (uptr);                       /* da increments */
                return dp_chan_err (dva, st);
                }
            wd1 = (dp_buf[i >> 2] >> (24 - ((i & 0x3) * 8))) & 0xFF; /* byte */
            if (wd != wd1) {                            /* check error? */
                dp_inc_ad (uptr);                       /* da increments */
                ctx->dp_flags |= DPF_WCHK;              /* set status */
                chan_uen (dva);                         /* force uend */
                return SCPE_OK;
                }
            }        
        if (dp_end_sec (uptr, i, DP_WDSC * 4, st))      /* transfer done? */
            return SCPE_OK;                             /* err or cont */
        break;

    case DPS_READ:                                      /* read */
        if (dp_inv_ad (uptr, &da)) {                    /* invalid addr? */
            ctx->dp_flags |= DPF_PGE;
            chan_uen (dva);                             /* uend */
                return SCPE_OK;
            }
        if ((r = dp_read (uptr, da)))                   /* read buf, error? */
            return r;
        for (i = 0, st = 0; (i < DP_WDSC) && (st != CHS_ZBC); i++) {
            st = chan_WrMemW (dva, dp_buf[i]);          /* store in mem */
            if (CHS_IFERR (st)) {                       /* channel error? */
                dp_inc_ad (uptr);                       /* da increments */
                return dp_chan_err (dva, st);
                }
            }
        if (dp_end_sec (uptr, i, DP_WDSC, st))          /* transfer done? */
            return SCPE_OK;                             /* err or cont */
        break;

/* Read header reads 8 bytes per sector */

    case DPS_RHDR:                                      /* read header */
        if (dp_inv_ad (uptr, &da)) {                    /* invalid addr? */
            ctx->dp_flags |= DPF_PGE;
            chan_uen (dva);                             /* uend */
                return SCPE_OK;
            }
       c[0] = c[5] = c[6] = c[7] = 0;
       wd = DPA_GETCY (uptr->UDA);
       c[1] = (wd >> 8) & 0xFF;
       c[2] = wd & 0xFF;
       c[3] = DPA_GETHD (uptr->UDA);
       c[4] = DPA_GETSC (uptr->UDA);
       for (i = 0, st = 0; (i < 8) && (st != CHS_ZBC); i++) {
            st = chan_WrMemB (dva, c[i]);               /* store in mem */
            if (CHS_IFERR (st)) {                       /* channel error? */
                dp_inc_ad (uptr);                       /* da increments */
                return dp_chan_err (dva, st);
                }
            }
        if (dp_end_sec (uptr, i, 8, st))                /* transfer done? */
            return SCPE_OK;                             /* err or cont */
        break;

/* Test mode is not really implemented */

    case DPS_TEST:                                      /* test mode */
        if (!dp_test_mode (cidx))                       /* enter test mode */
            return SCPE_OK;
        break;

    case DPS_RSRV:                                      /* reserve */
    case DPS_RLS:                                       /* release */
    case DPS_RLSA:                                      /* release */
        break;                                          /* nop */
        }

uptr->UCMD = DPS_END;                                   /* op done, next state */
sim_activate (uptr, chan_ctl_time);
return SCPE_OK;
}

/* Seek completion service */

t_stat dps_svc (UNIT *uptr)
{
uint32 cidx = uptr->UCTX;
DP_CTX *ctx = &dp_ctx[cidx];
UNIT *dp_unit = dp_dev[cidx].units;
uint32 un = uptr - dp_unit - DP_SEEK;
uint32 dtype = GET_DTYPE (dp_unit[un].flags);

if (uptr->UCMD != DSC_SEEK) {                           /* int? */
    if (chan_chk_chi (dp_dib[cidx].dva) >= 0) {         /* ctl int pending? */
        sim_activate (uptr, ctx->dp_time * dp_tab[dtype].sc);
        uptr->UCMD = DSC_SEEKW;
        }
    else dp_set_ski (cidx, un);
    }
return SCPE_OK;
}

/* Common read/write sector end routine 

   case 1 - more to transfer, not end cylinder - reschedule, return TRUE
   case 2 - more to transfer, end cylinder - uend, return TRUE
   case 3 - transfer done, length error - uend, return TRUE
   case 4 - transfer done, no length error - return FALSE (sched end state)
*/

t_bool dp_end_sec (UNIT *uptr, uint32 lnt, uint32 exp, uint32 st)
{
uint32 cidx = uptr->UCTX;
uint32 un = uptr - dp_dev[cidx].units;
uint32 dva = dp_dib[cidx].dva | un;
DP_CTX *ctx = &dp_ctx[cidx];

if (st != CHS_ZBC) {                                    /* end record? */
    if (dp_inc_ad (uptr)) {                             /* inc addr, cross cyl? */
        ctx->dp_flags |= (DPF_IVA | DPF_EOC);
        chan_uen (dva);                                 /* uend */
        }
    else sim_activate (uptr, ctx->dp_time * 16);        /* no, next sector */
    return TRUE;
    }
dp_inc_ad (uptr);                                       /* just incr addr */
if (lnt != exp) {                                       /* length error at end? */
    if (exp == 8)                                       /* hdr op? */
        ctx->dp_flags |= DPF_PGE;                       /* set PGE */
    if (chan_set_chf (dva, CHF_LNTE))                   /* do we care? */
        return TRUE;
    }
return FALSE;                                           /* cmd done */
}

/* DP status routines */

/* The ctrl is busy if any drive is busy.
   The device is busy if either the main unit or the seek unit is busy */

uint32 dp_tio_status (uint32 cidx, uint32 un)
{
uint32 i;
DP_CTX *ctx = &dp_ctx[cidx];
UNIT *dp_unit = dp_dev[cidx].units;
uint32 stat = DVS_AUTO;

for (i = 0; i < DP_NUMDR; i++) {
    if (sim_is_active (&dp_unit[i])) {
        stat |= (DVS_CBUSY | (CC2 << DVT_V_CC));
        break;
        }
    }
if (sim_is_active (&dp_unit[un]) ||
    sim_is_active (&dp_unit[un + DP_SEEK]))
    stat |= (DVS_DBUSY | (CC2 << DVT_V_CC));
return stat;
}

uint32 dp_tdv_status (uint32 cidx, uint32 un)
{
uint32 st;
UNIT *dp_unit = dp_dev[cidx].units;
t_bool on_cyl;

st = 0;
on_cyl = !sim_is_active (&dp_unit[un + DP_SEEK]) ||
    (dp_unit[un + DP_SEEK].UCMD == DSC_SEEKW);
if (DP_Q10B (dp_ctx[cidx].dp_ctype))
    st = ((dp_ctx[cidx].dp_flags & (DPF_IVA|DPF_PGE))? 0x20: 0) |
        (on_cyl? 0x04: 0);
else st = ((dp_ctx[cidx].dp_flags & DPF_PGE)? 0x20: 0) |
        ((dp_ctx[cidx].dp_flags & DPF_WPE)? 0x08: 0);
return st;
}

uint32 dp_aio_status (uint32 cidx, uint32 un)
{
uint32 st;
UNIT *dp_unit = dp_dev[cidx].units;
t_bool on_cyl;

st = 0;
on_cyl = !sim_is_active (&dp_unit[un + DP_SEEK]) ||
    (dp_unit[un + DP_SEEK].UCMD == DSC_SEEKW);
if ((DP_Q10B (dp_ctx[cidx].dp_ctype)) && on_cyl)
    st |= 0x04;
if (chan_chk_chi (dp_dib[cidx].dva) < 0)
    st |= 0x08;
return st;
}

/* Set sense status */

void dp_set_sense (UNIT *uptr, uint32 *c)
{
uint32 cidx = uptr->UCTX;
UNIT *sptr = uptr + DP_SEEK;
DP_CTX *ctx = &dp_ctx[cidx];
uint8 data;
DP_SNSTAB *tptr;

if (sim_is_active (sptr) &&
    (sptr->UCMD != DSC_SEEKW))
    ctx->dp_flags |= DPF_AIM;
else ctx->dp_flags &= ~DPF_AIM;
if (DP_Q10B (ctx->dp_ctype))
    tptr = dp_sense_10B;
else tptr = dp_sense_16B;
while (tptr->byte != 0) {
    if (ctx->dp_flags & tptr->mask) {
        data = (uint8) ((ctx->dp_flags & tptr->mask) >> tptr->fpos);
        c[tptr->byte] |= (data << tptr->tpos);
        }
    tptr++;
    }
return;
}

/* Validate disk address */

t_bool dp_inv_ad (UNIT *uptr, uint32 *da)
{
uint32 dtype = GET_DTYPE (uptr->flags);
uint32 cy = DPA_GETCY (uptr->UDA);
uint32 hd = DPA_GETHD (uptr->UDA);
uint32 sc = DPA_GETSC (uptr->UDA);

if ((cy >= dp_tab[dtype].cy) ||
    (hd >= dp_tab[dtype].hd) ||
    (sc >= dp_tab[dtype].sc))
    return TRUE;
if (da)                                                 /* return word addr */
    *da = ((((cy * dp_tab[dtype].hd) + hd) * dp_tab[dtype].sc) + sc) * DP_WDSC;
return FALSE;
}

/* Increment disk address */

t_bool dp_inc_ad (UNIT *uptr)
{
uint32 dtype = GET_DTYPE (uptr->flags);
uint32 cy = DPA_GETCY (uptr->UDA);
uint32 hd = DPA_GETHD (uptr->UDA);
uint32 sc = DPA_GETSC (uptr->UDA);

sc = sc + 1;                                            /* sector++ */
if (sc >= dp_tab[dtype].sc) {                           /* overflow? */
    sc = 0;                                             /* wrap sector */
    hd = hd + 1;                                        /* head++ */
    if (hd >= dp_tab[dtype].hd)                         /* overflow? */
        hd = 0;                                         /* wrap heads */
    }
uptr->UDA = (cy << DPA_V_CY) | (hd << DPA_V_HD) | (sc << DPA_V_SC);
if ((hd == 0) && (sc == 0))
    return TRUE;
return FALSE;
}

/* Read and write sector */

t_stat dp_read (UNIT *uptr, uint32 da)
{
int32 err, awc;

err = fseek (uptr->fileref, da * sizeof (int32), SEEK_SET);
if (err == 0) {
    awc = fxread (dp_buf, sizeof (uint32), DP_WDSC, uptr->fileref);
    err = ferror (uptr->fileref);
    for (; awc < DP_WDSC; awc++)                        /* fill buf */
       dp_buf[awc] = 0;
    }
if (err != 0)
    return dp_ioerr (uptr);
return SCPE_OK;
}

t_stat dp_write (UNIT *uptr, uint32 da)
{
int32 err;

err = fseek (uptr->fileref, da * sizeof (int32), SEEK_SET);
if (err == 0) {
    fxwrite (dp_buf, sizeof (uint32), DP_WDSC, uptr->fileref);
    err = ferror (uptr->fileref);
    }
if (err != 0)
    return dp_ioerr (uptr);
return SCPE_OK;
}

t_stat dp_ioerr (UNIT *uptr)
{
uint32 cidx = uptr->UCTX;
uint32 un = uptr - dp_dev[cidx].units;
uint32 dva = dp_dib[cidx].dva | un;

perror ("DP I/O error");
clearerr (uptr->fileref);
dp_ctx[cidx].dp_flags |= DPF_DPE;                       /* set DPE flag */
chan_set_chf (dva, CHF_XMDE);
chan_uen (dva);                                         /* force uend */
return SCPE_IOERR;
}

/* Test mode */

t_bool dp_test_mode (uint32 cidx)
{
DP_CTX *ctx = &dp_ctx[cidx];
uint32 dva = dp_dib[cidx].dva;
uint32 i, st, wd;

ctx->dp_test = 0;
for (i = 0, st = 0; i < DPT_NBY; i++) {                 /* sector loop */
    if (st != CHS_ZBC) {                                /* chan not done? */
        st = chan_RdMemB (dva, &wd);                    /* read word */
        if (CHS_IFERR (st)) {
            dp_chan_err (dva, st);
            return FALSE;
            }
        }
    else wd = 0;
    ctx->dp_test |= (wd & 0xFF) << (i * 8);
    }
return TRUE;
}

/* Channel error */

t_stat dp_chan_err (uint32 dva, uint32 st)
{
chan_uen (dva);                                         /* uend */
if (st < CHS_ERR)
    return st;
return SCPE_OK;
}

/* Clear controller/device interrupt */

int32 dp_clr_int (uint32 cidx)
{
int32 iu;
DP_CTX *ctx = &dp_ctx[cidx];

if ((iu = chan_clr_chi (dp_dib[cidx].dva)) >= 0) {      /* chan int? clear */
    if (ctx->dp_ski != 0)                               /* more int? */
        chan_set_dvi (dp_dib[cidx].dva);                /* set INP */
    return iu;
    }
for (iu = 0; iu < (int32) DP_NUMDR; iu++) {             /* seek int? */
    if (ctx->dp_ski & (1u << iu)) {
        dp_clr_ski (cidx, iu);                          /* clear */
        return iu;
        }
    }
return 0;
}

/* Set seek interrupt */

void dp_set_ski (uint32 cidx, uint32 un)
{
dp_ctx[cidx].dp_ski |= (1u << un);
chan_set_dvi (dp_dib[cidx].dva);                        /* set INP */
return;
}

/* Clear seek interrupt */

void dp_clr_ski (uint32 cidx, uint32 un)
{
dp_ctx[cidx].dp_ski &= ~(1u << un);                     /* clear */
if (dp_ctx[cidx].dp_ski != 0)                           /* more int? */
    chan_set_dvi (dp_dib[cidx].dva);                    /* set INP */
else if (chan_chk_chi (dp_dib[cidx].dva) < 0)           /* any int? */
    chan_clr_chi (dp_dib[cidx].dva);                    /* clr INP */
return;
}

/* Reset routines */

void dp_reset_unit (UNIT *uptr, uint32 cidx)
{
sim_cancel (uptr);                                      /* stop dev thread */
uptr->UDA = 0;
uptr->UCMD = 0;
uptr->UCTX = cidx;
return;
}

t_stat dp_reset (DEVICE *dptr)
{
uint32 i;
uint32 cidx = dptr - dp_dev;
UNIT *dp_unit;
DP_CTX *ctx;

if (cidx >= DP_NUMCTL)
    return SCPE_IERR;
dp_unit = dptr->units;
ctx = &dp_ctx[cidx];
dp_reset_unit (&dp_unit[DP_CONT], cidx);                /* reset controller */
for (i = 0; i < DP_NUMDR_16B; i++) {                    /* reset drives */
    dp_reset_unit (&dp_unit[i], cidx);
    dp_reset_unit (&dp_unit[i + DP_SEEK], cidx);        /* reset seek thread */
    }
ctx->dp_flags = 0;
ctx->dp_ski = 0;
ctx->dp_test = 0;
chan_reset_dev (dp_dib[cidx].dva);                      /* clr int, active */
return SCPE_OK;
}

/* Device attach */

t_stat dp_attach (UNIT *uptr, CONST char *cptr)
{
uint32 i, p;
t_stat r;

uptr->capac = dp_tab[GET_DTYPE (uptr->flags)].capac;
r = attach_unit (uptr, cptr);                           /* attach unit */
if (r != SCPE_OK)                                       /* error? */
    return r;
if ((uptr->flags & UNIT_AUTO) == 0)                     /* autosize? */
    return SCPE_OK;
p = sim_fsize (uptr->fileref);
if (p == 0)                                             /* new file? */
    return SCPE_OK;
for (i = 0; dp_tab[i].sc != 0; i++) {
    if ((dp_tab[i].ctype == DP_C3281) &&                /* only on 3281 */
        (p <= (dp_tab[i].capac * (uint32) sizeof (int32)))) {
        uptr->flags = (uptr->flags & ~UNIT_DTYPE) | (i << UNIT_V_DTYPE);
        uptr->capac = dp_tab[i].capac;
        return SCPE_OK;
        }
    }
return SCPE_OK;
}

/* Set drive type command validation routine */

t_stat dp_set_size (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
uint32 dtype = GET_DTYPE (val);
uint32 cidx = uptr->UCTX;

if (cidx >= DP_NUMCTL)                                  /* valid ctrl idx? */
    return SCPE_IERR;
if (uptr->flags & UNIT_ATT)                             /* unattached? */
    return SCPE_ALATT;
if (dp_ctx[cidx].dp_ctype != DP_C3281)                  /* only on 3281 */
    return SCPE_NOFNC;
uptr->capac = dp_tab[dtype].capac;
return SCPE_OK;
}

/* Set unit autosize validation routine */

t_stat dp_set_auto (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
uint32 cidx = uptr->UCTX;

if (cidx >= DP_NUMCTL)                                  /* valid ctrl idx? */
    return SCPE_IERR;
if (uptr->flags & UNIT_ATT)                             /* unattached? */
    return SCPE_ALATT;
if (dp_ctx[cidx].dp_ctype != DP_C3281)                  /* only on 3281 */
    return SCPE_NOFNC;
return SCPE_OK;
}

/* Set controller type command validation routine */

t_stat dp_set_ctl (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
uint32 i, new_dtyp, cidx = uptr->UCTX;
DP_CTX *ctx = &dp_ctx[cidx];
UNIT *dp_unit = dp_dev[cidx].units;

if ((cidx >= DP_NUMCTL) || (val >= DP_CTYPE))           /* valid ctrl idx? */
    return SCPE_IERR;
if (val == dp_ctx[cidx].dp_ctype)                       /* no change? */
    return SCPE_OK;
for (i = 0; i < DP_NUMDR; i++) {                        /* all units detached? */
    if ((dp_unit[i].flags & UNIT_ATT) != 0)
        return SCPE_ALATT;
    }
for (i = new_dtyp = 0; dp_tab[i].sc != 0; i++) {        /* find default capac */
    if (dp_tab[i].ctype == val) {
        new_dtyp = i;
        break;
        }
    }
if (dp_tab[new_dtyp].sc == 0)
    return SCPE_IERR;
ctx->dp_ctype = val;                                    /* new ctrl type */
for (i = 0; i < DP_NUMDR_16B; i++) {
    if (i >= DP_NUMDR)
        dp_unit[i].flags = (dp_unit[i].flags & ~UNIT_DISABLE) | UNIT_DIS;
    else dp_unit[i].flags = (dp_unit[i].flags | UNIT_DISABLE) & ~UNIT_DIS;
    if (val != DP_C3281)
        dp_unit[i].flags &= ~UNIT_AUTO;
    dp_unit[i].flags = (dp_unit[i].flags & ~UNIT_DTYPE) |
           (new_dtyp << UNIT_V_DTYPE);
    dp_unit[i].capac = dp_tab[new_dtyp].capac;
    }
return SCPE_OK;
}

t_stat dp_show_ctl (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
uint32 cidx = uptr->UCTX;

if (cidx >= DP_NUMCTL)                                 /* valid ctrl idx? */
    return SCPE_IERR;
if (dp_ctx[cidx].dp_ctype >= DP_CTYPE)
    return SCPE_IERR;
fprintf (st, "%s controller", dp_cname[dp_ctx[cidx].dp_ctype]);
return SCPE_OK;
}

