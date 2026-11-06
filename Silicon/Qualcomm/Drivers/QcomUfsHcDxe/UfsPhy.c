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

  The entry offsets were validated against the PHY register window when the
  table was parsed (UfsDtGetConfigData()).

  @param[in]  ControllerData  Pointer to controller data containing MMIO base and config.
  @param[in]  PhyTableType    Table selector (Init, Lane0, Lane1, End).

  @retval EFI_SUCCESS           PHY programming completed successfully.
  @retval EFI_INVALID_PARAMETER ControllerData is NULL, PhyTableType is out of range,
                                or the table has entries but no entry array.

**/
EFI_STATUS
EFIAPI
UfsPhyControl (
  IN UFS_CONTROLLER_DATA  *ControllerData,
  IN UFS_PHY_TABLE_TYPE   PhyTableType
  )
{
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

  if ((PhyTable == NULL) && (NumEntries != 0)) {
    DEBUG ((DEBUG_ERROR, "[UfsPhy] PHY table %d has %u entries but no entry array\n", PhyTableType, NumEntries));
    return EFI_INVALID_PARAMETER;
  }

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

  return EFI_SUCCESS;
}

/**
  Starts MPHY and waits for PCS ready.

  See UfsPhy.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsMphyStart (
  IN UFS_CONTROLLER_DATA  *ControllerData
  )
{
  EFI_STATUS  Status;
  UINT32      RegValue;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MmioOr32 (ControllerData->UfsMemBase + UFS_MEM_MPHY_START_OFFSET, UFS_MEM_MPHY_START);
  MicroSecondDelay (UFS_MPHY_START_DELAY_US);

  Status = UfsMmioPoll32 (
             ControllerData->UfsMemBase + UFS_MEM_MPHY_PCS_READY_OFFSET,
             UFS_MEM_MPHY_PCS_READY,
             UFS_MEM_MPHY_PCS_READY,
             ControllerData->Config.TimeoutConfig.HcPcsReadyPollTimeout,
             &RegValue
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] MPHY PCS ready wait failed, PCS status 0x%x [Status=%r]\n", RegValue, Status));
  }

  return Status;
}

/**
  Configures MPHY using platform tables.

  Applies the Init/Lane/End PHY tables according to the controller version
  and the configured lane count.

  The lane 1 table is applied when the configuration uses two lanes. The
  PHY is programmed before the host controller is enabled, when no UIC
  command can be sent, so the lane count comes from the configuration rather
  than from PA_AvailTxDataLanes.

  @param[in,out] ControllerData     Pointer to controller data containing caps/config.

  @retval EFI_SUCCESS           MPHY configuration completed.
  @retval EFI_INVALID_PARAMETER ControllerData is NULL, or a PHY table is malformed.

**/
EFI_STATUS
EFIAPI
UfsMphyConfigure (
  IN OUT UFS_CONTROLLER_DATA  *ControllerData
  )
{
  EFI_STATUS        Status;
  UFS_SPEED_CONFIG  *SpeedConfig;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
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
      (SpeedConfig->NumLanes >= UFS_LANE_COUNT_DUAL))
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
