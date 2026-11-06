/** @file
  UFS core initialization and callback handler implementation.

  Implements pre-HCE and post-link-up initialization sequences, including
  PHY configuration, host controller enable, and protocol event registration.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage
  - HCE - Host Controller Enable
  - PHY - Physical Layer

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>
#include <IndustryStandard/UfsHci.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UfsCore.h"
#include "UfsGetConfig.h"
#include "UfsPhy.h"
#include "UfsUtils.h"
#include "UfsUtpCore.h"

///
/// Settling time after asserting and after releasing the UFS PHY soft reset,
/// in microseconds. Carried over from the original bring-up sequence; not
/// derived from a specification minimum.
///
#define UFS_PHY_SOFT_RESET_DELAY_US  1000

//
// Spare register layout (UFS_REGISTER_CONFIG.SpareRegister). It is shared with
// other boot stages: this driver only ORs in the fields below, once per boot,
// after EFI_UFS_DEVICE_CONFIG_PROTOCOL appears, and leaves the other bits to
// their owners. The consumer is a later boot stage outside this tree.
//   [23:8]  JEDEC manufacturer ID (Device Descriptor wManufacturerID)
//   [7:4]   UFS specification major version (wSpecVersion bits [11:8])
//   [3:0]   UFS specification minor version (wSpecVersion bits [7:4])
//
#define UFS_SPARE_MANUFACTURER_ID_SHIFT  8
#define UFS_SPARE_SPEC_MAJOR_SHIFT       4
#define UFS_SPARE_SPEC_MAJOR_MASK        0xF0
#define UFS_SPARE_SPEC_MINOR_MASK        0x0F

/**
  Returns a printable name for a gear.

  @param[in]  Gear      The gear.
  @param[in]  HsEnable  TRUE for an HS gear, FALSE for a PWM gear.

  @return The name.
**/
STATIC
CONST CHAR8 *
UfsGearToString (
  IN UFS_GEAR  Gear,
  IN BOOLEAN   HsEnable
  )
{
  STATIC CONST CHAR8  *CONST  HsGearNames[] = { "Invalid", "HS-G1", "HS-G2", "HS-G3", "HS-G4", "HS-G5" };

  if (!HsEnable) {
    return (Gear == 0) ? "Invalid" : "PWM";
  }

  return (Gear < ARRAY_SIZE (HsGearNames)) ? HsGearNames[Gear] : "Invalid";
}

/**
  Returns a printable name for a voltage corner.

  @param[in]  VoltageCorner  The voltage corner.

  @return The name.
**/
STATIC
CONST CHAR8 *
UfsVoltageCornerToString (
  IN UFS_VOLTAGE_CORNER  VoltageCorner
  )
{
  STATIC CONST CHAR8  *CONST  Names[] = { "Invalid", "MinSVS", "LowSVS", "SVS", "Nominal", "Turbo" };

  return (VoltageCorner < ARRAY_SIZE (Names)) ? Names[VoltageCorner] : "Invalid";
}

/**
  Logs the configuration PreHce is about to apply.

  @param[in]  ControllerData  The controller.
**/
STATIC
VOID
UfsLogActiveConfig (
  IN CONST UFS_CONTROLLER_DATA  *ControllerData
  )
{
  CONST UFS_CONFIG_DATA  *Config;

  Config = &ControllerData->Config;

  DEBUG ((
    DEBUG_INFO,
    "[UfsHc] Controller[%u]: MemBase 0x%Lx HciBase 0x%Lx\n",
    ControllerData->ControllerId,
    (UINT64)ControllerData->UfsMemBase,
    (UINT64)ControllerData->UfsHciBase
    ));
  DEBUG ((
    DEBUG_INFO,
    "[UfsHc]   %a rate %a, %u lanes, adapt %u, %a link startup, WriteBooster %u\n",
    UfsGearToString (Config->SpeedConfig.Gear, Config->SpeedConfig.HsEnable),
    (Config->SpeedConfig.Rate == UfsRateA) ? "A" : ((Config->SpeedConfig.Rate == UfsRateB) ? "B" : "Invalid"),
    Config->SpeedConfig.NumLanes,
    Config->SpeedConfig.AdaptEnable,
    (Config->SpeedConfig.LinkStartupMode == UfsLinkStartupModeHsLss) ? "HS-LSS" : "LS-LSS",
    Config->SpeedConfig.WriteBoosterEnable
    ));
  DEBUG ((
    DEBUG_INFO,
    "[UfsHc]   voltage corner %a, UMPS timeout %u us\n",
    UfsVoltageCornerToString (Config->ResourceConfig.VoltageCorner),
    Config->TimeoutConfig.UmpsPollTimeout
    ));
}

