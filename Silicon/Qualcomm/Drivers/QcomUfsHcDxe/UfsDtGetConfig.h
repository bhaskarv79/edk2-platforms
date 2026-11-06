/** @file
  Declarations for retrieving UFS configuration and base addresses from device tree.

  Provides prototypes for functions that parse platform device tree (DT) to obtain
  UFS host controller base addresses and platform-specific configuration parameters.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage
  - DTB - Device Tree Blob
  - HCI - Host Controller Interface

**/

#pragma once

#include "UfsCore.h"

///
/// Maximum byte size of a single PHY init table sourced from the DTB.
/// UFS_PHY_TABLE entries are 6 bytes each; 512 entries = 3072 bytes.
/// 4096 provides a generous platform ceiling and prevents oversized allocations
/// or NumEntries overflow from a malformed DTB.
///
#define UFS_PHY_TABLE_MAX_SIZE  4096U

///
/// Maximum number of de-emphasis vendor entries sourced from the DTB.
/// A value beyond this indicates a malformed DTB and prevents the
/// sizeof(UFS_PRODUCT_LIST) * SubNodeCount multiplication from overflowing
/// on 32-bit UEFI builds.
///
#define UFS_MAX_VENDOR_COUNT  32U

///
/// PHY table offsets are UINT16, so the tables can reach at most 64 KiB above
/// UfsMemBase. Each entry is written as a 32-bit register, so offset + 4 must
/// stay within this. The register space actually mapped for PHY programming
/// is the extent the tables use (UFS_PHY_CONFIG.RegisterWindowSize).
///
#define UFS_PHY_REGISTER_WINDOW_MAX  SIZE_64KB

/**
  Retrieves the base config for UFS Driver from device tree.

  Reads the BaseConfig node of /soc/UfsController<n>: base addresses,
  enabled callback phases and the EDKII_UFS_HC_PLATFORM_PROTOCOL flags. All
  seven properties are required.

  @param[in]  ControllerId      The unique identifier of the UFS controller.
  @param[out] BaseConfig        Pointer to store the UFS base config.

  @retval EFI_SUCCESS            The base config was read.
  @retval EFI_INVALID_PARAMETER  Invalid ControllerId or NULL pointer parameters.
  @retval EFI_NOT_FOUND          The DTB extension protocol, the node, or a base
                                 address is missing.
  @retval EFI_COMPROMISED_DATA   The node does not hold exactly the expected properties.
  @retval EFI_DEVICE_ERROR       The DTB could not be parsed.

**/
EFI_STATUS
EFIAPI
UfsDtGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG    *BaseConfig
  );

/**
  Retrieves the UFS configuration data for the specified controller from device tree.

  This function parses platform-specific configuration parameters required for UFS
  host controller initialization, including speed, resource, timeout, and register
  configurations, PHY tables, and de-emphasis vendor/product lists.

  @param[in]  ControllerId    The unique identifier of the UFS controller.
  @param[out] Config          Pointer to the UFS_CONFIG_DATA structure to be
                              populated with configuration parameters.

  @retval EFI_SUCCESS            Configuration data retrieved successfully.
  @retval EFI_INVALID_PARAMETER  Invalid ControllerId or Config pointer is NULL.
  @retval EFI_NOT_FOUND          The DTB extension protocol or a required node or
                                 property is missing.
  @retval EFI_COMPROMISED_DATA   A node or table is malformed.
  @retval EFI_DEVICE_ERROR       The DTB could not be parsed.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure while building tables.
**/
EFI_STATUS
EFIAPI
UfsDtGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA    *Config
  );

/**
  Frees all heap allocations made by UfsDtGetConfigData() into Config.

  Safe to call on a partially-populated or already-freed Config (NULL
  pointers are skipped, and freed pointers are cleared). Does NOT free
  Config itself.

  @param[in,out] Config  Pointer to the config structure whose members to free.
**/
VOID
EFIAPI
UfsDtFreeConfigData (
  IN OUT UFS_CONFIG_DATA  *Config
  );
