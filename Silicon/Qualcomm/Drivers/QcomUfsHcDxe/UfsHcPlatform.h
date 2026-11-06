/** @file
  Declarations for the UFS Host Controller Platform Protocol callback.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>
#include <Protocol/UfsHostControllerPlatform.h>

/**
  Platform-specific callback function for UFS Host Controller operations.

  This function provides platform-specific handling for different phases of UFS
  Host Controller initialization and operation. It is called by the UFS Host
  Controller driver at various stages to perform platform-specific configurations,
  power management, and hardware-specific optimizations.

  @param[in]     ControllerHandle  Handle to the UFS Host Controller instance.
  @param[in]     CallbackPhase     The specific phase of UFS HC operation requiring
                                   platform-specific handling.
  @param[in,out] CallbackData      The EDKII_UFS_HC_DRIVER_INTERFACE of the
                                   controller, for every phase this protocol
                                   version defines (see
                                   MdeModulePkg/Include/Protocol/UfsHostControllerPlatform.h).

  @retval EFI_SUCCESS              The phase was handled, or needs no work. An
                                   unknown phase is treated as needing no work.
  @retval EFI_INVALID_PARAMETER    CallbackData is NULL, or ControllerHandle
                                   does not carry this driver's protocol.
  @retval other                    The UfsPreHce() or UfsPostLinkUp() error.

**/
EFI_STATUS
EFIAPI
UfsHcPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  );