/**
  Loads and validates the controller's configuration, once.

  The reset, shared PHY and spare registers of the configuration are outside
  the HCI and PHY windows; each one in use is described in the GCD as
  uncached MMIO here, before the bring-up accesses it.

  @param[in,out]  ControllerData  The controller.
  @param[in]      Reload          TRUE to re-read it even if already loaded.

  @retval EFI_SUCCESS  The configuration is loaded.
  @retval other        Status from UfsGetConfigData() or UfsRegisterMmioRange().
**/
STATIC
EFI_STATUS
UfsLoadConfig (
  IN OUT UFS_CONTROLLER_DATA  *ControllerData,
  IN     BOOLEAN              Reload
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Registers[3];
  UINTN                 Index;

  if (ControllerData->ConfigLoaded && !Reload) {
    return EFI_SUCCESS;
  }

  //
  // Release any tables from an earlier load before they are overwritten.
  //
  UfsFreeConfigData (&ControllerData->Config);
  ControllerData->ConfigLoaded = FALSE;
  Status                       = UfsGetConfigData (ControllerData->ControllerId, &ControllerData->Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Registers[0] = ControllerData->Config.RegisterConfig.ResetRegister;
  Registers[1] = ControllerData->Config.RegisterConfig.SharedPhyRegister;
  Registers[2] = ControllerData->Config.RegisterConfig.SpareRegister;
  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    if (Registers[Index] == 0) {
      continue;
    }

    Status = UfsRegisterMmioRange (Registers[Index] & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK, EFI_PAGE_SIZE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to map register 0x%Lx [Status=%r]\n", Registers[Index], Status));
      UfsFreeConfigData (&ControllerData->Config);
      return Status;
    }
  }

  ControllerData->ConfigLoaded = TRUE;
  return EFI_SUCCESS;
}

/**
  Frees a controller context allocated by UfsPreHce() or UfsPostLinkUp().

  @param[in]  ControllerData  The context to free.
**/
STATIC
VOID
UfsFreeControllerData (
  IN UFS_CONTROLLER_DATA  *ControllerData
  )
{
  UfsFreeConfigData (&ControllerData->Config);
  FreePool (ControllerData);
}

