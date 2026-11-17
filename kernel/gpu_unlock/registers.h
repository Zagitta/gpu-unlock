/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_REGISTERS_H
#define GPU_UNLOCK_REGISTERS_H

/* BAR0 registers used by the Ampere recipes and firmware transaction.
 * Names follow the pinned NVIDIA 610.43.03 register headers and cmpunlocker
 * PLM table. Falcon/FBIF/Falcon2 offsets are relative to their engine bases.
 */
#define GU_PMC_BOOT_0_OFFSET          0x00000000u
#define GU_PROM_DATA_OFFSET           0x00300000u
#define GU_PROM_2_DATA_OFFSET         0x00109000u
#define GU_ROM_ADDR_OFFSET_2          0x0000e21cu
#define GU_ROM_WINDOW_OFFSET          0x0000e218u
#define GU_BUS_SW_SCRATCH(index)       (0x00001400u + (index) * 4u)
#define GU_ACR_SCRATCH_OFFSET         GU_BUS_SW_SCRATCH(14)
#define GU_PREOS_REQUEST_OFFSET       GU_BUS_SW_SCRATCH(1)
#define GU_PREOS_PROGRESS_OFFSET      GU_BUS_SW_SCRATCH(6)
#define GU_PREOS_EXIT_REQUEST         (1u << 9)
#define GU_PMU_BASE                   0x0010a000u
#define GU_PMU_RISCV_BASE             0x0010b000u
#define GU_RISCV_CPUCTL_OFFSET        0x00000388u

#define GU_GSP_BASE                   0x00110000u
#define GU_GSP_FBIF_BASE              0x00110600u
#define GU_GSP_FALCON2_BASE           0x00111000u
#define GU_GSP_RISCV_STATUS_OFFSET   0x00111240u
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
#define GU_FALCON_BAR0_CSR_OFFSET     0x00000700u
#define GU_FALCON_BAR0_ADDR_OFFSET    0x00000704u
#define GU_FALCON_BAR0_DATA_OFFSET    0x00000708u
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
#define GU_INFOROM_CARVEOUT_OFFSET    0x00118238u
#define GU_GFW_CPUCTL_OFFSET          (GU_GSP_BASE + GU_FALCON_CPUCTL_OFFSET)
/* PGC6 AON_SECURE_SCRATCH_GROUP_42 reports framebuffer size in MiB. */
#define GU_GFW_FB_SIZE_OFFSET         0x001183a4u
#define GU_GFW_FB_SIZE_PLM_OFFSET     0x00118390u
#define GU_MEMORY_RANGE_MIRROR_OFFSET 0x001180f0u
#define GU_MEMORY_RANGE_MIRROR_PLM_OFFSET 0x001180b0u
#define GU_VGA_WORKSPACE_BASE_OFFSET  0x00625f04u
#define GU_MMU_LOCK_CFG_PLM_OFFSET    0x001fa7c8u
#define GU_MMU_LOCK_ADDR_LO_OFFSET    0x001fa82cu
#define GU_MMU_LOCK_ADDR_HI_OFFSET    0x001fa830u
#define GU_WPR2_ADDR_LO_OFFSET        0x001fa824u
#define GU_WPR2_ADDR_HI_OFFSET        0x001fa828u
#define GU_WPR2_ALIGN                 0x00020000u

/* GA102 boot VPR reservation and secure scrubber handoff. */
#define GU_VPR_PLM_OFFSET             0x001fa7d0u
#define GU_VPR_ADDR_LO_OFFSET         0x001fa804u
#define GU_VPR_ADDR_HI_OFFSET         0x001fa808u
#define GU_VPR_CONTROL_OFFSET         0x001fa80cu
#define GU_VPR_IN_USE                 (1u << 4)
#define GU_VPR_BOOT_SIZE_OFFSET       0x001180f4u
#define GU_VPR_BOOT_SIZE_PLM_OFFSET   0x001180b4u
#define GU_VPR_BOOT_HANDOFF_OFFSET    0x001180fcu
#define GU_VPR_BOOT_HANDOFF_PLM_OFFSET 0x001180bcu
#define GU_VPR_HANDOFF_MASK           0xe0000000u
#define GU_VPR_HANDOFF_DEVINIT        0x20000000u
#define GU_VPR_HANDOFF_TAKING_OVER    0x40000000u
#define GU_VPR_HANDOFF_DONE           0x60000000u
#define GU_VPR_SYNC_SCRUB_MIB_OFFSET  0x000205f4u
#define GU_GC6_INTERRUPT_OFFSET       0x00118c50u
#define GU_GC6_EXIT_PENDING           (1u << 27)
#define GU_GC6_HANDOFF_OFFSET         0x000205dcu
#define GU_DISPLAY_FUSE_STATUS_OFFSET 0x00820c04u
#define GU_DISPLAY_VPR_PLM_OFFSET     0x00624900u
#define GU_DISPLAY_VPR_POLICY_OFFSET  0x00624904u

