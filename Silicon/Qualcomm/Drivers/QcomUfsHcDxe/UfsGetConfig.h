/** @file
  Declarations for UFS controller configuration data retrieval.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage

**/

#pragma once

#include "UfsCore.h"

/**
  Retrieves the UFS configuration data for the specified controller.

  This function returns the platform-specific configuration parameters
  required for UFS host controller initialization, including gear settings,
  lane configuration, and vendor-specific parameters.

  @param[in]  ControllerId    The unique identifier of the UFS controller.
  @param[out] Config          Pointer to the UFS_CONFIG_DATA structure to be
                              populated with configuration parameters.

  @retval EFI_SUCCESS         Configuration data retrieved successfully.
  @retval EFI_INVALID_PARAMETER Invalid ControllerId or Config pointer is NULL.
  @retval EFI_NOT_FOUND       Configuration data not found for the specified controller.
  @retval EFI_DEVICE_ERROR    Failed to retrieve configuration data due to hardware error.

**/
EFI_STATUS
EFIAPI
UfsGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA  *Config
  );

/**
  Retrieves the power management configuration for the specified voltage corner.

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
  );

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
  OUT UFS_BASE_CONFIG  *BaseConfig
  );