/**
  Checks the UFS device identity and applies de-emphasis settings if required.

  Locates the SCSI Pass-Through protocol on the controller handle, reads the
  device inquiry data to identify vendor and product, looks up the device in
  the de-emphasis vendor list, and, for a listed device running at HS-G5,
  programs the G5 de-emphasis value. Any other device keeps the de-emphasis
  setting it already has.

  @param[in] Context  Pointer to the UFS_CONTROLLER_DATA for this controller.

**/
STATIC
VOID
EFIAPI
UfsCheckAndApplyDeEmphasisSettings (
  IN VOID  *Context
  )
{
  EFI_STATUS                       Status;
  UFS_CONTROLLER_DATA              *ControllerData;
  UFS_CALLBACK_EVENT_CONTEXT       *PtContext;
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *ScsiPassThru;
  UINT8                            Target[TARGET_MAX_BYTES];
  CHAR8                            Vendor[UFS_INQUIRY_VENDOR_ID_BYTES + 1];
  CHAR8                            Product[UFS_INQUIRY_PRODUCT_ID_BYTES + 1];
  CHAR8                            Revision[UFS_INQUIRY_REV_LEVEL_BYTES + 1];
  UINTN                            FoundVendorIdx;
  UINTN                            FoundProductIdx;

  if (Context == NULL) {
    return;
  }

  ControllerData = (UFS_CONTROLLER_DATA *)Context;
  PtContext      = ControllerData->UfsCallbackEventContext;
  if ((PtContext == NULL) || (PtContext->HcDriverInterface == NULL)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsCheckAndApplyDeEmphasisSettings: callback context incomplete\n"));
    return;
  }

  if ((ControllerData->Config.DeEmphasisVendorList.List == NULL) ||
      (ControllerData->Config.DeEmphasisVendorList.NumVendors == 0))
  {
    return;
  }

  //
  // The event is signalled for every EFI_EXT_SCSI_PASS_THRU_PROTOCOL installed
  // anywhere in the system. Only act on the one on this controller.
  //
  Status = gBS->HandleProtocol (
                  PtContext->ControllerHandle,
                  &gEfiExtScsiPassThruProtocolGuid,
                  (VOID **)&ScsiPassThru
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_VERBOSE, "[UfsHc] SCSI Pass Thru not yet on controller %p\n", PtContext->ControllerHandle));
    return;
  }

  ZeroMem (Target, sizeof (Target));

  Status = UfsReadInquiry (ScsiPassThru, Target, UFS_LUN_0, 0, NULL, Vendor, Product, Revision);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc] INQUIRY failed, de-emphasis left unchanged [Status=%r]\n", Status));
    return;
  }

  Status = UfsFindPart (
             &ControllerData->Config.DeEmphasisVendorList,
             Vendor,
             Product,
             &FoundVendorIdx,
             &FoundProductIdx
             );
  if (EFI_ERROR (Status) || (ControllerData->Config.SpeedConfig.Gear != UfsHsGear5)) {
    DEBUG ((
      DEBUG_INFO,
      "[UfsHc] Controller[%u]: de-emphasis left unchanged for %a %a at gear %u\n",
      ControllerData->ControllerId,
      Vendor,
      Product,
      ControllerData->Config.SpeedConfig.Gear
      ));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "[UfsHc] Controller[%u]: %a %a is in the de-emphasis list (vendor %u, product %u)\n",
    ControllerData->ControllerId,
    Vendor,
    Product,
    (UINT32)FoundVendorIdx,
    (UINT32)FoundProductIdx
    ));

  Status = UfsUtpApplyDeEmphasisSettings (ControllerData, PtContext->HcDriverInterface, DE_EMPHASIS_GEAR5_VAL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to apply de-emphasis [Status=%r]\n", Status));
  }
}

/**
  Event notification callback invoked when the SCSI Pass-Through protocol is installed.

  Delegates to UfsCheckAndApplyDeEmphasisSettings to identify the device and
  apply de-emphasis settings if the device is in the vendor list.

  @param[in] Event    The EFI event that was signaled.
  @param[in] Context  Pointer to the UFS_CONTROLLER_DATA for this controller.

**/
STATIC
VOID
EFIAPI
UfsPostPassThruInitCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UfsCheckAndApplyDeEmphasisSettings (Context);
}

/**
  Publishes the device's manufacturer ID and specification version in the
  spare register (see the layout at the top of this file).

  @param[in] ControllerData  The controller.
  @param[in] UfsDevCfg       EFI_UFS_DEVICE_CONFIG_PROTOCOL of this controller.

**/
STATIC
VOID
UfsUpdateSharedInfo (
  IN UFS_CONTROLLER_DATA             *ControllerData,
  IN EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg
  )
{
  EFI_STATUS            Status;
  UFS_DEV_DESC          DeviceDesc;
  UINT32                DescSize;
  EFI_PHYSICAL_ADDRESS  SpareRegister;
  UINT32                SpareData;
  UINT16                SpecVersion;

  if (ControllerData->SharedInfoUpdated) {
    return;
  }

  SpareRegister = ControllerData->Config.RegisterConfig.SpareRegister;
  if (SpareRegister == 0) {
    //
    // No spare register on this SoC: nothing to publish.
    //
    ControllerData->SharedInfoUpdated = TRUE;
    return;
  }

  ZeroMem (&DeviceDesc, sizeof (DeviceDesc));
  DescSize = sizeof (DeviceDesc);
  Status   = UfsDevCfg->RwUfsDescriptor (UfsDevCfg, TRUE, UfsDeviceDesc, 0, 0, (UINT8 *)&DeviceDesc, &DescSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc] Device descriptor read failed, device info not published [Status=%r]\n", Status));
    return;
  }

  //
  // A device may return a shorter descriptor; only use fields it covered.
  //
  if (DescSize < MAX (
                   OFFSET_OF (UFS_DEV_DESC, ManufacturerId) + sizeof (DeviceDesc.ManufacturerId),
                   OFFSET_OF (UFS_DEV_DESC, SpecVersion) + sizeof (DeviceDesc.SpecVersion)
                   ))
  {
    DEBUG ((DEBUG_WARN, "[UfsHc] Device descriptor too short (%u bytes), device info not published\n", DescSize));
    return;
  }

  SpecVersion = SwapBytes16 (DeviceDesc.SpecVersion);
  SpareData   = ((UINT32)SwapBytes16 (DeviceDesc.ManufacturerId) << UFS_SPARE_MANUFACTURER_ID_SHIFT) |
                (((SpecVersion >> 8) << UFS_SPARE_SPEC_MAJOR_SHIFT) & UFS_SPARE_SPEC_MAJOR_MASK) |
                ((SpecVersion >> 4) & UFS_SPARE_SPEC_MINOR_MASK);

  MmioOr32 (SpareRegister, SpareData);
  ControllerData->SharedInfoUpdated = TRUE;
}

