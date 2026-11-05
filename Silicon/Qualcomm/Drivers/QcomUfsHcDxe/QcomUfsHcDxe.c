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
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "QcomUfsHcDxe.h"
#include "UfsGetConfig.h"
#include "UfsHcPlatform.h"

///
/// Template for UFS Host Controller private data structure
/// This template is used to initialize new controller instances
///
/// This is a read-only copy source for AllocateCopyPool() in
/// QcomUfsHcDriverEntry(). No locking is required: DXE dispatch is
/// single-threaded and the template is never written after module load.
///
STATIC CONST QCOM_UFS_HC_PRIVATE_DATA  mUfsHcTemplate = {
  QCOM_UFS_HC_PRIVATE_DATA_SIGNATURE,     ///< Structure signature
  NULL,                                   ///< Handle (set at runtime)
  {                                       ///< EDKII_UFS_HC_PLATFORM_PROTOCOL
    EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
    NULL,                                 ///< OverrideHcInfo not used
    UfsHcPlatformCallback,                ///< PHY init callbacks
    0,                                    ///< RefClkFreq (filled from BaseConfig.RefClkFreq, PCD/DT-configurable)
    0,                                    ///< SkipHceReenable (filled from BaseConfig.SkipHceReenable, PCD/DT-configurable)
    0,                                    ///< SkipLinkStartup (filled from BaseConfig.SkipLinkStartup, PCD/DT-configurable)
  },
  {                                       ///< UFS_BASE_CONFIG
    0,                                    ///< UfsMemBase
    0,                                    ///< UfsHciBase
    0,                                    ///< PreHceEnable
    0,                                    ///< PostLinkUpEnable
    0,                                    ///< RefClkFreq
    0,                                    ///< SkipHceReenable
    0,                                    ///< SkipLinkStartup
  },
  UfsNumControllers,                      ///< UFS Controller Id -- invalid; overwritten per-controller in the loop
  NULL,                                   ///< UFS Controller Context
};

/**
  Self-registers the UFS HCI MMIO window with the GCD as uncached
  memory-mapped I/O.

  @param[in] HciBase        Physical base address of the HCI register window.
  @param[in] ControllerId   Controller index, used for diagnostic messages.

  @retval EFI_SUCCESS       Window registered and marked UC successfully.
  @return                   Status from AddMemorySpace or SetMemorySpaceAttributes
                            on failure.

**/
STATIC
EFI_STATUS
UfsHcRegisterMmioWindow (
  IN EFI_PHYSICAL_ADDRESS  HciBase,
  IN UINTN                 ControllerId
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDesc;

  Status = gDS->GetMemorySpaceDescriptor (HciBase, &GcdDesc);
  if (EFI_ERROR (Status) || (GcdDesc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeMemoryMappedIo,
                    HciBase,
                    UFS_HC_MMIO_WINDOW_SIZE,
                    EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "[QcomUfsHcDxe] Controller[%u]: AddMemorySpace 0x%lx - %r\n",
        (UINT32)ControllerId,
        HciBase,
        Status
        ));
      return Status;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (HciBase, UFS_HC_MMIO_WINDOW_SIZE, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[QcomUfsHcDxe] Controller[%u]: SetMemorySpaceAttributes 0x%lx - %r (HCI window may remain cacheable)\n",
      (UINT32)ControllerId,
      HciBase,
      Status
      ));
  } else {
    DEBUG ((DEBUG_INFO, "[QcomUfsHcDxe] Controller[%u]: GCD window 0x%lx registered\n", (UINT32)ControllerId, HciBase));
  }

  return Status;
}

/**
  Logs a controller-init failure and frees its private data.

  Shared tail for the QcomUfsHcDriverEntry() per-controller failure paths.
  The caller must have uninstalled any protocol it installed on
  Private->Handle before calling this.

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
    "[QcomUfsHcDxe] Controller[%u]: %a - %r\n",
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
  @return               Any error returned by UfsGetBaseConfig() or the GCD.

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

  Private->UfsHcPlat.RefClkFreq      = (EDKII_UFS_CARD_REF_CLK_FREQ_ATTRIBUTE)Private->BaseConfig.RefClkFreq;
  Private->UfsHcPlat.SkipHceReenable = (BOOLEAN)Private->BaseConfig.SkipHceReenable;
  Private->UfsHcPlat.SkipLinkStartup = (BOOLEAN)Private->BaseConfig.SkipLinkStartup;

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%u]: PreHceEnable=%u PostLinkUpEnable=%u RefClkFreq=%u SkipHceReenable=%u SkipLinkStartup=%u\n",
    (UINT32)ControllerIndex,
    Private->BaseConfig.PreHceEnable,
    Private->BaseConfig.PostLinkUpEnable,
    Private->UfsHcPlat.RefClkFreq,
    Private->UfsHcPlat.SkipHceReenable,
    Private->UfsHcPlat.SkipLinkStartup
    ));

  //
  // Self-register the HCI MMIO window with the GCD.
  //
  Status = UfsHcRegisterMmioWindow (
             (EFI_PHYSICAL_ADDRESS)Private->BaseConfig.UfsHciBase,
             ControllerIndex
             );
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

/**
  Entry point for the UFS DXE driver.

  For each UFS controller, resolves its configuration, installs the UFS Host
  Controller Platform Protocol on a new handle and registers the UFSHCI MMIO
  window on that same handle as a non-discoverable UFS MMIO device. UfsMemHcDxe
  binds to the device and produces EDKII_UFS_HOST_CONTROLLER_PROTOCOL on the
  handle, so the ControllerHandle that UfsPassThruDxe passes to the platform
  callback is the handle that carries the platform protocol.

  @param[in] ImageHandle       The firmware allocated handle for the EFI image.
  @param[in] SystemTable       Pointer to the EFI System Table.

  @retval EFI_SUCCESS             At least one controller was registered.
  @retval EFI_NOT_FOUND           No UFS controllers are defined.
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
  EFI_STATUS                Status;
  QCOM_UFS_HC_PRIVATE_DATA  *Private;
  UINTN                     ControllerIndex;
  UINTN                     SuccessCount;

  Status       = EFI_DEVICE_ERROR;
  SuccessCount = 0;

  if (UfsNumControllers == 0) {
    DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] No UFS controllers defined (UfsNumControllers == 0)\n"));
    return EFI_NOT_FOUND;
  }

  for (ControllerIndex = 0; ControllerIndex < UfsNumControllers; ControllerIndex++) {
    Private = AllocateCopyPool (sizeof (QCOM_UFS_HC_PRIVATE_DATA), &mUfsHcTemplate);
    if (Private == NULL) {
      DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] Controller[%u]: AllocateCopyPool failed\n", (UINT32)ControllerIndex));
      Status = EFI_OUT_OF_RESOURCES;
      continue;
    }

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
