/** @file
  Private definitions and data structures for the Qcom UFS Host Controller DXE driver.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#include <Protocol/UfsHostControllerPlatform.h>

#include "UfsCore.h"

///
/// Qualcomm UFS Host Controller Private Data Structure
/// Contains all the private data and protocol instances for the UFS host controller driver
///
struct _QCOM_UFS_HC_PRIVATE_DATA {
  UINT32                            Signature;            ///< Unique signature for structure validation
  EFI_HANDLE                        Handle;               ///< Handle to the controller instance
  EDKII_UFS_HC_PLATFORM_PROTOCOL    UfsHcPlat;            ///< UFS Host Controller Platform Protocol instance
  UFS_BASE_CONFIG                   BaseConfig;           ///< UFS Base configuration
  UFS_CONTROLLER_ID                 ControllerId;         ///< UFS Controller Id
  VOID                              *ControllerContext;   ///< UFS Controller Context
};

typedef struct _QCOM_UFS_HC_PRIVATE_DATA QCOM_UFS_HC_PRIVATE_DATA;

///
/// Unique signature for Qualcomm UFS Host Controller private data structure validation
///
#define QCOM_UFS_HC_PRIVATE_DATA_SIGNATURE  SIGNATURE_32 ('U','F','S','H')

///
/// Size of the UFS HCI register window registered with the GCD and published
/// as the MMIO resource of the non-discoverable UFS device. It covers the
/// UFSHCI standard registers (below 0x100) and the vendor registers this
/// driver uses (UFS_HC_CFG0_OFFSET, UFS_HC_CFG1_OFFSET, UFS_HC_HW_VER_OFFSET).
/// The PHY registers are not in it: UfsPreHce() maps them separately, relative
/// to UfsMemBase, sized from the PHY tables (UFS_PHY_CONFIG.RegisterWindowSize).
///
#define UFS_HC_MMIO_WINDOW_SIZE  SIZE_4KB
