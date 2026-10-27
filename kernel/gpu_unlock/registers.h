/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_REGISTERS_H
#define GPU_UNLOCK_REGISTERS_H

/* BAR0 registers used by the Ampere recipes and firmware transaction.
 * Names follow the pinned NVIDIA 610.43.03 register headers and cmpunlocker
 * PLM table. Falcon/FBIF/Falcon2 offsets are relative to their engine bases.
 */
#define GU_PMC_BOOT_0_OFFSET          0x00000000u
#define GU_PROM_DATA_OFFSET           0x00300000u
#define GU_BUS_SW_SCRATCH(index)       (0x00001400u + (index) * 4u)
#define GU_ACR_SCRATCH_OFFSET         GU_BUS_SW_SCRATCH(14)

#define GU_GSP_BASE                   0x00110000u
#define GU_GSP_FBIF_BASE              0x00110600u
#define GU_GSP_FALCON2_BASE           0x00111000u
#define GU_SEC2_BASE                  0x00840000u
#define GU_SEC2_FBIF_BASE             0x00840600u
#define GU_SEC2_FALCON2_BASE          0x00841000u
#define GU_FALCON2_WINDOW_SIZE        0x00001000u

#define GU_FALCON_MAILBOX0_OFFSET     0x00000040u
#define GU_FALCON_MAILBOX1_OFFSET     0x00000044u
#define GU_FALCON_RM_OFFSET           0x00000084u
#define GU_FALCON_HWCFG2_OFFSET       0x000000f4u
#define GU_FALCON_CPUCTL_OFFSET       0x00000100u
#define GU_FALCON_BOOTVEC_OFFSET      0x00000104u
#define GU_FALCON_HWCFG_OFFSET        0x00000108u
#define GU_FALCON_DMACTL_OFFSET       0x0000010cu
#define GU_FALCON_DMATRFBASE_OFFSET   0x00000110u
#define GU_FALCON_DMATRFMOFFS_OFFSET  0x00000114u
#define GU_FALCON_DMATRFCMD_OFFSET    0x00000118u
#define GU_FALCON_DMATRFFBOFFS_OFFSET 0x0000011cu
#define GU_FALCON_DMATRFBASE1_OFFSET  0x00000128u
#define GU_FALCON_CPUCTL_ALIAS_OFFSET 0x00000130u
/* Host IMEM/DMEM access port 0. */
#define GU_FALCON_IMEMC_OFFSET        0x00000180u
#define GU_FALCON_IMEMD_OFFSET        0x00000184u
#define GU_FALCON_IMEMT_OFFSET        0x00000188u
#define GU_FALCON_DMEMC_OFFSET        0x000001c0u
#define GU_FALCON_DMEMD_OFFSET        0x000001c4u
#define GU_FALCON_ENGINE_OFFSET       0x000003c0u
#define GU_FALCON_FHSTATE_OFFSET      0x0000005cu
#define GU_SEC2_CSBERRSTAT_OFFSET     0x00000244u
#define GU_FALCON_RESET_PLM_OFFSET    0x000003c4u
#define GU_SEC2_RESET_PLM_OFFSET      (GU_SEC2_BASE + GU_FALCON_RESET_PLM_OFFSET)

#define GU_FBIF_TRANSCFG(index)       (0x00000000u + (index) * 4u)
#define GU_FBIF_CTL_OFFSET            0x00000024u
#define GU_FALCON2_MOD_SEL_OFFSET             0x00000180u
#define GU_FALCON2_BROM_CURR_UCODE_ID_OFFSET  0x00000198u
#define GU_FALCON2_BROM_ENGIDMASK_OFFSET       0x0000019cu
/* BROM parameter-address register 0 and RISC-V boot control. */
#define GU_FALCON2_BROM_PARAADDR_OFFSET        0x00000210u
#define GU_RISCV_BCR_CTRL_OFFSET              0x00000668u

#define GU_FUSE_SEC2_UCODE1           0x00824140u
#define GU_FUSE_GSP_UCODE1            0x008241c0u

#define GU_GFW_PLM_OFFSET             0x00118128u
#define GU_GFW_BOOT_OFFSET            0x00118234u
#define GU_GFW_CPUCTL_OFFSET          (GU_GSP_BASE + GU_FALCON_CPUCTL_OFFSET)
/* PGC6 AON_SECURE_SCRATCH_GROUP_42 reports framebuffer size in MiB. */
#define GU_GFW_FB_SIZE_OFFSET         0x001183a4u
#define GU_VGA_WORKSPACE_BASE_OFFSET  0x00625f04u
#define GU_MMU_LOCK_CFG_PLM_OFFSET    0x001fa7c8u
#define GU_MMU_LOCK_ADDR_LO_OFFSET    0x001fa82cu
#define GU_MMU_LOCK_ADDR_HI_OFFSET    0x001fa830u
#define GU_WPR2_ADDR_LO_OFFSET        0x001fa824u
#define GU_WPR2_ADDR_HI_OFFSET        0x001fa828u
#define GU_WPR2_ALIGN                 0x00020000u

#define GU_WPR_CFG_PLM_OFFSET         0x001fa7ccu
#define GU_FBPA_PLM_OFFSET            0x009a0148u
#define GU_WPR_PLM_OFFSET             0x001fa7c4u
#define GU_FEAT_PLM_OFFSET            0x00823804u
#define GU_XVE_PLM_OFFSET             0x00088ff4u
#define GU_XVE_B_PLM_OFFSET           0x00088ab4u
#define GU_XVE_C_PLM_OFFSET           0x00088ff8u
#define GU_FEAT2_PLM_OFFSET           0x00823b00u
#define GU_OPT_PLM_OFFSET             0x008200fcu
#define GU_RECONFIG_PLM_OFFSET        0x008200d8u
#define GU_PJTAG_PLM_OFFSET           0x0000c840u
#define GU_PJTAG_SEC_PLM_OFFSET       0x0000c848u
#define GU_FEAT_READOUT_1_OFFSET      0x00823818u
#define GU_SPEED_READOUT_MASK         0x07ffffffu
#define GU_FEAT_SS0_OFFSET            0x0082381cu
#define GU_FEAT_SS1_OFFSET            0x00823820u
/* SS0 has eight 4-bit fields; SS1 has one. Value 8 selects full rate. */
#define GU_SS0_UNLOCK                 0x88888888u
#define GU_SS1_UNLOCK                 0x8u

#endif
