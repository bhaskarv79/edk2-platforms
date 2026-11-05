/** @file
  UFS PHY configuration and initialization implementation.

  Provides MPHY configuration, SERDES startup, and PHY-specific register
  programming for Qualcomm UFS host controllers.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS    - Universal Flash Storage
  - PHY    - Physical Layer
  - MPHY   - M-PHY (MIPI physical layer specification)
  - SERDES - Serializer/Deserializer

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>
#include <IndustryStandard/UfsHci.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include "UfsCore.h"
#include "UfsPhy.h"
#include "UfsUtpCore.h"
#include "UfsUtils.h"

/**
  Programs UFS PHY configuration tables.

  Applies the selected PHY table entries based on the active configuration
  (rate and gear). Conditional entries are evaluated and written to the UFS
  PHY MMIO space.

  @param[in]  ControllerData  Pointer to controller data containing MMIO base and config.
  @param[in]  PhyTableType    Table selector (Init, Lane0, Lane1, End).

  @retval EFI_SUCCESS           PHY programming completed successfully.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsPhyControl (
  IN UFS_CONTROLLER_DATA  *ControllerData,
  IN UFS_PHY_TABLE_TYPE   PhyTableType
  )
{
  EFI_STATUS        Status;
  UFS_PHY_TABLE     *PhyTable;
  UINT32            NumEntries;
  UINT32            Loop;
  UINT16            Flags;
  BOOLEAN           LowGearConfig;
  BOOLEAN           HighGearConfig;
  BOOLEAN           RateB;
  UFS_SPEED_CONFIG  *SpeedConfig;
  UFS_PHY_CONFIG    *PhyConfig;
  UINT32            Value;

  Status         = EFI_SUCCESS;
  PhyTable       = NULL;
  NumEntries     = 0;
  LowGearConfig  = FALSE;
  HighGearConfig = FALSE;
  RateB          = FALSE;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (PhyTableType >= UfsPhyTableTypeMax) {
    return EFI_INVALID_PARAMETER;
  }

  SpeedConfig = &ControllerData->Config.SpeedConfig;
  PhyConfig   = &ControllerData->Config.PhyConfig;

  RateB = (SpeedConfig->Rate == UfsRateB);
  if (SpeedConfig->HsEnable) {
    LowGearConfig  = (SpeedConfig->Gear <= UfsHsGear4);
    HighGearConfig = (SpeedConfig->Gear >= UfsHsGear5);
  }

  PhyTable   = PhyConfig->PhyTable[PhyTableType].Entries;
  NumEntries = PhyConfig->PhyTable[PhyTableType].NumEntries;

  for (Loop = 0; Loop < NumEntries; Loop++) {
    Flags = PhyTable[Loop].Flags;
    if (Flags == UFS_PHY_CONFIG_STOP_CONDITION) {
      break;
    }

    if (Flags != 0) {
      if (((Flags & UFS_RATE_MASK) >> UFS_RATE_SHIFT) == UfsRateA) {
        if (RateB) {
          continue;
        }
      } else if (((Flags & UFS_RATE_MASK) >> UFS_RATE_SHIFT) == UfsRateB) {
        if (!RateB) {
          continue;
        }
      }

      switch (((Flags & UFS_GEAR_MASK) >> UFS_GEAR_SHIFT)) {
        case UfsGearMode4:
        {
          if (!LowGearConfig) {
            continue;
          }

          break;
        }
        case UfsGearMode5:
        {
          if (!HighGearConfig) {
            continue;
          }

          break;
        }
        case UfsGearModeDefault:
        {
          if (LowGearConfig || HighGearConfig) {
            continue;
          }

          break;
        }
        default:
          break;
      }
    }

    Value = PhyTable[Loop].Value;
    MmioWrite32 (ControllerData->UfsMemBase + PhyTable[Loop].Offset, Value);
  }

  return Status;
}

/**
  Starts MPHY and waits for PCS ready.

  Sets the MPHY start bit and polls the PCS ready status with a timeout
  derived from the configuration.

  @param[in]  ControllerData  Pointer to controller data.

  @retval EFI_SUCCESS           MPHY start sequence succeeded.
  @retval EFI_INVALID_PARAMETER ControllerData is NULL.
  @retval EFI_DEVICE_ERROR      PCS ready did not assert within timeout.

**/
EFI_STATUS
EFIAPI
UfsMphyStart (
  IN UFS_CONTROLLER_DATA  *ControllerData
  )
{
  UINT32              Timeout;
  UINT32              RegValue;
  UFS_TIMEOUT_CONFIG  *TimeoutConfig;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  TimeoutConfig = &ControllerData->Config.TimeoutConfig;

  RegValue = MmioRead32 (ControllerData->UfsMemBase + UFS_MEM_MPHY_START_OFFSET);

  RegValue |= UFS_MEM_MPHY_START;

  MmioWrite32 (ControllerData->UfsMemBase + UFS_MEM_MPHY_START_OFFSET, RegValue);

  MicroSecondDelay (10);

  Timeout = TimeoutConfig->HcPcsReadyPollTimeout;

  while (TRUE) {
    RegValue = MmioRead32 (ControllerData->UfsMemBase + UFS_MEM_MPHY_PCS_READY_OFFSET);

    if ((RegValue & UFS_MEM_MPHY_PCS_READY) != 0) {
      break;
    }

    if (Timeout == 0) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] MPHY PCS ready timeout\n"));
      return EFI_DEVICE_ERROR;
    }

    Timeout--;
    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

