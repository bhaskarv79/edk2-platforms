/** @file
  Qcom UFS Host Controller DXE driver entry point and binding protocol implementation.

  Implements the EFI Driver Binding Protocol to manage UFS host controller
  devices and installs the UFS Host Controller and Platform protocols.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "QcomUfsHcDxe.h"
#include "UfsGetConfig.h"
#include "UfsHcPlatform.h"
#include "UfsHostController.h"

// Forward declaration — defined after QcomUfsHcDriverEntry.
STATIC EFI_STATUS InstallControllerDevicePath (IN EFI_HANDLE Handle, IN UINTN ControllerIndex);

///
/// Template for UFS Host Controller private data structure
/// This template is used to initialize new controller instances
///
/// This is a read-only copy source for AllocateCopyPool() in
/// QcomUfsHcDriverEntry(). No locking is required: DXE dispatch is
/// single-threaded and the template is never written after module load.
///
STATIC QCOM_UFS_HC_PRIVATE_DATA  gUfsHcTemplate = {
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
  {                                                 ///< UFS Host Controller Protocol instance
    UfsHcGetMmioBar,                                ///< Get MMIO base address function
    UfsHcAllocateBuffer,                            ///< Allocate DMA buffer function
    UfsHcFreeBuffer,                                ///< Free DMA buffer function
    UfsHcMap,                                       ///< Map memory for DMA function
    UfsHcUnmap,                                     ///< Unmap DMA memory function
    UfsHcFlush,                                     ///< Flush posted writes function
    UfsHcMmioRead,                                  ///< MMIO read function
    UfsHcMmioWrite                                  ///< MMIO write function
  },
  {                                       ///< EDKII_UFS_MEM_HC_DEVICE_PROTOCOL
    0,                                    ///< HciMmioBase (filled from BaseConfig.UfsHciBase)
    0,                                    ///< HciMmioLength
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
  -1,                                               ///< UFS Controller Id -- sentinel; overwritten per-controller in the loop
  NULL,                                             ///< UFS Controller Context
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
        "[QcomUfsHcDxe] Controller[%d]: AddMemorySpace 0x%lx - %r (region was confirmed absent)\n",
        ControllerId,
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
      "[QcomUfsHcDxe] Controller[%d]: SetMemorySpaceAttributes 0x%lx - %r (HCI window may remain cacheable)\n",
      ControllerId,
      HciBase,
      Status
      ));
  } else {
    DEBUG ((DEBUG_INFO, "[QcomUfsHcDxe] Controller[%d]: GCD window 0x%lx registered\n", ControllerId, HciBase));
  }

  return Status;
}

/**
  Logs a controller-init failure and frees its private data.

  Shared tail for the QcomUfsHcDriverEntry() per-controller failure paths that
  have not yet installed any protocol on Private->Handle, so a plain FreePool
  is sufficient -- no protocol rollback is needed.

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
    "[QcomUfsHcDxe] Controller[%d]: %a - %r\n",
    ControllerIndex,
    Step,
    Status
    ));
  FreePool (Private);
}

/**
  Resolves BaseConfig, applies the mandatory PreHce/PostLinkUp override, and
  self-registers the controller's HCI MMIO window and device protocol.

  Extracted from QcomUfsHcDriverEntry() so the entry point's per-controller loop
  reads as a sequence of named steps rather than inlining config resolution,
  flag overrides, GCD registration, and device-protocol population together.

  @param[in,out] Private          Private data for this controller; BaseConfig,
                                   UfsHcPlat, and DevProto are populated on success.
  @param[in]     ControllerIndex  Controller index, used for diagnostic messages.

  @retval EFI_SUCCESS   Configuration resolved and MMIO window registered.
  @return               Any error returned by UfsGetBaseConfig().

**/
STATIC
EFI_STATUS
UfsHcInitControllerConfig (
  IN OUT QCOM_UFS_HC_PRIVATE_DATA  *Private,
  IN     UINTN                     ControllerIndex
  )
{
  EFI_STATUS  Status;
  BOOLEAN     RawPreHceEnable;
  BOOLEAN     RawPostLinkUpEnable;

  Status = UfsGetBaseConfig (
             Private->ControllerId,
             &Private->BaseConfig
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RawPreHceEnable     = (BOOLEAN)Private->BaseConfig.PreHceEnable;
  RawPostLinkUpEnable = (BOOLEAN)Private->BaseConfig.PostLinkUpEnable;

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%d]: raw PCD/DT values: PreHceEnable=%d PostLinkUpEnable=%d (will be overridden to TRUE)\n",
    ControllerIndex,
    RawPreHceEnable,
    RawPostLinkUpEnable
    ));

  //
  // XBL no longer performs UFS PHY/link bring-up, so UEFI must run the
  // PreHce (PHY/clock/HCE bring-up) and PostLinkUp (gear/lane/power-mode
  // negotiation) platform-callback phases unconditionally, regardless of
  // what DT/PCD BaseConfig specifies.
  //
  Private->BaseConfig.PreHceEnable     = TRUE;
  Private->BaseConfig.PostLinkUpEnable = TRUE;

  //
  // Copy the PCD/DT-resolved platform-protocol behavior flags from
  // BaseConfig into UfsHcPlat now that UfsGetBaseConfig() has populated
  // them -- these are no longer compile-time literals in gUfsHcTemplate.
  //
  Private->UfsHcPlat.RefClkFreq      = (EDKII_UFS_CARD_REF_CLK_FREQ_ATTRIBUTE)Private->BaseConfig.RefClkFreq;
  Private->UfsHcPlat.SkipHceReenable = (BOOLEAN)Private->BaseConfig.SkipHceReenable;
  Private->UfsHcPlat.SkipLinkStartup = (BOOLEAN)Private->BaseConfig.SkipLinkStartup;

  DEBUG ((
    DEBUG_INFO,
    "[UfsHc] Controller %u: RefClkFreq=%u SkipHceReenable=%u SkipLinkStartup=%u\n",
    ControllerIndex,
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

  //
  // Fill the device protocol from the resolved base config so UfsMemHcDxe
  // can call GetUfsHcMmioBar() without any platform knowledge.
  //
  Private->DevProto.HciMmioBase   = Private->BaseConfig.UfsHciBase;
  Private->DevProto.HciMmioLength = UFS_HC_MMIO_WINDOW_SIZE;

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] Controller[%d]: MemBase=0x%lx HciBase=0x%lx\n",
    ControllerIndex,
    Private->BaseConfig.UfsMemBase,
    Private->BaseConfig.UfsHciBase
    ));

  return EFI_SUCCESS;
}