/**
  Enables the device's WriteBooster if the configuration asks for it.

  @param[in] ControllerData  The controller.
  @param[in] UfsDevCfg       EFI_UFS_DEVICE_CONFIG_PROTOCOL of this controller.

**/
STATIC
VOID
UfsEnableWriteBooster (
  IN UFS_CONTROLLER_DATA             *ControllerData,
  IN EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg
  )
{
  EFI_STATUS  Status;
  UINT8       FlagValue;

  if (!ControllerData->Config.SpeedConfig.WriteBoosterEnable || ControllerData->WriteBoosterEnabled) {
    return;
  }

  FlagValue = 1;
  Status    = UfsDevCfg->RwUfsFlag (UfsDevCfg, FALSE, UfsFlagWriteBoosterEn, &FlagValue);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc] Failed to enable WriteBooster [Status=%r]\n", Status));
    return;
  }

  ControllerData->WriteBoosterEnabled = TRUE;
}

/**
  Event notification callback invoked when the UFS Device Config protocol is installed.

  Publishes device info in the spare register and enables WriteBooster.

  @param[in] Event    The EFI event that was signaled.
  @param[in] Context  Pointer to the UFS_CONTROLLER_DATA for this controller.

**/
STATIC
VOID
EFIAPI
UfsDeviceConfigProtocolCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                      Status;
  UFS_CONTROLLER_DATA             *ControllerData;
  EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg;

  if (Context == NULL) {
    return;
  }

  ControllerData = (UFS_CONTROLLER_DATA *)Context;
  if (ControllerData->UfsCallbackEventContext == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsDeviceConfigProtocolCallback: callback context missing\n"));
    return;
  }

  //
  // The event is signalled for every EFI_UFS_DEVICE_CONFIG_PROTOCOL installed
  // anywhere in the system. Only act on the one on this controller.
  //
  Status = gBS->HandleProtocol (
                  ControllerData->UfsCallbackEventContext->ControllerHandle,
                  &gEfiUfsDeviceConfigProtocolGuid,
                  (VOID **)&UfsDevCfg
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_VERBOSE,
      "[UfsHc] UFS Device Config not yet on controller %p\n",
      ControllerData->UfsCallbackEventContext->ControllerHandle
      ));
    return;
  }

  UfsUpdateSharedInfo (ControllerData, UfsDevCfg);
  UfsEnableWriteBooster (ControllerData, UfsDevCfg);
}

/**
  Reads the host controller version and checks the read looks sane.

  @param[in,out]  ControllerData  The controller; Version is set.
  @param[in]      UfsHc           The UFS host controller protocol.

  @retval EFI_SUCCESS       The version was read.
  @retval EFI_DEVICE_ERROR  The register read back as 0 or all ones, which
                            means a wrong UfsHciBase or an unclocked controller.
  @retval other             The register read failed.
**/
STATIC
EFI_STATUS
UfsPreHceReadVersion (
  IN OUT UFS_CONTROLLER_DATA                 *ControllerData,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc
  )
{
  EFI_STATUS  Status;
  UINT32      RegValue;

  Status = UfsHciRead32 (UfsHc, UFS_HC_HW_VER_OFFSET, &RegValue);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "[UfsHc] HW version register 0x%08x\n", RegValue));

  if ((RegValue == 0) || (RegValue == MAX_UINT32)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Implausible HW version 0x%08x; check UfsHciBase and controller clocks\n", RegValue));
    return EFI_DEVICE_ERROR;
  }

  ControllerData->Version = RegValue >> 16;
  return EFI_SUCCESS;
}