/**
  Configures MPHY using platform tables.

  Validates controller capabilities, then applies Init/Lane/End PHY tables
  according to controller version and lane count.

  @param[in,out] ControllerData     Pointer to controller data containing caps/config.
  @param[in]     HcDriverInterface  UFS host controller driver interface.

  @retval EFI_SUCCESS           MPHY configuration completed.
  @retval EFI_INVALID_PARAMETER Parameter validation failed.
  @retval EFI_NOT_STARTED       Required interfaces/capabilities are not ready.
  @retval EFI_DEVICE_ERROR      PHY programming failed.

**/
EFI_STATUS
EFIAPI
UfsMphyConfigure (
  IN OUT UFS_CONTROLLER_DATA            *ControllerData,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                          Status;
  UFS_CAPABILITIES                    *HostCaps;
  UFS_SPEED_CONFIG                    *SpeedConfig;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;

  UfsHc = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsHc = HcDriverInterface->UfsHcProtocol;
  if (UfsHc == NULL) {
    return EFI_NOT_STARTED;
  }

  HostCaps = &ControllerData->HostCaps;
  if (!HostCaps->Valid) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UfsMphyConfigure: HostCaps not valid -- capabilities not yet populated\n"));
    return EFI_NOT_STARTED;
  }

  SpeedConfig = &ControllerData->Config.SpeedConfig;

  Status = UfsPhyControl (ControllerData, UfsPhyTableInit);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsPhy] Failed to program PHY table: Init [Status=%r]\n", Status));
    goto Exit;
  }

  Status = UfsPhyControl (ControllerData, UfsPhyTableInitLane0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsPhy] Failed to program PHY table: Lane0 [Status=%r]\n", Status));
    goto Exit;
  }

  if ((ControllerData->Version >= UFS_VERSION_3_1) &&
      (ControllerData->HostCaps.TxLanes == UFS_LANE_COUNT_DUAL))
  {
    Status = UfsPhyControl (ControllerData, UfsPhyTableInitLane1);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsPhy] Failed to program PHY table: Lane1 [Status=%r]\n", Status));
      goto Exit;
    }
  }

  Status = UfsPhyControl (ControllerData, UfsPhyTableInitEnd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsPhy] Failed to program PHY table: InitEnd [Status=%r]\n", Status));
    goto Exit;
  }

  if (SpeedConfig->LinkStartupMode == UfsLinkStartupModeHsLss) {
    Status = UfsPhyControl (ControllerData, UfsPhyTableHsLss);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsPhy] Failed to program PHY table: HsLss [Status=%r]\n", Status));
      goto Exit;
    }
  }

Exit:
  return Status;
}
