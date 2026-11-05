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
/// UFS Configuration from Device Tree
///
#define UFS_CONFIG_SOURCE_DEVICE_TREE  FixedPcdGet8 (PcdUfsConfigSourceDeviceTree)

///
/// UFS Controller Generic Timeout Value
///
#define UFS_GENERIC_TIMEOUT  100000

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
/// Skip HCE re-enable (XBL already brought up PHY); see UFS_BASE_CONFIG.SkipHceReenable (UfsCore.h)
#define UFS_0_DEFAULT_SKIP_HCE_REENABLE  FixedPcdGetBool (PcdUfs0SkipHceReenable)
/// Skip DME_LINKSTARTUP (XBL already started link); see UFS_BASE_CONFIG.SkipLinkStartup (UfsCore.h)
#define UFS_0_DEFAULT_SKIP_LINK_STARTUP  FixedPcdGetBool (PcdUfs0SkipLinkStartup)

///
/// UFS Controller 0 Default Configuration Parameters
///
#define UFS_0_DEFAULT_GEAR            FixedPcdGet8    (PcdUfs0Gear)               ///< Default High Speed gear 5
#define UFS_0_DEFAULT_RATE            FixedPcdGet8    (PcdUfs0Rate)               ///< Default rate series B
#define UFS_0_DEFAULT_HS_GEAR_ENABLE  FixedPcdGetBool (PcdUfs0HsGearEnable)       ///< Enable High Speed mode
#define UFS_0_DEFAULT_NUM_LANES       FixedPcdGet8    (PcdUfs0Lanes)              ///< Default number of lanes
#define UFS_0_DEFAULT_ADAPT_ENABLE    FixedPcdGetBool (PcdUfs0AdaptEnable)        ///< Enable adaptive configuration
#define UFS_0_DEFAULT_VOLTAGE_CORNER  FixedPcdGet8    (PcdUfs0VoltageCorner)      ///< Default voltage corner
/// UMPS polling timeout in microseconds.
#define UFS_0_DEFAULT_TIMEOUT_US  FixedPcdGet32   (PcdUfs0TimeoutuS)

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

