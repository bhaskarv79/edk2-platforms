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

/**
  Checks the UFS device identity and applies de-emphasis settings if required.

  Locates the SCSI Pass-Through protocol on the controller handle, reads the
  device inquiry data to identify vendor and product, looks up the device in
  the de-emphasis vendor list, and applies the appropriate de-emphasis value
  for the active gear.

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
  UINT32                           DeEmphasisValue;

  if (Context == NULL) {
    return;
  }

  ControllerData = (UFS_CONTROLLER_DATA *)Context;
  if (ControllerData->UfsCallbackEventContext == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsCheckAndApplyDeEmphasisSettings: UfsCallbackEventContext is NULL\n"));
    return;
  }

  if ((ControllerData->Config.DeEmphasisVendorList.List == NULL) ||
      (ControllerData->Config.DeEmphasisVendorList.NumVendors == 0))
  {
    return;
  }

  PtContext = (UFS_CALLBACK_EVENT_CONTEXT *)ControllerData->UfsCallbackEventContext;

  if (PtContext->HcDriverInterface == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsCheckAndApplyDeEmphasisSettings: HcDriverInterface is NULL\n"));
    return;
  }

  Status = gBS->HandleProtocol (
                  PtContext->ControllerHandle,
                  &gEfiExtScsiPassThruProtocolGuid,
                  (VOID **)&ScsiPassThru
                  );

  if (EFI_ERROR (Status)) {
    //
    // The event fired (someone installed the protocol), but it wasn't on OUR controller.
    // Do nothing and wait for the next signal.
    //
    DEBUG ((DEBUG_WARN, "[UfsHc] SCSI Pass Thru not available on controller %p\n", PtContext->ControllerHandle));
    goto Exit;
  } else {
    DEBUG ((DEBUG_INFO, "[UfsHc] SCSI Pass Thru located on controller %p\n", PtContext->ControllerHandle));
  }

  ZeroMem (Target, sizeof (Target));

  Status = UfsReadInquiry (ScsiPassThru, Target, UFS_LUN_0, 0, NULL, Vendor, Product, Revision);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc] ReadUfsInquiry Failed [Status:%r]\n", Status));
    goto Exit;
  }

  DeEmphasisValue = DE_EMPHASIS_DISABLE_VAL;

  Status = UfsFindPart (
             &ControllerData->Config.DeEmphasisVendorList,
             Vendor,
             Product,
             &FoundVendorIdx,
             &FoundProductIdx
             );

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "[UfsHc] Controller[%u]: De-Emphasis: no match for Vendor=%a Product=%a\n",
      ControllerData->ControllerId,
      Vendor,
      Product
      ));
  } else {
    DEBUG ((DEBUG_INFO, "[UfsHc] Ufs Part details found in De-Emphasis List: Vendor[%d], Product[%d]\n",
      FoundVendorIdx,
      FoundProductIdx
      ));
    switch (ControllerData->Config.SpeedConfig.Gear) {
      case UfsHsGear5:
        DeEmphasisValue = DE_EMPHASIS_GEAR5_VAL;
        break;
      default:
        DeEmphasisValue = DE_EMPHASIS_DISABLE_VAL;
        break;
    }
  }

  if (DeEmphasisValue != 0) {
    Status = UfsUtpApplyDeEmphasisSettings (
               ControllerData,
               PtContext->HcDriverInterface,
               DeEmphasisValue
               );
  }

Exit:
  if (EFI_ERROR (Status) &&
      (Status != EFI_NOT_FOUND))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHc] De-Emphasis may not have been applied correctly [Status:%r]\n", Status));
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
  Reads UFS device descriptor and writes shared device information to the spare register.

  Locates the UFS Device Config protocol on the controller handle, reads the
  device descriptor to obtain the manufacturer ID and spec version, encodes
  this information, and writes it to the spare register for use by other
  firmware components.

  @param[in] Context  Pointer to the UFS_CONTROLLER_DATA for this controller.

**/
STATIC
VOID
EFIAPI
UfsUpdateSharedInfo (
  IN VOID  *Context
  )
{
  EFI_STATUS                      Status;
  UFS_CONTROLLER_DATA             *ControllerData;
  UFS_CALLBACK_EVENT_CONTEXT      *PtContext;
  EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg;
  UFS_DEV_DESC                    DeviceDesc;
  UINT32                          DescSize;
  UFS_CONFIG_DATA                 *Config;
  EFI_PHYSICAL_ADDRESS            SpareRegister;
  UINT32                          RegValue;
  UINT32                          SpareData;
  UINT16                          SpecVersion;

  if (Context == NULL) {
    return;
  }

  ControllerData = (UFS_CONTROLLER_DATA *)Context;
  if (ControllerData->UfsCallbackEventContext == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsUpdateSharedInfo: UfsCallbackEventContext is NULL\n"));
    return;
  }

  if (ControllerData->SharedInfoUpdated) {
    return;
  }

  PtContext = (UFS_CALLBACK_EVENT_CONTEXT *)ControllerData->UfsCallbackEventContext;

  if (PtContext->HcDriverInterface == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsUpdateSharedInfo: HcDriverInterface is NULL\n"));
    return;
  }

  Status = gBS->HandleProtocol (
                  PtContext->ControllerHandle,
                  &gEfiUfsDeviceConfigProtocolGuid,
                  (VOID **)&UfsDevCfg
                  );

  if (EFI_ERROR (Status)) {
    //
    // The event fired (someone installed the protocol), but it wasn't on OUR controller.
    // Do nothing and wait for the next signal.
    //
    DEBUG ((DEBUG_WARN, "[UfsHc] UFS Device Config protocol not available on controller %p\n", PtContext->ControllerHandle));
    goto Exit;
  } else {
    DEBUG ((DEBUG_INFO, "[UfsHc] UFS Device Config protocol located on controller %p\n", PtContext->ControllerHandle));
  }

  ZeroMem (&DeviceDesc, sizeof (DeviceDesc));
  DescSize = sizeof (DeviceDesc);
  Status   = UfsDevCfg->RwUfsDescriptor (
                          UfsDevCfg,
                          TRUE,
                          UfsDeviceDesc,
                          0,
                          0,
                          (UINT8 *)&DeviceDesc,
                          &DescSize
                          );

  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Config        = &ControllerData->Config;
  SpareRegister = Config->RegisterConfig.SpareRegister;

  if (SpareRegister == 0) {
    goto Exit;
  }

  RegValue = MmioRead32 (SpareRegister);

  SpareData   = ((UINT32)SwapBytes16 (DeviceDesc.ManufacturerId) << 8);
  SpecVersion = SwapBytes16 (DeviceDesc.SpecVersion);

  SpareData |= ((SpecVersion >> 8) & 0xF) << 4 |
               ((SpecVersion & 0xF0) >> 4);

  RegValue |= SpareData;

  MmioWrite32 (SpareRegister, RegValue);

Exit:
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Shared info may not have been updated correctly [Status=%r]\n", Status));
  } else {
    ControllerData->SpareRegisterValue = RegValue;
    ControllerData->SharedInfoUpdated  = TRUE;
  }
}