/**
  Entry point for the UFS DXE driver.

  This function initializes the UFS Host Controller driver. It allocates private
  data structures, installs required protocols (UFS HC Platform Protocol and
  UFS Host Controller Protocol), and sets up the device path for the controller.

  @param[in] ImageHandle       The firmware allocated handle for the EFI image.
  @param[in] SystemTable       Pointer to the EFI System Table.

  @retval EFI_SUCCESS             The driver entry point executed successfully.
  @retval EFI_OUT_OF_RESOURCES    Insufficient memory to initialize the controller.
  @retval EFI_DEVICE_ERROR        Failed to install protocols.

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
  BOOLEAN                   AnySuccess;
  BOOLEAN                   LegacyHcEn;

  Status       = EFI_DEVICE_ERROR;
  Private      = NULL;
  SuccessCount = 0;
  AnySuccess   = FALSE;

  //
  // Capture once: FixedPcdGetBool expands to a compile-time constant for
  // FIXED PCDs, but capturing it here avoids two textually separate
  // evaluations that would diverge if PcdUfs0LegacyHcProtocolEn were ever
  // changed to a dynamic PCD. Note: this PCD is named 'Ufs0' but controls
  // legacy protocol installation for all controllers in the loop.
  //
  LegacyHcEn = FixedPcdGetBool (PcdUfs0LegacyHcProtocolEn);

  if (UfsNumControllers == 0) {
    DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] No UFS controllers defined (UfsNumControllers == 0)\n"));
    return EFI_NOT_FOUND;
  }

  for (ControllerIndex = 0; ControllerIndex < UfsNumControllers; ControllerIndex++) {
    //
    // Allocate memory for the UFS Host Controller private data
    //
    Private = AllocateCopyPool (sizeof (QCOM_UFS_HC_PRIVATE_DATA), &gUfsHcTemplate);
    if (Private == NULL) {
      DEBUG ((DEBUG_ERROR, "[QcomUfsHcDxe] Controller[%d]: AllocateCopyPool failed\n", ControllerIndex));
      Status = EFI_OUT_OF_RESOURCES;
      continue;
    }

    Private->Handle       = NULL;
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
                    &gEdkiiUfsMemHcDeviceProtocolGuid,
                    &Private->DevProto,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      UfsHcAbandonController (ControllerIndex, "InstallMultipleProtocols", Status, Private);
      continue;
    }

    if (LegacyHcEn) {
      Status = gBS->InstallProtocolInterface (
                      &Private->Handle,
                      &gEdkiiUfsHostControllerProtocolGuid,
                      EFI_NATIVE_INTERFACE,
                      &Private->UfsHc
                      );
      if (EFI_ERROR (Status)) {
        gBS->UninstallMultipleProtocolInterfaces (
               Private->Handle,
               &gEdkiiUfsHcPlatformProtocolGuid,
               &Private->UfsHcPlat,
               &gEdkiiUfsMemHcDeviceProtocolGuid,
               &Private->DevProto,
               NULL
               );
        UfsHcAbandonController (ControllerIndex, "InstallLegacyUfsHc", Status, Private);
        continue;
      }
    }

    //
    // Install the controller device path
    //
    Status = InstallControllerDevicePath (Private->Handle, ControllerIndex);
    if (EFI_ERROR (Status)) {
      //
      // Private->UfsHcPlat and Private->DevProto are already live as
      // installed protocol interfaces on Private->Handle -- roll those
      // back before freeing Private, or the handle is left with
      // protocol pointers into freed memory.
      //
      if (LegacyHcEn) {
        gBS->UninstallProtocolInterface (
               Private->Handle,
               &gEdkiiUfsHostControllerProtocolGuid,
               &Private->UfsHc
               );
      }

      gBS->UninstallMultipleProtocolInterfaces (
             Private->Handle,
             &gEdkiiUfsHcPlatformProtocolGuid,
             &Private->UfsHcPlat,
             &gEdkiiUfsMemHcDeviceProtocolGuid,
             &Private->DevProto,
             NULL
             );
      UfsHcAbandonController (ControllerIndex, "InstallDevicePath", Status, Private);
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "[QcomUfsHcDxe] Controller[%d]: ready on handle %p\n",
      ControllerIndex,
      Private->Handle
      ));
    AnySuccess = TRUE;
    SuccessCount++;
  }

  DEBUG ((
    DEBUG_INFO,
    "[QcomUfsHcDxe] %u/%u controllers initialized\n",
    SuccessCount,
    (UINTN)UfsNumControllers
    ));

  return AnySuccess ? EFI_SUCCESS : Status;
}

/**
  Installs the device path protocol for the UFS controller.

  This function creates and installs a controller device path for the UFS
  host controller handle. The device path includes a hardware controller
  node to uniquely identify the controller in the system.

  Ownership of the DevicePath allocation:
  - On EFI_SUCCESS, ownership of the DevicePath buffer transfers to the
    firmware. The caller must NOT free it. Any consumer that later calls
    HandleProtocol(gEfiDevicePathProtocolGuid) receives a pointer into
    this buffer and must not free it either.
  - On any error, this function frees the DevicePath buffer itself before
    returning. The caller has no cleanup to do.
  - If the caller later needs to remove this handle, it must:
      1. Call UninstallProtocolInterface(gEfiDevicePathProtocolGuid, DevicePath)
         to retrieve the DevicePath pointer.
      2. FreePool(DevicePath) to release the allocation.

  @param[in] Handle               Handle of the UFS controller.
  @param[in] ControllerIndex      Index of the UFS controller instance.

  @retval EFI_SUCCESS             Device path installed successfully.
  @retval EFI_OUT_OF_RESOURCES    Failed to allocate memory for device path.

**/
STATIC
EFI_STATUS
InstallControllerDevicePath (
  IN EFI_HANDLE  Handle,
  IN UINTN       ControllerIndex
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CONTROLLER_DEVICE_PATH    *ControllerDevicePathNode;

  //
  // Allocate memory for the controller device path node
  //
  ControllerDevicePathNode = AllocateZeroPool (sizeof (CONTROLLER_DEVICE_PATH));
  if (ControllerDevicePathNode == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Initialize the controller device path node
  //
  ControllerDevicePathNode->Header.Type    = HARDWARE_DEVICE_PATH;
  ControllerDevicePathNode->Header.SubType = HW_CONTROLLER_DP;
  SetDevicePathNodeLength (&ControllerDevicePathNode->Header, sizeof (CONTROLLER_DEVICE_PATH));
  ControllerDevicePathNode->ControllerNumber = ControllerIndex;

  //
  // Append the controller node to the existing device path
  //
  DevicePath = AppendDevicePathNode (DevicePathFromHandle (Handle), (EFI_DEVICE_PATH_PROTOCOL *)ControllerDevicePathNode);

  //
  // Free allocated memory
  //
  FreePool (ControllerDevicePathNode);

  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Install the device path protocol on the handle
  //
  Status = gBS->InstallProtocolInterface (&Handle, &gEfiDevicePathProtocolGuid, EFI_NATIVE_INTERFACE, DevicePath);
  if (EFI_ERROR (Status)) {
    FreePool (DevicePath);
  }

  return Status;
}
