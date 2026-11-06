/** @file
  Qcom UFS Host Controller DXE driver entry point.

  For each UFS controller, installs EDKII_UFS_HC_PLATFORM_PROTOCOL and
  registers the UFSHCI MMIO window as a non-discoverable UFS MMIO device on
  the same handle, for the generic UfsMemHcDxe driver to bind to.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/DeviceTreeExtension.h>

#include "QcomUfsHcDxe.h"
#include "UfsGetConfig.h"
#include "UfsHcPlatform.h"
#include "UfsUtils.h"

/**
  Logs a controller-init failure and frees its private data.

  Shared tail for the QcomUfsHcDriverEntry() per-controller failure paths.
  The caller must have uninstalled any protocol it installed on
  Private->Handle before calling this.

  The GCD entry UfsRegisterMmioRange() may have added for the HCI window is
  intentionally left in place: the range is MMIO whether or not this driver
  uses it, and re-registering it is idempotent.

  @param[in] ControllerIndex   Controller index, used for diagnostic messages.
  @param[in] Step              Short name of the step that failed, e.g. "UfsGetBaseConfig".
  @param[in] Status            The failing status returned by that step.
  @param[in] Private           Private data to free.

**/
STATIC
VOID
UfsHcAbandonController (
  IN UINTN                     ControllerIndex,
  IN CONST CHAR8               *Step,
  IN EFI_STATUS                Status,
  IN QCOM_UFS_HC_PRIVATE_DATA  *Private
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "[QcomUfsHcDxe] Controller[%u]: %a failed, controller not registered - %r\n",
    (UINT32)ControllerIndex,
    Step,
    Status
    ));
  FreePool (Private);
}

/**
  Resolves BaseConfig, fills in the platform protocol from it, and
  self-registers the controller's HCI MMIO window with the GCD.

  @param[in,out] Private          Private data for this controller; BaseConfig
                                  and UfsHcPlat are populated on success.
  @param[in]     ControllerIndex  Controller index, used for diagnostic messages.

  @retval EFI_SUCCESS   Configuration resolved and MMIO window registered.
  @return               Any error returned by UfsGetBaseConfig() or
                        UfsRegisterMmioRange().

**/
STATIC
EFI_STATUS
UfsHcInitControllerConfig (
  IN OUT QCOM_UFS_HC_PRIVATE_DATA  *Private,
  IN     UINTN                     ControllerIndex
  )
{
  EFI_STATUS  Status;

  Status = UfsGetBaseConfig (
             Private->ControllerId,
             &Private->BaseConfig
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // UfsPreHce() leaves the host controller enabled with UniPro clock and LCC
  // settings applied. If UfsPassThruDxe then re-enabled HCE it would reset
  // UniPro and discard them, so a controller that runs the PreHce phase must
  // also skip the HCE re-enable.
  //
  if (Private->BaseConfig.PreHceEnable && !Private->BaseConfig.SkipHceReenable) {
    DEBUG ((
      DEBUG_WARN,
      "[QcomUfsHcDxe] Controller[%u]: PreHceEnable requires SkipHceReenable; forcing it\n",
      (UINT32)ControllerIndex
      ));
    Private->BaseConfig.SkipHceReenable = TRUE;
  }

  Private->UfsHcPlat.Version         = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  Private->UfsHcPlat.OverrideHcInfo  = NULL;
  Private->UfsHcPlat.Callback        = UfsHcPlatformCallback;
  Private->UfsHcPlat.RefClkFreq      = (EDKII_UFS_CARD_REF_CLK_FREQ_ATTRIBUTE)Private->BaseConfig.RefClkFreq;
  Private->UfsHcPlat.SkipHceReenable = (BOOLEAN)Private->BaseConfig.SkipHceReenable;
  Private->UfsHcPlat.SkipLinkStartup = (BOOLEAN)Private->BaseConfig.SkipLinkStartup;

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%u]: PreHceEnable=%u PostLinkUpEnable=%u RefClkFreq=%u\n",
    (UINT32)ControllerIndex,
    Private->BaseConfig.PreHceEnable,
    Private->BaseConfig.PostLinkUpEnable,
    Private->UfsHcPlat.RefClkFreq
    ));
  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%u]: SkipHceReenable=%u SkipLinkStartup=%u\n",
    (UINT32)ControllerIndex,
    Private->UfsHcPlat.SkipHceReenable,
    Private->UfsHcPlat.SkipLinkStartup
    ));

  //
  // UfsMemHcDxe and UfsPassThruDxe access the HCI registers through the
  // non-discoverable device's MMIO resource, which must be uncached.
  //
  Status = UfsRegisterMmioRange (Private->BaseConfig.UfsHciBase, UFS_HC_MMIO_WINDOW_SIZE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%u]: MemBase=0x%lx HciBase=0x%lx\n",
    (UINT32)ControllerIndex,
    Private->BaseConfig.UfsMemBase,
    Private->BaseConfig.UfsHciBase
    ));

  return EFI_SUCCESS;
}

