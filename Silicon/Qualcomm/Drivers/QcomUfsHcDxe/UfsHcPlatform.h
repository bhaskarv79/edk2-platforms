/** @file
  Declarations for the UFS Host Controller Platform Protocol callback.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "QcomUfsHcDxe.h"

/**
  Platform-specific callback function for UFS Host Controller operations.

  This function provides platform-specific handling for different phases of UFS
  Host Controller initialization and operation. It is called by the UFS Host
  Controller driver at various stages to perform platform-specific configurations,
  power management, and hardware-specific optimizations.

  @param[in]     ControllerHandle  Handle to the UFS Host Controller instance.
  @param[in]     CallbackPhase     The specific phase of UFS HC operation requiring
                                   platform-specific handling.
  @param[in,out] CallbackData      Pointer to phase-specific data structure.
                                   The content and type depend on the CallbackPhase.

  @retval EFI_SUCCESS              Platform callback executed successfully.
  @retval EFI_INVALID_PARAMETER    Invalid CallbackPhase or CallbackData parameter.
  @retval EFI_UNSUPPORTED          The specified CallbackPhase is not supported.
  @retval EFI_DEVICE_ERROR         Platform-specific hardware error occurred.
  @retval EFI_INCOMPATIBLE_VERSION Platform protocol version mismatch.

**/
EFI_STATUS
EFIAPI
UfsHcPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  );

