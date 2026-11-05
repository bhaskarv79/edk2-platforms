/** @file
  Private definitions and data structures for the Qcom UFS Host Controller DXE driver.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

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
/// as the MMIO resource of the non-discoverable UFS device.
///
#define UFS_HC_MMIO_WINDOW_SIZE  SIZE_4KB

///
/// Macro to retrieve the private data structure from the UFS Host Controller Platform Protocol
/// This macro uses the ContainerOf (CR) mechanism to get the containing structure
///
#define QCOM_UFS_HC_PRIVATE_DATA_FROM_PLATFORM(a) \
  CR (a, \
      QCOM_UFS_HC_PRIVATE_DATA, \
      UfsHcPlat, \
      QCOM_UFS_HC_PRIVATE_DATA_SIGNATURE \
      )
