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

/**
  Retrieves the base config for UFS Driver from device tree.

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
UfsDtGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG  *BaseConfig
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
  @retval EFI_NOT_FOUND          Required DT nodes/properties not found.
  @retval EFI_DEVICE_ERROR       Failed to parse DT properties.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure while building tables.
**/
EFI_STATUS
EFIAPI
UfsDtGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA  *Config
  );

