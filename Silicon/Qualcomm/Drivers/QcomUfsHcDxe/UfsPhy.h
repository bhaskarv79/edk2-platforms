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

/// UFS specification version 3.1 encoded as BCD (major=3, minor=0, maintenance=1).
#define UFS_VERSION_3_1      0x3001
/// UFS dual lane count for PHY initialisation.
#define UFS_LANE_COUNT_DUAL  2

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
  );

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
  );

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
  );

