/** @file
  Declarations for UFS PHY configuration and initialization functions.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS    - Universal Flash Storage
  - PHY    - Physical Layer
  - MPHY   - M-PHY (MIPI physical layer specification)
  - SERDES - Serializer/Deserializer

**/

#pragma once

#include <Uefi.h>

#include "UfsCore.h"

///
/// UFS host controller versions, as read from the upper 16 bits of the
/// controller's HW version register (major, minor and step in BCD nibbles).
///
#define UFS_VERSION_3_1  0x3001
#define UFS_VERSION_5_0  0x5000
/// UFS dual lane count for PHY initialisation.
#define UFS_LANE_COUNT_DUAL  2

///
/// Delay after setting the MPHY start bit before polling PCS ready, in
/// microseconds. Carried over from the original bring-up sequence.
///
#define UFS_MPHY_START_DELAY_US  10

/**
  Programs UFS PHY configuration tables.

  Applies the selected PHY table entries based on the active configuration
  (rate and gear). Conditional entries are evaluated and written to the UFS
  PHY MMIO space.

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
  );

/**
  Starts MPHY and waits for PCS ready.

  Sets the MPHY start bit and polls the PCS ready status for up to
  TimeoutConfig.HcPcsReadyPollTimeout microseconds.

  @param[in]  ControllerData  Pointer to controller data.

  @retval EFI_SUCCESS           MPHY start sequence succeeded.
  @retval EFI_INVALID_PARAMETER ControllerData is NULL or the timeout is 0.
  @retval EFI_TIMEOUT           PCS ready did not assert within the timeout.

**/
EFI_STATUS
EFIAPI
UfsMphyStart (
  IN UFS_CONTROLLER_DATA  *ControllerData
  );

/**
  Configures MPHY using platform tables.

  Applies the Init/Lane/End PHY tables according to the controller version
  and the configured lane count. Needs no UIC command, so it can run before
  the host controller is enabled.

  @param[in,out] ControllerData     Pointer to controller data containing caps/config.

  @retval EFI_SUCCESS           MPHY configuration completed.
  @retval EFI_INVALID_PARAMETER ControllerData is NULL, or a PHY table is malformed.

**/
EFI_STATUS
EFIAPI
UfsMphyConfigure (
  IN OUT UFS_CONTROLLER_DATA  *ControllerData
  );
