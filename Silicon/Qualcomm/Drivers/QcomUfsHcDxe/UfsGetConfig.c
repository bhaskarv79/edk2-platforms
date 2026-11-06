/** @file
  UFS controller configuration data retrieval implementation.

  Provides functions to obtain controller-specific configuration including
  base addresses, timing parameters, and platform settings.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include "UfsCore.h"
#include "UfsDtGetConfig.h"

///
/// Configuration source, chosen at build time. When PcdUfsConfigSourceDeviceTree
/// is 0 the device tree code is compiled out: only the PCD base configuration
/// below is available, and the bring-up callbacks cannot be enabled.
///
#define UFS_CONFIG_SOURCE_DEVICE_TREE  FixedPcdGet8 (PcdUfsConfigSourceDeviceTree)

///
/// UFS Controller 0 Hardware Base Addresses
///
#define UFS_0_MEM_BASE    FixedPcdGet64 (PcdUfs0MemBaseAddress)            ///< UFS memory base address
#define UFS_0_HCI_OFFSET  FixedPcdGet32 (PcdUfs0HciOffset)                 ///< HCI register offset from memory base
#define UFS_0_HCI_BASE    (UFS_0_MEM_BASE + UFS_0_HCI_OFFSET)              ///< UFS controller 0 memory base address

///
/// UFS Controller 0 Supported Platform Callback Phases
///
/// Pre HCE Platform Callback Phase Enabled.
#define UFS_0_DEFAULT_PRE_HCE_EN  FixedPcdGetBool (PcdUfs0PreHceCallbackEn)
/// Post Link Up Platform Callback Phase Enabled.
#define UFS_0_DEFAULT_POST_LINK_EN  FixedPcdGetBool (PcdUfs0PostLinkCallbackEn)

///
/// UFS Controller 0 Platform Protocol Behavior Flags
///
/// EDKII_UFS_CARD_REF_CLK_FREQ_ATTRIBUTE value; see UFS_BASE_CONFIG.RefClkFreq (UfsCore.h)
#define UFS_0_DEFAULT_REF_CLK_FREQ  FixedPcdGet8    (PcdUfs0RefClkFreq)
/// Skip HCE re-enable; see UFS_BASE_CONFIG.SkipHceReenable (UfsCore.h)
#define UFS_0_DEFAULT_SKIP_HCE_REENABLE  FixedPcdGetBool (PcdUfs0SkipHceReenable)
/// Skip DME_LINKSTARTUP; see UFS_BASE_CONFIG.SkipLinkStartup (UfsCore.h)
#define UFS_0_DEFAULT_SKIP_LINK_STARTUP  FixedPcdGetBool (PcdUfs0SkipLinkStartup)

///
/// UFS Power management config corresponding to voltage corners
///
STATIC UFS_PM_CONFIG  mUfsPmConfig[] =
{
  {
    UfsVoltageCornerMinSVS,
    UfsHsGearInvalid,
    { 0,   0,  0   }
  },
  {
    UfsVoltageCornerLowSVS,
    UfsHsGear2,
    { 100, 4,  100 }
  },
  {
    UfsVoltageCornerSVS,
    UfsHsGear3,
    { 202, 8,  202 }
  },
  {
    UfsVoltageCornerNominal,
    UfsHsGear5,
    { 403, 16, 403 }
  },
  {
    UfsVoltageCornerTurbo,
    UfsHsGear5,
    { 403, 16, 403 }
  }
};

///
/// Calculate the size of PM config list
///
#define UFS_PM_CONFIG_LIST_SIZE  (sizeof (mUfsPmConfig) / sizeof (UFS_PM_CONFIG))

/**
  Retrieves the power management (PM) configuration for a given voltage corner.

  Looks up the PM configuration table entry matching the requested voltage corner.
  The returned PM configuration provides timing parameters and default HS gear for
  that corner, used in clock control and power mode configuration.

  @param[in]  VoltageCorner   Voltage corner selector (e.g., MinSVS, SVS, Nominal).
  @param[out] PmConfig        On success, receives a pointer to the PM configuration
                              entry within the static table.

  @retval EFI_SUCCESS           Matching PM configuration was found.
  @retval EFI_INVALID_PARAMETER PmConfig is NULL, or VoltageCorner is out of range.
  @retval EFI_NOT_FOUND         No PM configuration exists for the voltage corner.
**/
EFI_STATUS
EFIAPI
UfsGetPmConfig (
  IN  UFS_VOLTAGE_CORNER  VoltageCorner,
  OUT UFS_PM_CONFIG       **PmConfig
  )
{
  UINTN  Loop;

  if (PmConfig == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((VoltageCorner <= UfsVoltageCornerUndefined) ||
      (VoltageCorner >= UfsVoltageCornerMax))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Linear scan of mUfsPmConfig[]. The table has UFS_PM_CONFIG_LIST_SIZE
  // entries (currently 5) and is called once per power mode transition, so
  // O(n) is inconsequential. A binary search or direct index would be a
  // premature optimisation here; keep the scan simple and readable.
  //
  for (Loop = 0; Loop < UFS_PM_CONFIG_LIST_SIZE; Loop++) {
    if (mUfsPmConfig[Loop].VoltageCorner == VoltageCorner) {
      break;
    }
  }

  if (Loop == UFS_PM_CONFIG_LIST_SIZE) {
    return EFI_NOT_FOUND;
  }

  *PmConfig = &mUfsPmConfig[Loop];

  return EFI_SUCCESS;
}

#if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)

/**
  Checks a bring-up configuration read from the device tree.

  @param[in]  ControllerId  The UFS controller identifier, for messages.
  @param[in]  Config        The configuration to check.

  @retval EFI_SUCCESS           The configuration is usable.
  @retval EFI_COMPROMISED_DATA  A field is out of range.
**/
STATIC
EFI_STATUS
UfsValidateConfigData (
  IN UFS_CONTROLLER_ID      ControllerId,
  IN CONST UFS_CONFIG_DATA  *Config
  )
{
  CONST UFS_SPEED_CONFIG    *Speed;
  CONST UFS_TIMEOUT_CONFIG  *Timeout;

  Speed   = &Config->SpeedConfig;
  Timeout = &Config->TimeoutConfig;

  if ((Speed->NumLanes == 0) || (Speed->NumLanes > UFS_MAX_DATA_LANES) ||
      (Speed->Gear == 0) ||
      (Speed->HsEnable && (Speed->Gear > UfsHsGearMax)) ||
      (Speed->HsEnable && (Speed->Rate != UfsRateA) && (Speed->Rate != UfsRateB)) ||
      (Speed->LinkStartupMode >= UfsLinkStartupModeMax))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: invalid SpeedConfig: Gear=%u Rate=%u HsEnable=%u NumLanes=%u LinkStartupMode=%u\n",
      ControllerId,
      Speed->Gear,
      Speed->Rate,
      Speed->HsEnable,
      Speed->NumLanes,
      Speed->LinkStartupMode
      ));
    return EFI_COMPROMISED_DATA;
  }

  if ((Timeout->HcEnablePollTimeout == 0) || (Timeout->HcPcsReadyPollTimeout == 0) ||
      (Timeout->HibernatePollTimeout == 0) || (Timeout->UmpsPollTimeout == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: TimeoutConfig values must be non-zero (us): "
      "HcEnable=%u HcPcsReady=%u Hibernate=%u Umps=%u\n",
      ControllerId,
      Timeout->HcEnablePollTimeout,
      Timeout->HcPcsReadyPollTimeout,
      Timeout->HibernatePollTimeout,
      Timeout->UmpsPollTimeout
      ));
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