#define GU_WPR_CFG_PLM_OFFSET         0x001fa7ccu
#define GU_PLM_HOST_WRITE             0x00000010u /* level 0 write access */
#define GU_FBPA_ECC_PLM_OFFSET 0x009a0170u
#define GU_FBPA_PLM_OFFSET            0x009a0148u
#define GU_FBPA_CONFIG_PLM_OFFSET     0x009a0168u
#define GU_FBPA_HBMPLL_PLM_OFFSET     0x009a3c7cu
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

/* GA100/GA10x/Ada PCIe mailbox permissions and secure-option read override.
 * Use SYS trap 30; GA102 reserves trap 31 for the IFF requester. */
#define GU_P2P_PLM_OFFSET             0x0013affcu
#define GU_P2P_SECURE_OFFSET          0x00821030u
#define GU_P2P_TRAP_MATCH_OFFSET      0x00122478u
#define GU_P2P_TRAP_MASK_OFFSET       0x001224f8u
#define GU_P2P_TRAP_DATA1_OFFSET      0x00122578u
#define GU_P2P_TRAP_DATA2_OFFSET      0x001225f8u
#define GU_P2P_TRAP_ACTION_OFFSET     0x00122678u
#define GU_P2P_TRAP_CFG_OFFSET        0x001226f8u
#define GU_P2P_TRAP_PLM_OFFSET        0x00122778u

/* Further GA100 PLMs from cmpunlocker 2d10f891 pcie-gen2.patch and
 * ecc-enable.patch. The OPT block PLMs are named in fuses.c. */
#define GU_XP3G_PLM(index)            (0x0008e1b0u + (index) * 4u)
#define GU_XVE_D_PLM(index)           (0x00088fe8u + (index) * 4u)
#define GU_OPT_BLOCK_PLM(offset)      (0x00820000u + (offset))
#define GU_FEAT_ECC_PLM_OFFSET        0x00823800u
#define GU_WPR_PLM_C0_OFFSET          0x001fa7c0u
#define GU_WPR_PLM_D0_OFFSET          0x001fa7d0u

/* GA100 memory geometry. The stock driver sizes FB from the local memory
 * range register on GA100, so writing it before nvidia loads is enough. */
#define GU_FBPA_CFG1_OFFSET           0x009a0204u
#define GU_LOCAL_MEMORY_RANGE_OFFSET  0x00100ce0u

/* NISO framebuffer scrubber: physical addresses in 4 KiB pages, inclusive end. */
#define GU_NISO_SCRUB_PLM_OFFSET      0x00100b10u
#define GU_NISO_SCRUB_START_OFFSET    0x00100b14u
#define GU_NISO_SCRUB_END_OFFSET      0x00100b18u
#define GU_NISO_SCRUB_TRIGGER_OFFSET  0x00100b1cu
#define GU_NISO_SCRUB_STATUS_OFFSET   0x00100b20u
#define GU_NISO_SCRUB_LAST_OFFSET     0x00100b24u
#define GU_NISO_SCRUB_PATTERN_OFFSET  0x00100b28u
#define GU_NISO_SCRUB_FIRST_OFFSET    0x00100b2cu

/* XVE resizable BAR capability for BAR1. */
#define GU_XVE_BAR1_CYA_OFFSET        0x00088724u
#define GU_XVE_BAR1_REBAR_OFFSET      0x00088dccu

/* GA100 PCIe link configuration, cmpunlocker pcie-gen2.patch names. The OPT
 * block keeps the Gen4 speed mirror next to GEN23; both are fuse shadows that
 * may ignore a host write. LnkCap2 is the supported-speed vector and the only
 * readback that proves the Gen2 policy took. */