///
/// Deferred registration when the configuration comes from the device tree:
/// the DTB extension protocol notification and the EndOfDxe fallback.
///
STATIC VOID       *mDtbExtnRegistration;
STATIC EFI_EVENT  mDtbExtnEvent;
STATIC EFI_EVENT  mEndOfDxeEvent;

/**
  Registers every UFS controller.

  For each UFS controller, resolves its configuration, installs the UFS Host
  Controller Platform Protocol on a new handle and registers the UFSHCI MMIO
  window on that same handle as a non-discoverable UFS MMIO device. UfsMemHcDxe
  binds to the device and produces EDKII_UFS_HOST_CONTROLLER_PROTOCOL on the
  handle, so the ControllerHandle that UfsPassThruDxe passes to the platform
  callback is the handle that carries the platform protocol.

  @retval EFI_SUCCESS             At least one controller was registered.
  @return                         The error from the last failing controller
                                  when none was registered.

**/
STATIC
EFI_STATUS
UfsHcRegisterControllers (
  VOID
  )
{
  EFI_STATUS                Status;
  QCOM_UFS_HC_PRIVATE_DATA  *Private;
  UINTN                     ControllerIndex;
  UINTN                     SuccessCount;

  Status       = EFI_DEVICE_ERROR;
  SuccessCount = 0;

  //
  // Only one controller is supported. UfsPassThruDxe locates a single
  // EDKII_UFS_HC_PLATFORM_PROTOCOL with LocateProtocol() and reads RefClkFreq,
  // SkipHceReenable and SkipLinkStartup from it for every controller; only
  // the Callback is resolved per controller handle. A second controller with
  // different settings would need that consumer to change first.
  //
  for (ControllerIndex = 0; ControllerIndex < UfsNumControllers; ControllerIndex++) {
    Private = AllocateZeroPool (sizeof (QCOM_UFS_HC_PRIVATE_DATA));
    if (Private == NULL) {
      DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] Controller[%u]: AllocateZeroPool failed\n", (UINT32)ControllerIndex));
      Status = EFI_OUT_OF_RESOURCES;
      continue;
    }

    Private->Signature    = QCOM_UFS_HC_PRIVATE_DATA_SIGNATURE;
    Private->ControllerId = (UFS_CONTROLLER_ID)ControllerIndex;

    Status = UfsHcInitControllerConfig (Private, ControllerIndex);
    if (EFI_ERROR (Status)) {
      UfsHcAbandonController (ControllerIndex, "UfsHcInitControllerConfig", Status, Private);
      continue;
    }

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->Handle,
                    &gEdkiiUfsHcPlatformProtocolGuid,
                    &Private->UfsHcPlat,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      UfsHcAbandonController (ControllerIndex, "InstallUfsHcPlatform", Status, Private);
      continue;
    }

    //
    // Installs NON_DISCOVERABLE_DEVICE and a unique device path on
    // Private->Handle.
    //
    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeUfsMmio,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               &Private->Handle,
               1,
               (UINTN)Private->BaseConfig.UfsHciBase,
               (UINTN)UFS_HC_MMIO_WINDOW_SIZE
               );
    if (EFI_ERROR (Status)) {
      gBS->UninstallMultipleProtocolInterfaces (
             Private->Handle,
             &gEdkiiUfsHcPlatformProtocolGuid,
             &Private->UfsHcPlat,
             NULL
             );
      UfsHcAbandonController (ControllerIndex, "RegisterNonDiscoverableMmioDevice", Status, Private);
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "[QcomUfsHcDxe] Controller[%u]: ready on handle %p\n",
      (UINT32)ControllerIndex,
      Private->Handle
      ));
    SuccessCount++;
  }

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] %u/%u controllers registered\n",
    (UINT32)SuccessCount,
    (UINT32)UfsNumControllers
    ));

  return (SuccessCount != 0) ? EFI_SUCCESS : Status;
}