/**
  Enables write booster flag.

  Locates the UFS Device Config protocol on the controller handle,
  checks device tree configuration and enables write booster if configured.

  @param[in] Context  Pointer to the UFS_CONTROLLER_DATA for this controller.

**/
STATIC
VOID
EFIAPI
UfsEnableWriteBooster (
  IN VOID  *Context
  )
{
  EFI_STATUS                      Status;
  UFS_CONTROLLER_DATA             *ControllerData;
  UFS_CALLBACK_EVENT_CONTEXT      *PtContext;
  EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg;
  UFS_SPEED_CONFIG                *SpeedConfig;
  UINT8                           FlagValue;

  if (Context == NULL) {
    return;
  }

  ControllerData = (UFS_CONTROLLER_DATA *)Context;
  if (ControllerData->UfsCallbackEventContext == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsEnableWriteBooster: UfsCallbackEventContext is NULL\n"));
    return;
  }

  SpeedConfig = &ControllerData->Config.SpeedConfig;

  if (!SpeedConfig->WriteBoosterEnable) {
    return;
  }

  if (ControllerData->WriteBoosterEnabled) {
    return;
  }

  PtContext = (UFS_CALLBACK_EVENT_CONTEXT *)ControllerData->UfsCallbackEventContext;

  if (PtContext->HcDriverInterface == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsEnableWriteBooster: HcDriverInterface is NULL\n"));
    return;
  }

  Status = gBS->HandleProtocol (
                  PtContext->ControllerHandle,
                  &gEfiUfsDeviceConfigProtocolGuid,
                  (VOID **)&UfsDevCfg
                  );

  if (EFI_ERROR (Status)) {
    //
    // The event fired (someone installed the protocol), but it wasn't on OUR controller.
    // Do nothing and wait for the next signal.
    //
    DEBUG ((DEBUG_WARN, "[UfsHc] UFS Device Config protocol not available on controller %p\n", PtContext->ControllerHandle));
    goto Exit;
  } else {
    DEBUG ((DEBUG_INFO, "[UfsHc] UFS Device Config protocol located on controller %p\n", PtContext->ControllerHandle));
  }

  FlagValue = 1;
  Status    = UfsDevCfg->RwUfsFlag (
                           UfsDevCfg,
                           FALSE,
                           UfsFlagWriteBoosterEn,
                           &FlagValue
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc] Failed to enable write booster %r\n", Status));
  }

