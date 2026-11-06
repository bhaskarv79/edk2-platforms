/** @file
  UFS Host Controller Platform Protocol callback implementation.

  Provides platform-specific handling for UFS HC initialization phases
  including pre-HCE, post-HCE, pre-link, and post-link startup callbacks.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#include "QcomUfsHcDxe.h"
#include "UfsHcPlatform.h"

/**
  Runs the EdkiiUfsHcPreHce phase: UEFI bring-up of the PHY and host
  controller, when the configuration enables it.

  @param[in]      Private            The controller's private data.
  @param[in]      HcDriverInterface  The controller's driver interface.

  @retval EFI_SUCCESS  The phase completed or is disabled.
  @retval other        The UfsPreHce() error.
**/
STATIC
EFI_STATUS
UfsHcPlatformPreHce (
  IN QCOM_UFS_HC_PRIVATE_DATA       *Private,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  DEBUG ((
    DEBUG_INFO,
    "[UfsHcPlat] PreHce: ControllerId=%u PreHceEnable=%u\n",
    Private->ControllerId,
    Private->BaseConfig.PreHceEnable
    ));

  if (!Private->BaseConfig.PreHceEnable) {
    return EFI_SUCCESS;
  }

  return UfsPreHce (
           Private->ControllerId,
           Private->BaseConfig.UfsMemBase,
           Private->BaseConfig.UfsHciBase,
           &Private->ControllerContext,
           HcDriverInterface
           );
}

/**
  Runs the EdkiiUfsHcPostLinkStartup phase: UniPro tuning, power mode
  reconfiguration and device-config event registration, when the
  configuration enables it.

  @param[in]      Private            The controller's private data.
  @param[in]      ControllerHandle   The controller handle.
  @param[in]      HcDriverInterface  The controller's driver interface.

  @retval EFI_SUCCESS  The phase completed or is disabled.
  @retval other        The UfsPostLinkUp() error.
**/
STATIC
EFI_STATUS
UfsHcPlatformPostLinkStartup (
  IN QCOM_UFS_HC_PRIVATE_DATA       *Private,
  IN EFI_HANDLE                     ControllerHandle,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  DEBUG ((
    DEBUG_INFO,
    "[UfsHcPlat] PostLinkStartup: ControllerId=%u PostLinkUpEnable=%u\n",
    Private->ControllerId,
    Private->BaseConfig.PostLinkUpEnable
    ));

  if (!Private->BaseConfig.PostLinkUpEnable) {
    return EFI_SUCCESS;
  }

  return UfsPostLinkUp (
           Private->ControllerId,
           Private->BaseConfig.UfsHciBase,
           ControllerHandle,
           &Private->ControllerContext,
           HcDriverInterface
           );
}

/**
  Platform-specific callback function for UFS Host Controller operations.

  See UfsHcPlatform.h for the contract. EdkiiUfsHcPostHce and
  EdkiiUfsHcPreLinkStartup need no platform work.

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

  if (CallbackData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HcDriverInterface = (EDKII_UFS_HC_DRIVER_INTERFACE *)CallbackData;

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  (VOID **)&UfsHcPlat
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcPlat] No UFS HC platform protocol on handle %p: %r\n",
      ControllerHandle,
      Status
      ));
    return EFI_INVALID_PARAMETER;
  }

  //
  // The handle could carry another producer's instance of the protocol.
  // CR() only ASSERTs the signature, so check it explicitly for RELEASE too.
  //
  Private = BASE_CR (UfsHcPlat, QCOM_UFS_HC_PRIVATE_DATA, UfsHcPlat);
  if (Private->Signature != QCOM_UFS_HC_PRIVATE_DATA_SIGNATURE) {
    DEBUG ((DEBUG_ERROR, "[UfsHcPlat] Handle %p carries another driver's platform protocol\n", ControllerHandle));
    return EFI_INVALID_PARAMETER;
  }

  switch (CallbackPhase) {
    case EdkiiUfsHcPreHce:
      Status = UfsHcPlatformPreHce (Private, HcDriverInterface);
      break;

    case EdkiiUfsHcPostLinkStartup:
      Status = UfsHcPlatformPostLinkStartup (Private, ControllerHandle, HcDriverInterface);
      break;

    case EdkiiUfsHcPostHce:
    case EdkiiUfsHcPreLinkStartup:
      Status = EFI_SUCCESS;
      break;

    default:
      //
      // A phase added by a later protocol version. Both UfsPassThruDxe call
      // sites treat a callback error as fatal, so report it and carry on.
      //
      DEBUG ((
        DEBUG_WARN,
        "[UfsHcPlat] ControllerId=%u: unknown callback phase %u ignored\n",
        Private->ControllerId,
        (UINT32)CallbackPhase
        ));
      Status = EFI_SUCCESS;
      break;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcPlat] CallbackPhase=%u ControllerId=%u failed [Status=%r]\n",
      (UINT32)CallbackPhase,
      Private->ControllerId,
      Status
      ));
  }

  return Status;
}
