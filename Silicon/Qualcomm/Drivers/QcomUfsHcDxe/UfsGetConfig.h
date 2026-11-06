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

  Reads the configuration the PreHce and PostLinkUp phases need (speed,
  voltage corner, timeouts, reset/shared-PHY/spare registers, PHY tables and
  the de-emphasis device list) from the device tree, validates it, and looks
  up the power management settings for its voltage corner. There is no PCD
  fallback: UFS bring-up requires the device tree.

  @param[in]  ControllerId    The unique identifier of the UFS controller.
  @param[out] Config          Pointer to the UFS_CONFIG_DATA structure to be
                              populated with configuration parameters. On
                              error, Config holds no heap allocations. Release
                              a successful result with UfsFreeConfigData().

  @retval EFI_SUCCESS            Configuration data retrieved successfully.
  @retval EFI_INVALID_PARAMETER  Invalid ControllerId or Config pointer is NULL,
                                 or the voltage corner is out of range.
  @retval EFI_UNSUPPORTED        The device tree code is not built
                                 (PcdUfsConfigSourceDeviceTree is 0).
  @retval EFI_NOT_FOUND          The configuration is missing from the device tree,
                                 or there are no settings for its voltage corner.
  @retval EFI_COMPROMISED_DATA   The configuration is malformed or out of range.
  @retval other                  Status from UfsDtGetConfigData().

**/
EFI_STATUS
EFIAPI
UfsGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA    *Config
  );

/**
  Frees the heap allocations UfsGetConfigData() made into Config.

  Safe to call on a partially-populated or already-freed Config. Does NOT
  free Config itself.

  @param[in,out] Config  The configuration to release.
**/
VOID
EFIAPI
UfsFreeConfigData (
  IN OUT UFS_CONFIG_DATA  *Config
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

  Returns the base addresses, the enabled callback phases and the
  EDKII_UFS_HC_PLATFORM_PROTOCOL flags. The device tree is used when it is
  built in and has the controller's BaseConfig node; when the node (or the
  DTB extension protocol) is missing the PcdUfs<n>* PCDs are used, but a
  node that is present and malformed is an error. The PCD fallback cannot
  enable the PreHce or PostLinkUp phases, since they need the device tree
  configuration.

  @param[in]  ControllerId      The unique identifier of the UFS controller.
  @param[out] BaseConfig        Pointer to store the UFS base config.

  @retval EFI_SUCCESS            The base config was retrieved.
  @retval EFI_INVALID_PARAMETER  Invalid ControllerId or NULL pointer parameters.
  @retval EFI_NOT_FOUND          No base address is configured.
  @retval EFI_UNSUPPORTED        The PCD fallback enables a bring-up phase.
  @retval EFI_COMPROMISED_DATA   The device tree BaseConfig node is malformed.
  @retval EFI_DEVICE_ERROR       The device tree could not be parsed.

**/
EFI_STATUS
EFIAPI
UfsGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG    *BaseConfig
  );