/**
  Registers the controllers once, from the first of the DTB extension
  protocol notification and EndOfDxe, and closes both events.

**/
STATIC
VOID
UfsHcRegisterDeferred (
  VOID
  )
{
  if (mDtbExtnEvent != NULL) {
    gBS->CloseEvent (mDtbExtnEvent);
    mDtbExtnEvent = NULL;
  }

  if (mEndOfDxeEvent != NULL) {
    gBS->CloseEvent (mEndOfDxeEvent);
    mEndOfDxeEvent = NULL;
  }

  UfsHcRegisterControllers ();
}

/**
  DTB extension protocol notification: the device tree configuration can now
  be read.

  @param[in] Event    The event.
  @param[in] Context  Not used.

**/
STATIC
VOID
EFIAPI
UfsHcDtbExtnNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  VOID  *DtbExtn;

  if (EFI_ERROR (gBS->LocateProtocol (&gDtbExtnProtocolGuid, mDtbExtnRegistration, &DtbExtn))) {
    return;
  }

  UfsHcRegisterDeferred ();
}

/**
  EndOfDxe: no DTB extension protocol was installed; register from the PCDs,
  as UfsGetBaseConfig() does when the protocol is missing.

  @param[in] Event    The event.
  @param[in] Context  Not used.

**/
STATIC
VOID
EFIAPI
UfsHcEndOfDxeNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_WARN, "[QcomUfsHcDxe] No DTB extension protocol by EndOfDxe\n"));
  UfsHcRegisterDeferred ();
}

/**
  Entry point for the UFS DXE driver.

  Registers the UFS controllers (UfsHcRegisterControllers()). When the
  configuration comes from the device tree and the DTB extension protocol is
  not installed yet, the registration waits for it, or for EndOfDxe if it
  never comes, instead of depending on the driver dispatch order.

  @param[in] ImageHandle       The firmware allocated handle for the EFI image.
  @param[in] SystemTable       Pointer to the EFI System Table.

  @retval EFI_SUCCESS             At least one controller was registered, or
                                  the registration was deferred.
  @retval EFI_NOT_FOUND           No UFS controllers are defined.
  @retval EFI_OUT_OF_RESOURCES    The deferral events could not be created.
  @return                         The error from the last failing controller
                                  when none was registered.

**/
EFI_STATUS
EFIAPI
QcomUfsHcDriverEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *DtbExtn;

  if (UfsNumControllers == 0) {
    DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] No UFS controllers defined (UfsNumControllers == 0)\n"));
    return EFI_NOT_FOUND;
  }

  if ((FixedPcdGet8 (PcdUfsConfigSourceDeviceTree) == 0) ||
      !EFI_ERROR (gBS->LocateProtocol (&gDtbExtnProtocolGuid, NULL, &DtbExtn)))
  {
    return UfsHcRegisterControllers ();
  }

  DEBUG ((DEBUG_INFO, "[QcomUfsHcDxe] Waiting for the DTB extension protocol\n"));
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  UfsHcEndOfDxeNotify,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &mEndOfDxeEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDtbExtnEvent = EfiCreateProtocolNotifyEvent (
                    &gDtbExtnProtocolGuid,
                    TPL_CALLBACK,
                    UfsHcDtbExtnNotify,
                    NULL,
                    &mDtbExtnRegistration
                    );
  if (mDtbExtnEvent == NULL) {
    gBS->CloseEvent (mEndOfDxeEvent);
    mEndOfDxeEvent = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}