Exit:
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to enable write booster [Status=%r]\n", Status));
  } else {
    ControllerData->WriteBoosterEnabled = TRUE;
  }
}

/**
  Event notification callback invoked when the UFS Device Config protocol is installed.

  Delegates to UfsUpdateSharedInfo to read the device descriptor and update
  the spare register with shared device information.

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
  UfsUpdateSharedInfo (Context);
  UfsEnableWriteBooster (Context);}

/**
  Performs pre-HCE initialization for the UFS controller.

  This function performs UFS resource setup and PHY initialization, retrieves
  base addresses, configures the MPHY, enables clocks, and brings the host
  controller out of reset in preparation for link startup. It also initializes
  and/or updates the controller context.

  @param[in]      ControllerId       The UFS controller identifier.
  @param[in]      UfsMemBase         Ufs memory base address
  @param[in]      UfsHciBase         Ufs host controller base address
  @param[in,out]  ControllerContext  Pointer to a location that receives/contains
                                     the controller runtime context.
  @param[in]      HcDriverInterface  Pointer to the host controller driver interface
                                     used to communicate with the UFS controller.

  @retval EFI_SUCCESS             Initialization completed successfully.
  @retval EFI_INVALID_PARAMETER   One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES    Memory allocation failed.
  @retval EFI_DEVICE_ERROR        Hardware access failed during initialization.
  @retval EFI_TIMEOUT             Timed out waiting for controller state changes.

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
  EFI_STATUS           Status;
  UFS_CONFIG_DATA      *Config;
  UINT32               RegValue;
  UINTN                Timeout;
  UFS_CONTROLLER_DATA  *ControllerData;
  BOOLEAN              ContextIsNew;

  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;

  Status         = EFI_SUCCESS;
  Config         = NULL;
  RegValue       = 0;
  Timeout        = 0;
  ControllerData = NULL;
  ContextIsNew   = FALSE;
  UfsHc          = NULL;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (ControllerContext == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*ControllerContext == NULL) {
    ControllerData = (UFS_CONTROLLER_DATA *)AllocateZeroPool (sizeof (UFS_CONTROLLER_DATA));
    if (ControllerData == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    ControllerData->ControllerId = ControllerId;
    ContextIsNew                 = TRUE;
  } else {
    ControllerData = *ControllerContext;
  }

  if (ControllerData->ControllerId != ControllerId) {
    return EFI_INVALID_PARAMETER;
  }

  UfsHc = HcDriverInterface->UfsHcProtocol;
  if (UfsHc == NULL) {
    goto CleanupAndExit;
  }

  Config = &ControllerData->Config;

  ControllerData->UfsMemBase = UfsMemBase;
  ControllerData->UfsHciBase = UfsHciBase;

  Status = UfsGetConfigData (ControllerData->ControllerId, Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Get Configuration Failed [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  DEBUG ((DEBUG_INFO, "[UfsHc] Active config:\n"));
  DEBUG ((DEBUG_INFO, "  UfsMemBase=0x%Lx\n", (UINT64)ControllerData->UfsMemBase));
  DEBUG ((DEBUG_INFO, "  UfsHciBase=0x%Lx\n", (UINT64)ControllerData->UfsHciBase));
  DEBUG ((
    DEBUG_INFO,
    "  Gear=%d (%a) Rate=%d (%a) HsEnable=%d NumLanes=%d AdaptEnable=%d\n",
    Config->SpeedConfig.Gear,
    (Config->SpeedConfig.Gear == UfsHsGear1) ? "G1" :
    (Config->SpeedConfig.Gear == UfsHsGear2) ? "G2" :
    (Config->SpeedConfig.Gear == UfsHsGear3) ? "G3" :
    (Config->SpeedConfig.Gear == UfsHsGear4) ? "G4" :
    (Config->SpeedConfig.Gear == UfsHsGear5) ? "G5" : "PWM",
    Config->SpeedConfig.Rate,
    (Config->SpeedConfig.Rate == UfsRateA) ? "A" :
    (Config->SpeedConfig.Rate == UfsRateB) ? "B" : "?",
    Config->SpeedConfig.HsEnable,
    Config->SpeedConfig.NumLanes,
    Config->SpeedConfig.AdaptEnable
    ));
  DEBUG ((
    DEBUG_INFO,
    "  LinkStartupMode=%d (%a) WriteBoosterEnable=%d\n",
    Config->SpeedConfig.LinkStartupMode,
    (Config->SpeedConfig.LinkStartupMode == UfsLinkStartupModeHsLss) ? "HS-LSS" : "LS-LSS",
    Config->SpeedConfig.WriteBoosterEnable
    ));
  DEBUG ((
    DEBUG_INFO,
    "  VoltageCorner=%d (%a) UmpsPollTimeout=%d\n",
    Config->ResourceConfig.VoltageCorner,
    (Config->ResourceConfig.VoltageCorner == UfsVoltageCornerMinSVS)  ? "MinSVS"  :
    (Config->ResourceConfig.VoltageCorner == UfsVoltageCornerLowSVS)  ? "LowSVS"  :
    (Config->ResourceConfig.VoltageCorner == UfsVoltageCornerSVS)     ? "SVS"     :
    (Config->ResourceConfig.VoltageCorner == UfsVoltageCornerNominal) ? "Nominal" :
    (Config->ResourceConfig.VoltageCorner == UfsVoltageCornerTurbo)   ? "Turbo"   : "?",
    Config->TimeoutConfig.UmpsPollTimeout
    ));

  Status = UfsEnableSharedPhy (Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to enable UFS PHY [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  RegValue = MmioRead32 (ControllerData->UfsHciBase + UFS_HC_HW_VER_OFFSET);

  ControllerData->Version = RegValue >> 16;

  if (ControllerData->Version >= UFS_VERSION_3_1) {
    Status = UfsDeviceReset (Config);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to reset device [Status=%r]\n", Status));
      goto CleanupAndExit;
    }
  }

  Status = UfsUtpGetHostCaps (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to get host capabilities [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  //
  // Reset Host Controller
  //
  RegValue = 0;
  MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_ENABLE_OFFSET, RegValue);

  //
  // Issue Ufs Phy soft reset
  //
  RegValue = MmioRead32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET);

  RegValue |= UFS_HC_CFG1_PHY_SOFT_RESET;
  RegValue &= ~UFS_HC_CFG1_DEV_REFCLK_ENABLED;

  MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET, RegValue);

  MicroSecondDelay (1000);

  Status = UfsMphyConfigure (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to configure MPHY [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  RegValue = MmioRead32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET);

  RegValue &= ~UFS_HC_CFG1_PHY_SOFT_RESET;

  MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET, RegValue);

  MicroSecondDelay (1000);

  Status = UfsMphyStart (ControllerData);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to start MPHY SERDES [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  if (ControllerData->Version >= 0x5000) {
    RegValue = UFS_HC_CFG0_QUNIPRO_G4_SEL;
    MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_CFG0_OFFSET, RegValue);
  }

  //
  // Enable reference clock
  //
  RegValue = MmioRead32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET);

  RegValue |= UFS_HC_CFG1_DEV_REFCLK_ENABLED;

  MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_CFG1_OFFSET, RegValue);

  //
  // Enable host controller
  //
  RegValue = UFS_HC_HCE_EN;
  MmioWrite32 (ControllerData->UfsHciBase + UFS_HC_ENABLE_OFFSET, RegValue);

  //
  // Poll for host controller enable
  //
  Timeout = Config->TimeoutConfig.HcEnablePollTimeout;
  do {
    RegValue = MmioRead32 (ControllerData->UfsHciBase + UFS_HC_ENABLE_OFFSET);

    if ((RegValue & UFS_HC_HCE_EN) != 0) {
      break;
    }

    MicroSecondDelay (1);

    if (Timeout-- == 0) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Controller enable timeout.\n"));
      Status = EFI_TIMEOUT;
      goto CleanupAndExit;
    }
  } while (Timeout > 0);

  Status = UfsUtpSetClockControl (
             ControllerData,
             HcDriverInterface
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Set clock control failed."));
    goto CleanupAndExit;
  }

  Status = UfsUtpCheckLinkHibernate (
             ControllerData,
             HcDriverInterface
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Check link hibernate failed."));
    goto CleanupAndExit;
  }

  Status = UfsUtpDisableLlc (
             HcDriverInterface
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Disable LLC failed."));
    goto CleanupAndExit;
  }

  if (*ControllerContext == NULL) {
    *ControllerContext = ControllerData;
  }

  return EFI_SUCCESS;

CleanupAndExit:

  if (ContextIsNew && (ControllerData != NULL)) {
    FreePool (ControllerData);
  }

  *ControllerContext = NULL;

  return Status;
}

/**
  Performs post-link-up initialization for the UFS controller.

  This function is invoked after the UFS link has been successfully established.
  It performs post-link configuration such as tuning UniPro parameters and
  reconfiguration of the UTP layer, registers named event listeners for relevant
  protocols, and updates the controller context with event state required by
  runtime operations.

  @param[in]      ControllerId       The UFS controller identifier.
  @param[in]      UfsHciBase         Ufs host controller base address
  @param[in]      ControllerHandle   EFI handle of the UFS controller instance.
  @param[in,out]  ControllerContext  Pointer to the controller runtime context.
                                     On input, may point to an existing context or NULL.
                                     On output, set to the allocated/updated context.
  @param[in]      HcDriverInterface  Pointer to the host controller driver interface.

  @retval EFI_SUCCESS             Post-link initialization completed successfully.
  @retval EFI_INVALID_PARAMETER   One or more input parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES    Memory/resource allocation failed.
  @retval EFI_DEVICE_ERROR        Hardware/driver error during post-link steps.

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
  EFI_STATUS           Status;
  EFI_STATUS           ExitStatus;
  UFS_CONTROLLER_DATA  *ControllerData;
  BOOLEAN              ContextIsNew;

  Status         = EFI_SUCCESS;
  ExitStatus     = EFI_SUCCESS;
  ControllerData = NULL;
  ContextIsNew   = FALSE;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (ControllerHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (ControllerContext == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // All early-return guards above fire before the ControllerData allocation
  // below, so ContextIsNew is still FALSE on those paths and no memory is
  // leaked. Do not reorder these guards below the allocation.
  //
  if (*ControllerContext == NULL) {
    ControllerData = (UFS_CONTROLLER_DATA *)AllocateZeroPool (sizeof (UFS_CONTROLLER_DATA));
    if (ControllerData == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    ControllerData->ControllerId = ControllerId;
    ControllerData->UfsHciBase   = UfsHciBase;
    ContextIsNew                 = TRUE;
  } else {
    ControllerData = *ControllerContext;
  }

  if (ControllerData->ControllerId != ControllerId) {
    return EFI_INVALID_PARAMETER;
  }

  Status = UfsUtpTuneUniproParams (HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to tune unipro params [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  Status = UfsUtpReconfigure (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Utp Reconfiguration [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  ControllerData->UfsCallbackEventContext = AllocateZeroPool (sizeof (UFS_CALLBACK_EVENT_CONTEXT));
  if (ControllerData->UfsCallbackEventContext == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CleanupAndExit;
  }

  //
  // Fully populate all callback context fields before registering either
  // named event. If a NOTIFY_SIGNAL fires immediately on registration (e.g.,
  // the SCSI PassThru protocol is already installed), the callback must
  // observe a complete, consistent context — not a partially-written one.
  //
  ControllerData->UfsCallbackEventContext->ControllerHandle  = ControllerHandle;
  ControllerData->UfsCallbackEventContext->HcDriverInterface = HcDriverInterface;

  //
  // Register named events only after the context is fully initialized.
  //
  Status = UfsNamedEventListen (
             TPL_CALLBACK,
             UfsPostPassThruInitCallback,
             ControllerData,
             &gEfiExtScsiPassThruProtocolGuid,
             &ControllerData->UfsCallbackEventContext->ScsiPassthruEvent,
             &ControllerData->UfsCallbackEventContext->ScsiPassthruEventReg
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
             &ControllerData->UfsCallbackEventContext->DeviceConfigEvent,
             &ControllerData->UfsCallbackEventContext->DeviceConfigEventReg
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to register for Ufs Device Config [Status=%r]\n", Status));
    goto CleanupAndExit;
  }

  if (*ControllerContext == NULL) {
    *ControllerContext = ControllerData;
  }

  return EFI_SUCCESS;

CleanupAndExit:

  if (ControllerData != NULL) {
    if (ControllerData->UfsCallbackEventContext != NULL) {
      if (ControllerData->UfsCallbackEventContext->ScsiPassthruEvent != NULL) {
        ExitStatus = UfsNamedEventDiscard (
                       &ControllerData->UfsCallbackEventContext->ScsiPassthruEvent,
                       &ControllerData->UfsCallbackEventContext->ScsiPassthruEventReg
                       );
        if (EFI_ERROR (ExitStatus)) {
          DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to un-register for UFS SCSI Pass Thru [Status=%r]\n", ExitStatus));
        }
      }

      if (ControllerData->UfsCallbackEventContext->DeviceConfigEvent != NULL) {
        ExitStatus = UfsNamedEventDiscard (
                       &ControllerData->UfsCallbackEventContext->DeviceConfigEvent,
                       &ControllerData->UfsCallbackEventContext->DeviceConfigEventReg
                       );
        if (EFI_ERROR (ExitStatus)) {
          DEBUG ((DEBUG_ERROR, "[UfsHc] Unable to un-register for UFS Device Config [Status=%r]\n", ExitStatus));
        }
      }

      FreePool (ControllerData->UfsCallbackEventContext);
      ControllerData->UfsCallbackEventContext = NULL;
    }

    if (ContextIsNew) {
      FreePool (ControllerData);
    }
  }

  *ControllerContext = NULL;

  return Status;
}

/**
  Public API wrapper for UfsPreHceInternal.
  See UfsHcLib.h for the full contract.
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
  See UfsHcLib.h for the full contract.
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
