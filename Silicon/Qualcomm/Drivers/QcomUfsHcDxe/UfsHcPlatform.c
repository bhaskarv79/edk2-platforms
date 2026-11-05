/** @file
  UFS Host Controller Platform Protocol callback implementation.

  Provides platform-specific handling for UFS HC initialization phases
  including pre-HCE, post-HCE, pre-link, and post-link startup callbacks.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#include "QcomUfsHcDxe.h"
#include "UfsHcPlatform.h"

///
/// Compatible version for UFS Host Controller Platform Protocol.
/// Must match EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION from
/// <Protocol/UfsHostControllerPlatform.h> — use that macro directly
/// to prevent silent divergence if the upstream header is updated.
///
#define UFS_HC_PLATFORM_PROTOCOL_COMPATIBLE_VERSION  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION

/**
  Platform-specific callback function for UFS Host Controller operations.

  This function provides platform-specific handling for different phases of UFS
  Host Controller initialization and operation. It is called by the UFS Host
  Controller driver at various stages to perform platform-specific configurations,
  power management, and hardware-specific optimizations.

  The function handles the following callback phases:
  - EdkiiUfsHcPreHce: Before Host Controller Enable
  - EdkiiUfsHcPostHce: After Host Controller Enable
  - EdkiiUfsHcPreLinkStartup: Before UFS link startup
  - EdkiiUfsHcPostLinkStartup: After UFS link startup (calls UfsPostLinkUp)

  @param[in]     ControllerHandle  Handle to the UFS Host Controller instance.
  @param[in]     CallbackPhase     The specific phase of UFS HC operation requiring
                                   platform-specific handling.
  @param[in,out] CallbackData      Pointer to phase-specific data structure.
                                   Contains EDKII_UFS_HC_DRIVER_INTERFACE.

  @retval EFI_SUCCESS              Platform callback executed successfully.
  @retval EFI_INVALID_PARAMETER    Invalid CallbackPhase or CallbackData parameter.
  @retval EFI_DEVICE_ERROR         Failed to retrieve private data structure.
  @retval EFI_INCOMPATIBLE_VERSION Platform protocol version mismatch.
  @retval EFI_UNSUPPORTED          The specified CallbackPhase is not supported.

**/
EFI_STATUS
EFIAPI
UfsHcPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  EFI_STATUS                      Status;
  QCOM_UFS_HC_PRIVATE_DATA        *Private;
  EDKII_UFS_HC_DRIVER_INTERFACE   *HcDriverInterface;
  EDKII_UFS_HC_PLATFORM_PROTOCOL  *UfsHcPlat;

  Status            = EFI_SUCCESS;
  Private           = NULL;
  HcDriverInterface = NULL;
  UfsHcPlat         = NULL;

  if (CallbackData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get the UFS Host Controller driver interface from callback data
  //
  HcDriverInterface = (EDKII_UFS_HC_DRIVER_INTERFACE *)CallbackData;

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  (VOID **)&UfsHcPlat
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcPlat] HandleProtocol(gEdkiiUfsHcPlatformProtocolGuid) failed: %r\n",
      Status
      ));
    return Status;
  }

  Private = QCOM_UFS_HC_PRIVATE_DATA_FROM_PLATFORM (UfsHcPlat);

  if (Private == NULL) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Verify protocol version compatibility
  //
  if (UfsHcPlat->Version != UFS_HC_PLATFORM_PROTOCOL_COMPATIBLE_VERSION) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcPlat] Version mismatch: got %u, expected %u\n",
      UfsHcPlat->Version,
      UFS_HC_PLATFORM_PROTOCOL_COMPATIBLE_VERSION
      ));
    return EFI_INCOMPATIBLE_VERSION;
  }

  //
  // Handle different callback phases
  //
  switch (CallbackPhase) {
    case EdkiiUfsHcPreHce:
    {
      DEBUG ((
        DEBUG_INFO,
        "[UfsHcPlat] PreHce: ControllerId=%d PreHceEnable=%d\n",
        Private->ControllerId,
        Private->BaseConfig.PreHceEnable
        ));
      if (Private->BaseConfig.PreHceEnable) {
        //
        // Pre-HCE (Host Controller Enable) phase
        // Platform-specific initialization before enabling the host controller
        //
        Status = UfsPreHce (
                   Private->ControllerId,
                   Private->BaseConfig.UfsMemBase,
                   Private->BaseConfig.UfsHciBase,
                   &Private->ControllerContext,
                   HcDriverInterface
                   );
      } else {
        Status = EFI_SUCCESS;
      }

      break;
    }
    case EdkiiUfsHcPostHce:
    {
      DEBUG ((
        DEBUG_INFO,
        "[UfsHcPlat] PostHce: ControllerId=%d (no-op)\n",
        Private->ControllerId
        ));
      break;
    }
    case EdkiiUfsHcPreLinkStartup:
    {
      DEBUG ((
        DEBUG_INFO,
        "[UfsHcPlat] PreLinkStartup: ControllerId=%d (no-op)\n",
        Private->ControllerId
        ));
      break;
    }
    case EdkiiUfsHcPostLinkStartup:
    {
      DEBUG ((
        DEBUG_INFO,
        "[UfsHcPlat] PostLinkStartup: ControllerId=%d PostLinkUpEnable=%d\n",
        Private->ControllerId,
        Private->BaseConfig.PostLinkUpEnable
        ));
      if (Private->BaseConfig.PostLinkUpEnable) {
        //
        // Post-Link Startup phase
        // Perform platform-specific post-link-up initialization
        //
        Status = UfsPostLinkUp (
                   Private->ControllerId,
                   Private->BaseConfig.UfsHciBase,
                   ControllerHandle,
                   &Private->ControllerContext,
                   HcDriverInterface
                   );
      } else {
        Status = EFI_SUCCESS;
      }

      break;
    }
    default:
      //
      // Unrecognised callback phase
      //
      Status = EFI_UNSUPPORTED;
      break;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcPlat] CallbackPhase=%d ControllerId=%d failed [Status=%r]\n",
      CallbackPhase,
      Private->ControllerId,
      Status
      ));
  }

  return Status;
}