#endif

/**
  Retrieves the UFS bring-up configuration for the specified controller.

  See UfsGetConfig.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA    *Config
  )
{
  EFI_STATUS  Status;

  if ((ControllerId >= UfsNumControllers) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

 #if (UFS_CONFIG_SOURCE_DEVICE_TREE == 0)
  DEBUG ((
    DEBUG_ERROR,
    "[UfsHc] Controller[%u]: UFS bring-up needs device tree configuration, but PcdUfsConfigSourceDeviceTree is 0\n",
    ControllerId
    ));
  Status = EFI_UNSUPPORTED;
 #else
  Status = UfsDtGetConfigData (ControllerId, Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: no usable bring-up configuration in the device tree [Status=%r]\n",
      ControllerId,
      Status
      ));
    return Status;
  }

  Status = UfsValidateConfigData (ControllerId, Config);
  if (EFI_ERROR (Status)) {
    UfsDtFreeConfigData (Config);
    return Status;
  }

  Status = UfsGetPmConfig (Config->ResourceConfig.VoltageCorner, &Config->PmConfig);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: no power management settings for ResourceConfig VoltageCorner %u [Status=%r]\n",
      ControllerId,
      Config->ResourceConfig.VoltageCorner,
      Status
      ));
    UfsDtFreeConfigData (Config);
  }

 #endif

  return Status;
}

/**
  Frees the heap allocations UfsGetConfigData() made into Config.

  See UfsGetConfig.h for the contract.

**/
VOID
EFIAPI
UfsFreeConfigData (
  IN OUT UFS_CONFIG_DATA  *Config
  )
{
  if (Config == NULL) {
    return;
  }

 #if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)
  UfsDtFreeConfigData (Config);
 #endif
  Config->PmConfig = NULL;
}