/**
  Resets the host controller and programs and starts the PHY.

  Disables HCE, holds the PHY in soft reset while the PHY tables are written,
  releases it and starts the MPHY. The PHY register space the tables use is
  added to the GCD as uncached MMIO first.

  @param[in,out]  ControllerData  The controller.
  @param[in]      UfsHc           The UFS host controller protocol.

  @retval EFI_SUCCESS  The PHY is up.
  @retval other        A register access, the GCD mapping, PHY programming
                       or the MPHY start failed.
**/
STATIC
EFI_STATUS
UfsPreHcePhyReset (
  IN OUT UFS_CONTROLLER_DATA                 *ControllerData,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc
  )
{
  EFI_STATUS            Status;
  UINT32                RegValue;
  EFI_PHYSICAL_ADDRESS  WindowBase;
  UINT64                WindowSize;
  UINT64                WindowEnd;

  //
  // The PHY tables and the MPHY start/PCS-ready registers this sequence uses.
  //
  WindowSize = MAX (
                 ControllerData->Config.PhyConfig.RegisterWindowSize,
                 UFS_MEM_MPHY_PCS_READY_OFFSET + sizeof (UINT32)
                 );
  WindowBase = ControllerData->UfsMemBase & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK;
  WindowEnd  = ControllerData->UfsMemBase + WindowSize;
  Status     = UfsRegisterMmioRange (WindowBase, ALIGN_VALUE (WindowEnd - WindowBase, EFI_PAGE_SIZE));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to map the PHY registers at 0x%Lx [Status=%r]\n", WindowBase, Status));
    return Status;
  }

  Status = UfsHciWrite32 (UfsHc, UFS_HC_ENABLE_OFFSET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsHciRead32 (UfsHc, UFS_HC_CFG1_OFFSET, &RegValue);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RegValue |= UFS_HC_CFG1_PHY_SOFT_RESET;
  RegValue &= ~UFS_HC_CFG1_DEV_REFCLK_ENABLED;
  Status    = UfsHciWrite32 (UfsHc, UFS_HC_CFG1_OFFSET, RegValue);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (UFS_PHY_SOFT_RESET_DELAY_US);

  Status = UfsMphyConfigure (ControllerData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to configure MPHY [Status=%r]\n", Status));
    return Status;
  }

  RegValue &= ~UFS_HC_CFG1_PHY_SOFT_RESET;
  Status    = UfsHciWrite32 (UfsHc, UFS_HC_CFG1_OFFSET, RegValue);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (UFS_PHY_SOFT_RESET_DELAY_US);

  Status = UfsMphyStart (ControllerData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to start MPHY SERDES [Status=%r]\n", Status));
  }

  return Status;
}