/**
  Retrieves the UFS configuration data for the specified controller.

  This function returns the platform-specific configuration parameters
  required for UFS host controller initialization, including gear settings,
  lane configuration, and vendor-specific parameters. The configuration
  can be sourced from static data or device tree (currently static only).

  @param[in]  ControllerId    The unique identifier of the UFS controller.
  @param[out] Config          Pointer to the UFS_CONFIG_DATA structure to be
                              populated with configuration parameters.

  @retval EFI_SUCCESS         Configuration data retrieved successfully.
  @retval EFI_INVALID_PARAMETER Invalid ControllerId or Config pointer is NULL.
  @retval EFI_UNSUPPORTED     Configuration source not supported.

**/
EFI_STATUS
EFIAPI
UfsGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA    *Config
  )
{
  EFI_STATUS  Status;
  BOOLEAN     SkipStaticConfig;

  Status           = EFI_UNSUPPORTED;
  SkipStaticConfig = FALSE;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

 #if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)

  Status = UfsDtGetConfigData (
             ControllerId,
             Config
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to get config data from device tree [Status=%r]\n", Status));
    DEBUG ((DEBUG_INFO, "[UfsHc] Falling back to static config for config data \n"));
  } else {
    Status           = EFI_SUCCESS;
    SkipStaticConfig = TRUE;
  }

 #endif

  if (!SkipStaticConfig) {
    //
    // Configure based on static controller-specific settings
    //
    switch (ControllerId) {
      case 0:
        Config->SpeedConfig.Gear        = UFS_0_DEFAULT_GEAR;
        Config->SpeedConfig.Rate        = UFS_0_DEFAULT_RATE;
        Config->SpeedConfig.HsEnable    = UFS_0_DEFAULT_HS_GEAR_ENABLE;
        Config->SpeedConfig.NumLanes    = UFS_0_DEFAULT_NUM_LANES;
        Config->SpeedConfig.AdaptEnable = UFS_0_DEFAULT_ADAPT_ENABLE;

        Config->ResourceConfig.VoltageCorner = UFS_0_DEFAULT_VOLTAGE_CORNER;

        Config->TimeoutConfig.HcEnablePollTimeout   = UFS_GENERIC_TIMEOUT;
        Config->TimeoutConfig.HibernatePollTimeout  = UFS_GENERIC_TIMEOUT;
        Config->TimeoutConfig.HcPcsReadyPollTimeout = UFS_GENERIC_TIMEOUT;
        Config->TimeoutConfig.UmpsPollTimeout       = UFS_0_DEFAULT_TIMEOUT_US;
        break;
      default:
        Status = EFI_UNSUPPORTED;
        goto Exit;
    }

    DEBUG ((
      DEBUG_INFO,
      "[UfsHc] Controller[%u]: static timeouts: HcEnable=%u HcPcsReady=%u Hibernate=%u UmPs=%u (all in us)\n",
      ControllerId,
      Config->TimeoutConfig.HcEnablePollTimeout,
      Config->TimeoutConfig.HcPcsReadyPollTimeout,
      Config->TimeoutConfig.HibernatePollTimeout,
      Config->TimeoutConfig.UmpsPollTimeout
      ));
  }

  Status = UfsGetPmConfig (Config->ResourceConfig.VoltageCorner, &Config->PmConfig);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to get PM Config [Status=%r]\n", Status));
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Retrieves the base config for UFS Driver.

  This function returns the physical base addresses for the UFS memory region,
  Host Controller Interface (HCI) registers, Enable callback phases for
  platform callback on specified controller.

  @param[in]  ControllerId      The unique identifier of the UFS controller.
  @param[out] BaseConfig        Pointer to store the UFS base config.

  @retval EFI_SUCCESS         Base addresses retrieved successfully.
  @retval EFI_INVALID_PARAMETER Invalid ControllerId or NULL pointer parameters.
  @retval EFI_NOT_FOUND       Base addresses not found for the specified controller.
  @retval EFI_DEVICE_ERROR    Failed to retrieve base addresses due to hardware error.

**/
EFI_STATUS
EFIAPI
UfsGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG    *BaseConfig
  )
{
  EFI_STATUS  Status;
  BOOLEAN     SkipStaticConfig;

  Status           = EFI_UNSUPPORTED;
  SkipStaticConfig = FALSE;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (BaseConfig == NULL) {
    return EFI_INVALID_PARAMETER;
  }

 #if (UFS_CONFIG_SOURCE_DEVICE_TREE != 0)

  Status = UfsDtGetBaseConfig (
             ControllerId,
             BaseConfig
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to get base config from device tree [Status=%r] \n", Status));
    DEBUG ((DEBUG_INFO, "[UfsHc] Falling back to static config for base config \n"));
  } else {
    Status           = EFI_SUCCESS;
    SkipStaticConfig = TRUE;
  }

 #endif

  if (!SkipStaticConfig) {
    //
    // Return static base addresses based on controller ID
    //
    switch (ControllerId) {
      case 0:
        BaseConfig->UfsMemBase       = UFS_0_MEM_BASE;
        BaseConfig->UfsHciBase       = UFS_0_HCI_BASE;
        BaseConfig->PreHceEnable     = UFS_0_DEFAULT_PRE_HCE_EN;
        BaseConfig->PostLinkUpEnable = UFS_0_DEFAULT_POST_LINK_EN;
        BaseConfig->RefClkFreq       = UFS_0_DEFAULT_REF_CLK_FREQ;
        BaseConfig->SkipHceReenable  = UFS_0_DEFAULT_SKIP_HCE_REENABLE;
        BaseConfig->SkipLinkStartup  = UFS_0_DEFAULT_SKIP_LINK_STARTUP;
        break;
      default:
        Status = EFI_UNSUPPORTED;
        goto Exit;
    }

    if ((BaseConfig->UfsMemBase == 0) || (BaseConfig->UfsHciBase == 0)) {
      Status = EFI_NO_MEDIA;

      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc][Pcd] Invalid base address configuration \n"
        ));

      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc][Pcd] Check PcdUfs%dMemBaseAddress [= 0x%Lx], PcdUfs%dHciOffset [= 0x%Lx]\n",
        ControllerId,
        (UINT64)BaseConfig->UfsMemBase,
        ControllerId,
        (UINT64)BaseConfig->UfsHciBase - (UINT64)BaseConfig->UfsMemBase
        ));

      goto Exit;
    }
  }

  Status = EFI_SUCCESS;

Exit:
  return Status;
}