/**
  Retrieves the base config for UFS Driver.

  See UfsGetConfig.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG    *BaseConfig
  )
{
 #if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)
  EFI_STATUS  Status;
 #endif

  if ((ControllerId >= UfsNumControllers) || (BaseConfig == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

 #if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)
  Status = UfsDtGetBaseConfig (ControllerId, BaseConfig);
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  //
  // Only a missing node (or protocol) falls back to the PCDs; a node that is
  // there but malformed is an error, not a reason to run another config.
  //
  if (Status != EFI_NOT_FOUND) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: base config in the device tree is unusable [Status=%r]\n",
      ControllerId,
      Status
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_WARN,
    "[UfsHc] Controller[%u]: no base config in the device tree [Status=%r], using PCDs\n",
    ControllerId,
    Status
    ));
 #endif

  //
  // UfsController0 is the only controller; ControllerId was checked above.
  //
  ASSERT (ControllerId == UfsController0);
  BaseConfig->UfsMemBase       = UFS_0_MEM_BASE;
  BaseConfig->UfsHciBase       = UFS_0_HCI_BASE;
  BaseConfig->PreHceEnable     = UFS_0_DEFAULT_PRE_HCE_EN;
  BaseConfig->PostLinkUpEnable = UFS_0_DEFAULT_POST_LINK_EN;
  BaseConfig->RefClkFreq       = UFS_0_DEFAULT_REF_CLK_FREQ;
  BaseConfig->SkipHceReenable  = UFS_0_DEFAULT_SKIP_HCE_REENABLE;
  BaseConfig->SkipLinkStartup  = UFS_0_DEFAULT_SKIP_LINK_STARTUP;

  if ((BaseConfig->UfsMemBase == 0) || (BaseConfig->UfsHciBase == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: no base address; check PcdUfs%uMemBaseAddress (0x%Lx) and PcdUfs%uHciOffset (0x%x)\n",
      ControllerId,
      ControllerId,
      (UINT64)BaseConfig->UfsMemBase,
      ControllerId,
      (UINT32)(BaseConfig->UfsHciBase - BaseConfig->UfsMemBase)
      ));
    return EFI_NOT_FOUND;
  }

  //
  // The bring-up callbacks need PHY tables, reset and shared-PHY registers,
  // speed settings and timeouts, which only the device tree provides.
  //
  if (BaseConfig->PreHceEnable || BaseConfig->PostLinkUpEnable) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Controller[%u]: PcdUfs%uPreHceCallbackEn/PostLinkCallbackEn need the device tree configuration\n",
      ControllerId,
      ControllerId
      ));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}