#define GU_OPT_PCIE_GEN23_OFFSET      0x0082057cu
#define GU_OPT_PCIE_MAGIC_OFFSET      0x00820520u
#define GU_XP3G_OVR(index)            (0x0008e110u + (index) * 4u)
#define GU_XP3G_VAL(index)            (0x0008e120u + (index) * 4u)
#define GU_XVE_PRIV_MISC_1_OFFSET     0x0008841cu
#define GU_XVE_VSEC_DEVICE_OFFSET     0x0008860cu
#define GU_XVE_VSEC_HIERARCHY_OFFSET  0x00088610u
#define GU_XVE_OVERRIDE_OFFSET        0x0008872cu
#define GU_XVE_LINK_CAP_2_OFFSET      0x000880a4u
#define GU_XVE_LINK_CTRL_2_OFFSET     0x000880a8u
#define GU_XP_LINK_CONFIG_0_OFFSET    0x0008c040u
#define GU_XP_PL_LINK_RATE_OFFSET     0x0008c1c0u
#define GU_XP_CYA_0_OFFSET            0x0008c2c0u

/* GA100 per-GPC TPC enable override, cmpunlocker "SM-RECONFIG". The fuse
 * block's STATUS_OPT_TPC_GPC (fuses.c) shows the effect. */
#define GU_SM_RECONFIG_OVR(gpc)       (0x00820a40u + (gpc) * 4u)

/* GA100 ECC and WPR/MMU-lock ranges. */
#define GU_FEAT_ECC_0_OFFSET          0x0082380cu
#define GU_FEAT_ECC_1_OFFSET          0x00823810u
#define GU_FEAT_ECC_2_OFFSET          0x0082382cu
#define GU_FEAT_ECC_READOUT_OFFSET    0x00823814u
#define GU_OPT_GLOBAL_POISON_DIS_OFFSET 0x0082039cu
#define GU_OPT_SUBREVISION_OFFSET     0x0082033cu
#define GU_FEAT_POISON_CONTROL_OFFSET 0x00823808u
#define GU_FBPA_ECC_CONTROL_OFFSET    0x009a0470u
#define GU_FBPA_HBM_CFG0_OFFSET       0x009a1004u
#define GU_DRAM_MODE_4_OFFSET         0x009a033cu
#define GU_FB_L2_ECC_CONFIG_OFFSET    0x009a0084u
#define GU_L2_DATA_CONFIG_OFFSET      0x0017e318u
#define GU_L2_INPUT_QUEUE_CONFIG_OFFSET 0x0017e21cu
#define GU_L2_DATA_PLM_OFFSET         0x0017e3d4u
#define GU_L2_INPUT_QUEUE_PLM_OFFSET  0x0017e3c4u
#define GU_FBPA_STATUS_PLM_OFFSET     0x009a0168u
#define GU_MEMORY_FBP_COUNT_OFFSET   0x00022438u
#define GU_MEMORY_CHANNEL_COUNT_OFFSET 0x0002243cu
#define GU_MEMORY_CHANNELS_PER_FBP_OFFSET 0x00022458u
#define GU_MEMORY_FBP_DISABLED_OFFSET 0x00820d38u
#define GU_MEMORY_CHANNEL_DISABLED_OFFSET 0x00820c14u
#define GU_MEMORY_CHANNEL_STATUS(index) (0x0090020cu + (index) * 0x4000u)
/* GA102 uses positive polarity at the same offset as GA100's POISON_DIS. */
#define GU_OPT_GLOBAL_POISON_EN_OFFSET 0x0082039cu
#define GU_WPR1_ADDR_LO_OFFSET        0x001fa81cu
#define GU_WPR1_ADDR_HI_OFFSET        0x001fa820u
#define GU_WPR_ADDR_LO_04_OFFSET      0x001fa804u
#define GU_WPR_ADDR_HI_08_OFFSET      0x001fa808u
#define GU_WPR_CTL_0C_OFFSET          0x001fa80cu

/* Turing keeps the same FEAT fields at the TU102 register aperture. */
#define GU_TU102_FEAT_PLM_OFFSET      0x00409650u
#define GU_TU102_FEAT_ECC_OFFSET      0x00409658u
#define GU_TU102_FEAT_ECC_READOUT_OFFSET 0x00409660u
#define GU_TU102_MMU_PLM_OFFSET       0x00100cdcu
#define GU_TU102_FEAT_READOUT_OFFSET  0x00409668u
#define GU_TU102_FEAT_SS0_OFFSET      0x00409664u
#define GU_TU102_FEAT_SS1_OFFSET      0x0040966cu
/* SS0 has eight 4-bit fields; SS1 has one. Value 8 selects full rate. */
#define GU_SS0_UNLOCK                 0x88888888u
#define GU_SS1_UNLOCK                 0x8u

#endif