/**
  Enables the device reference clock and the host controller.

  @param[in]  ControllerData  The controller.
  @param[in]  UfsHc           The UFS host controller protocol.

  @retval EFI_SUCCESS  HCE is set.
  @retval EFI_TIMEOUT  HCE did not become set within HcEnablePollTimeout.
  @retval other        A register access failed.
**/
STATIC
EFI_STATUS
UfsPreHceEnableController (
  IN UFS_CONTROLLER_DATA                 *ControllerData,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc
  )
{
  EFI_STATUS  Status;
  UINT32      RegValue;

  if (ControllerData->Version >= UFS_VERSION_5_0) {
    Status = UfsHciWrite32 (UfsHc, UFS_HC_CFG0_OFFSET, UFS_HC_CFG0_QUNIPRO_G4_SEL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = UfsHciRead32 (UfsHc, UFS_HC_CFG1_OFFSET, &RegValue);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsHciWrite32 (UfsHc, UFS_HC_CFG1_OFFSET, RegValue | UFS_HC_CFG1_DEV_REFCLK_ENABLED);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsHciWrite32 (UfsHc, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsHciPoll32 (
             UfsHc,
             UFS_HC_ENABLE_OFFSET,
             UFS_HC_HCE_EN,
             UFS_HC_HCE_EN,
             ControllerData->Config.TimeoutConfig.HcEnablePollTimeout,
             NULL
             );
  if (Status == EFI_TIMEOUT) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Controller enable timeout\n"));
  }

  return Status;
}

/**
  Performs pre-HCE initialization for the UFS controller.

  See UfsCore.h for the contract. The sequence assumes UEFI owns UFS bring-up:
  it resets the device, the PHY and the host controller unconditionally, so
  it must only be enabled (PreHceEnable) on platforms where the previous boot
  stage leaves UFS uninitialized.

**/
STATIC
EFI_STATUS
EFIAPI
UfsPreHceInternal (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsMemBase,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                          Status;
  UFS_CONTROLLER_DATA                 *ControllerData;
  BOOLEAN                             ContextIsNew;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;

  if ((ControllerId >= UfsNumControllers) || (ControllerContext == NULL) || (HcDriverInterface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*ControllerContext != NULL) &&
      (((UFS_CONTROLLER_DATA *)*ControllerContext)->ControllerId != ControllerId))
  {
    return EFI_INVALID_PARAMETER;
  }

  UfsHc = HcDriverInterface->UfsHcProtocol;
  if (UfsHc == NULL) {
    return EFI_NOT_STARTED;
  }

  ContextIsNew = (BOOLEAN)(*ControllerContext == NULL);
  if (ContextIsNew) {
    ControllerData = AllocateZeroPool (sizeof (UFS_CONTROLLER_DATA));
    if (ControllerData == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    ControllerData->ControllerId = ControllerId;
  } else {
    ControllerData = *ControllerContext;
  }

  ControllerData->UefiBringUp = FALSE;
  ControllerData->UfsMemBase  = UfsMemBase;
  ControllerData->UfsHciBase  = UfsHciBase;

  Status = UfsLoadConfig (ControllerData, TRUE);
  if (EFI_ERROR (Status)) {
    goto CleanupAndExit;
  }

  UfsLogActiveConfig (ControllerData);

  Status = UfsEnableSharedPhy (&ControllerData->Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to enable UFS PHY [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsPreHceReadVersion (ControllerData, UfsHc);
  if (EFI_ERROR (Status)) {
    goto CleanupAndExit;
  }

  if (ControllerData->Version >= UFS_VERSION_3_1) {
    Status = UfsDeviceReset (&ControllerData->Config);
    if (EFI_ERROR (Status)) {
      goto CleanupAndExit;
    }
  }

  Status = UfsPreHcePhyReset (ControllerData, UfsHc);
  if (EFI_ERROR (Status)) {
    goto CleanupAndExit;
  }

  Status = UfsPreHceEnableController (ControllerData, UfsHc);
  if (EFI_ERROR (Status)) {
    goto CleanupAndExit;
  }

  //
  // UIC commands need the host controller enabled (UFSHCI: HCS.UCRDY is set
  // only after HCE), so the capabilities are read only now.
  //
  Status = UfsUtpGetHostCaps (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to get host capabilities [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsUtpSetClockControl (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Set clock control failed [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsUtpCheckLinkHibernate (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Check link hibernate failed [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsUtpDisableLlc (HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Disable LLC failed [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  ControllerData->UefiBringUp = TRUE;
  *ControllerContext          = ControllerData;
  return EFI_SUCCESS;

CleanupAndExit:
  //
  // A context this call allocated is freed; an existing one stays with the
  // caller, who still owns it.
  //
  if (ContextIsNew) {
    UfsFreeControllerData (ControllerData);
  }

  return Status;
}

/**
  Performs post-link-up initialization for the UFS controller.

  See UfsCore.h for the contract.

**/
STATIC
EFI_STATUS
EFIAPI
UfsPostLinkUpInternal (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN     EFI_HANDLE                     ControllerHandle,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                  Status;
  EFI_STATUS                  ExitStatus;
  UFS_CONTROLLER_DATA         *ControllerData;
  UFS_CALLBACK_EVENT_CONTEXT  *EventContext;
  BOOLEAN                     ContextIsNew;

  if ((ControllerId >= UfsNumControllers) || (ControllerHandle == NULL) ||
      (ControllerContext == NULL) || (HcDriverInterface == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((*ControllerContext != NULL) &&
      (((UFS_CONTROLLER_DATA *)*ControllerContext)->ControllerId != ControllerId))
  {
    return EFI_INVALID_PARAMETER;
  }

  ContextIsNew = (BOOLEAN)(*ControllerContext == NULL);
  if (ContextIsNew) {
    ControllerData = AllocateZeroPool (sizeof (UFS_CONTROLLER_DATA));
    if (ControllerData == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    ControllerData->ControllerId = ControllerId;
    ControllerData->UfsHciBase   = UfsHciBase;
  } else {
    ControllerData = *ControllerContext;
  }

  Status = UfsLoadConfig (ControllerData, FALSE);
  if (EFI_ERROR (Status)) {
    goto CleanupAndExit;
  }

  Status = UfsUtpTuneUniproParams (HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to tune unipro params [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsUtpReconfigure (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UTP reconfiguration failed [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  if (ControllerData->UfsCallbackEventContext != NULL) {
    //
    // The events are already registered from an earlier link up.
    //
    *ControllerContext = ControllerData;
    return EFI_SUCCESS;
  }

  //
  // Fully populate the callback context before registering either named
  // event: if the protocol is already installed, the callback can run as
  // soon as it is registered and must see a complete context.
  //
  EventContext = AllocateZeroPool (sizeof (UFS_CALLBACK_EVENT_CONTEXT));
  if (EventContext == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CleanupAndExit;
  }

  EventContext->ControllerHandle          = ControllerHandle;
  EventContext->HcDriverInterface         = HcDriverInterface;
  ControllerData->UfsCallbackEventContext = EventContext;

  Status = UfsNamedEventListen (
             TPL_CALLBACK,
             UfsPostPassThruInitCallback,
             ControllerData,
             &gEfiExtScsiPassThruProtocolGuid,
             &EventContext->ScsiPassthruEvent,
             &EventContext->ScsiPassthruEventReg
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to register for Scsi Passthru [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsNamedEventListen (
             TPL_CALLBACK,
             UfsDeviceConfigProtocolCallback,
             ControllerData,
             &gEfiUfsDeviceConfigProtocolGuid,
             &EventContext->DeviceConfigEvent,
             &EventContext->DeviceConfigEventReg
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to register for Ufs Device Config [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  *ControllerContext = ControllerData;
  return EFI_SUCCESS;

CleanupAndExit:
  EventContext = ControllerData->UfsCallbackEventContext;
  if (EventContext != NULL) {
    if (EventContext->ScsiPassthruEvent != NULL) {
      ExitStatus = UfsNamedEventDiscard (&EventContext->ScsiPassthruEvent, &EventContext->ScsiPassthruEventReg);
      if (EFI_ERROR (ExitStatus)) {
        DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to un-register for UFS SCSI Pass Thru [Status=%r]\n", ExitStatus));
      }
    }

    if (EventContext->DeviceConfigEvent != NULL) {
      ExitStatus = UfsNamedEventDiscard (&EventContext->DeviceConfigEvent, &EventContext->DeviceConfigEventReg);
      if (EFI_ERROR (ExitStatus)) {
        DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to un-register for UFS Device Config [Status=%r]\n", ExitStatus));
      }
    }

    FreePool (EventContext);
    ControllerData->UfsCallbackEventContext = NULL;
  }

  //
  // A context this call allocated is freed; an existing one stays with the
  // caller, who still owns it.
  //
  if (ContextIsNew) {
    UfsFreeControllerData (ControllerData);
  }

  return Status;
}

/**
  Public API wrapper for UfsPreHceInternal.
  See UfsCore.h for the full contract.
**/
EFI_STATUS
EFIAPI
UfsPreHce (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsMemBase,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  return UfsPreHceInternal (ControllerId, UfsMemBase, UfsHciBase, ControllerContext, HcDriverInterface);
}

/**
  Public API wrapper for UfsPostLinkUpInternal.
  See UfsCore.h for the full contract.
**/
EFI_STATUS
EFIAPI
UfsPostLinkUp (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN     EFI_HANDLE                     ControllerHandle,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  return UfsPostLinkUpInternal (ControllerId, UfsHciBase, ControllerHandle, ControllerContext, HcDriverInterface);
}
