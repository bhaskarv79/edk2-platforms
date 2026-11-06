/** @file
  UFS utility function implementations.

  Provides register access helpers, vendor/product lookup, SCSI inquiry,
  string descriptor retrieval, and named event management.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS  - Universal Flash Storage
  - SCSI - Small Computer System Interface

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UfsCore.h"
#include "UfsUtils.h"

///
/// Device reset (RST_n) low time and the settle time after releasing it, in
/// microseconds. Carried over from the original bring-up sequence; not
/// derived from a specification minimum.
///
#define UFS_DEVICE_RESET_PULSE_US   10
#define UFS_DEVICE_RESET_SETTLE_US  10

///
/// SCSI INQUIRY timeout, in 100 ns units (5 seconds).
///
#define UFS_INQUIRY_TIMEOUT  50000000

/**
  Compares a name from the platform list against a SCSI INQUIRY field.

  INQUIRY vendor/product fields are fixed width and padded with spaces, while
  the names in the platform list are plain NUL-terminated strings, so a match
  is the list name followed by nothing but trailing spaces.

  @param[in]  Name          NUL-terminated name from the platform list.
  @param[in]  MaxNameLen    Maximum number of characters to scan in Name.
  @param[in]  Field         NUL-terminated INQUIRY field.

  @retval TRUE   Name matches Field.
  @retval FALSE  Name does not match Field.

**/
STATIC
BOOLEAN
UfsInquiryFieldMatch (
  IN CONST CHAR8  *Name,
  IN UINTN        MaxNameLen,
  IN CONST CHAR8  *Field
  )
{
  UINTN  NameLen;

  NameLen = AsciiStrnLenS (Name, MaxNameLen);
  if ((NameLen == 0) || (NameLen > AsciiStrLen (Field))) {
    return FALSE;
  }

  if (AsciiStrnCmp (Name, Field, NameLen) != 0) {
    return FALSE;
  }

  for (Field += NameLen; *Field != '\0'; Field++) {
    if (*Field != ' ') {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Searches for a specific vendor and product combination in the vendor list.

  This function searches through the provided vendor list to find a matching
  vendor name and product name combination. It returns the indices of the
  matching vendor and product if found.

  @param[in]  VendorList        Pointer to the UFS vendor list to search.
  @param[in]  TargetVendor      Pointer to the target vendor name string.
  @param[in]  TargetProduct     Pointer to the target product name string.
  @param[out] OutVendorIndex    Pointer to store the index of the matching vendor.
  @param[out] OutProductIndex   Pointer to store the index of the matching product.

  @retval EFI_SUCCESS           Vendor and product found successfully.
  @retval EFI_NOT_FOUND         Vendor or product not found in the list.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsFindPart (
  IN  UFS_VENDOR_LIST  *VendorList,
  IN  CHAR8            *TargetVendor,
  IN  CHAR8            *TargetProduct,
  OUT UINTN            *OutVendorIndex,
  OUT UINTN            *OutProductIndex
  )
{
  UINTN             VendorIdx;
  UINTN             ProductIdx;
  UFS_PRODUCT_LIST  *CurrentVendor;
  UINTN             Index;
  CHAR8             *RemainingString;

  if ((VendorList == NULL) || (TargetVendor == NULL) || (TargetProduct == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Iterate through all vendors in the list
  //
  for (VendorIdx = 0; VendorIdx < VendorList->NumVendors; VendorIdx++) {
    ProductIdx    = 0;
    CurrentVendor = &VendorList->List[VendorIdx];

    //
    // Check if vendor name matches the target
    //
    if (UfsInquiryFieldMatch (CurrentVendor->VendorName, UFS_CONFIG_MAX_VENDOR_NAME_LEN, TargetVendor)) {
      Index = 0;

      //
      // Vendor found, now iterate through its products
      //
      while (Index < CurrentVendor->ProductListSize) {
        RemainingString = &CurrentVendor->Products[Index];

        //
        // Check if product name matches the target
        //
        if (UfsInquiryFieldMatch (RemainingString, CurrentVendor->ProductListSize - Index, TargetProduct)) {
          if (OutVendorIndex != NULL) {
            *OutVendorIndex = VendorIdx;
          }

          if (OutProductIndex != NULL) {
            *OutProductIndex = ProductIdx;
          }

          return EFI_SUCCESS;
        }

        ProductIdx++;
        Index += AsciiStrnLenS (RemainingString, CurrentVendor->ProductListSize - Index) + 1;
      }
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Creates and registers an event listener for a named event.

  This function creates an event with the specified notification function and
  registers it to listen for a specific GUID-based event. This is useful for
  responding to system events or protocol installations.

  @param[in]   NotifyTpl         The task priority level for the event notification.
  @param[in]   NotifyFunction    Pointer to the notification function to be called.
  @param[in]   NotifyContext     Pointer to the context data for the notification.
  @param[out]  Event             Pointer to store the created event handle.
  @param[in]   Guid              Pointer to the GUID for the event to listen for.
  @param[out]  Registration      Event registration structure pointer.

  @retval EFI_SUCCESS           Event created and registered successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  Insufficient resources to create the event.

**/
EFI_STATUS
EFIAPI
UfsNamedEventListen (
  IN  EFI_TPL           NotifyTpl,
  IN  EFI_EVENT_NOTIFY  NotifyFunction,
  IN  VOID              *NotifyContext,
  IN  EFI_GUID          *Guid,
  OUT EFI_EVENT         *Event,
  OUT VOID              **Registration
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  ErrorStatus;

  if ((NotifyFunction == NULL) ||
      (Guid == NULL) ||
      (Event == NULL) ||
      (Registration == NULL) ||
      (NotifyTpl > TPL_HIGH_LEVEL))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Create the event with the specified notification function
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  NotifyTpl,
                  NotifyFunction,
                  NotifyContext,
                  Event
                  );

  if (EFI_ERROR (Status)) {
    *Event = NULL;
    return Status;
  }

  *Registration = NULL;
  //
  // Register the event to be notified when the specified protocol is installed
  //
  Status = gBS->RegisterProtocolNotify (
                  Guid,
                  *Event,
                  Registration
                  );
  if (EFI_ERROR (Status)) {
    ErrorStatus = gBS->CloseEvent (*Event);
    if (EFI_ERROR (ErrorStatus)) {
      DEBUG ((DEBUG_ERROR, "[UfsNamedEventListen] Unable to close event %p. Status = %r\n", *Event, ErrorStatus));
    }

    *Event        = NULL;
    *Registration = NULL;
    return Status;
  }

  return Status;
}

/**
  Closes a previously registered named event and clears its registration.

  @param[in,out]  Event         Pointer to the event handle to close.
  @param[in,out]  Registration  Pointer to the registration handle to clear.

  @retval EFI_SUCCESS           Event closed and registration cleared.
  @retval EFI_INVALID_PARAMETER Event is NULL or points to a NULL handle.
**/
EFI_STATUS
EFIAPI
UfsNamedEventDiscard (
  IN OUT EFI_EVENT  *Event,
  IN OUT VOID       **Registration OPTIONAL
  )
{
  EFI_STATUS  Status;

  if ((Event == NULL) || (*Event == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Closing the event removes the protocol notification registration
  //
  Status = gBS->CloseEvent (*Event);

  if (!EFI_ERROR (Status)) {
    *Event = NULL;
    if (Registration != NULL) {
      *Registration = NULL;
    }
  }

  return Status;
}

/**
  Copy one fixed-width field out of an INQUIRY response and NUL-terminate it.

  @param[out]  Dest     Buffer of at least Length + 1 bytes, or NULL to skip.
  @param[in]   Inquiry  The INQUIRY response.
  @param[in]   Offset   The field's offset in the response.
  @param[in]   Length   The field's length in bytes.

**/
STATIC
VOID
UfsCopyInquiryField (
  OUT CHAR8        *Dest     OPTIONAL,
  IN  CONST UINT8  *Inquiry,
  IN  UINTN        Offset,
  IN  UINTN        Length
  )
{
  if (Dest == NULL) {
    return;
  }

  CopyMem (Dest, &Inquiry[Offset], Length);
  Dest[Length] = '\0';
  DEBUG ((DEBUG_VERBOSE, "[UfsUtils] INQUIRY field at %u: %a\n", (UINT32)Offset, Dest));
}

/**
  Reads device inquiry data from a UFS device via SCSI Pass Through.

  See UfsUtils.h for the parameter and return value contract.

**/
EFI_STATUS
EFIAPI
UfsReadInquiry (
  IN  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *PassThru,
  IN  UINT8                            *Target,
  IN  UINT64                           Lun,
  IN  UINTN                            InquiryRawSize,
  OUT UINT8                            *InquiryRaw        OPTIONAL,
  OUT CHAR8                            *Vendor            OPTIONAL,
  OUT CHAR8                            *Product           OPTIONAL,
  OUT CHAR8                            *Revision          OPTIONAL
  )
{
  EFI_STATUS                                  Status;
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  Packet;
  UINT8                                       Cdb[6];
  UINT8                                       *InquiryData;
  UINT8                                       SenseData[18];
  UINT8                                       TargetId[TARGET_MAX_BYTES];

  if ((PassThru == NULL) || (PassThru->Mode == NULL) || (Target == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The data buffer must satisfy the pass-thru Mode->IoAlign (UfsPassThruDxe
  // reports sizeof (UINTN)), which a UINT8 stack array does not guarantee.
  // Pool allocations are 8-byte aligned.
  //
  if ((PassThru->Mode->IoAlign > sizeof (UINT64)) ||
      ((PassThru->Mode->IoAlign & (PassThru->Mode->IoAlign - 1)) != 0))
  {
    return EFI_UNSUPPORTED;
  }

  InquiryData = AllocateZeroPool (UFS_INQUIRY_RESPONSE_LEN);
  if (InquiryData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (Cdb, sizeof (Cdb));
  ZeroMem (SenseData, sizeof (SenseData));
  CopyMem (TargetId, Target, TARGET_MAX_BYTES);

  Cdb[0] = EFI_SCSI_OP_INQUIRY;
  Cdb[4] = UFS_INQUIRY_RESPONSE_LEN;      // Allocation length

  Packet.Timeout          = UFS_INQUIRY_TIMEOUT;
  Packet.Cdb              = Cdb;
  Packet.CdbLength        = sizeof (Cdb);
  Packet.InDataBuffer     = InquiryData;
  Packet.InTransferLength = UFS_INQUIRY_RESPONSE_LEN;
  Packet.SenseData        = SenseData;
  Packet.SenseDataLength  = sizeof (SenseData);
  Packet.DataDirection    = EFI_EXT_SCSI_DATA_DIRECTION_READ;

  Status = PassThru->PassThru (PassThru, TargetId, Lun, &Packet, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsUtils] SCSI INQUIRY failed [Status=%r]\n", Status));
    goto Exit;
  }

  //
  // PassThru() returns EFI_SUCCESS for a command that reached the device,
  // even if it completed with CHECK CONDITION. Only a GOOD completion
  // carries a valid response.
  //
  if ((Packet.HostAdapterStatus != EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK) ||
      (Packet.TargetStatus != EFI_EXT_SCSI_STATUS_TARGET_GOOD))
  {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsUtils] SCSI INQUIRY completed with HostAdapterStatus 0x%x TargetStatus 0x%x, sense key 0x%x\n",
      Packet.HostAdapterStatus,
      Packet.TargetStatus,
      (Packet.SenseDataLength > 2) ? (SenseData[2] & 0x0F) : 0
      ));
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }

  if (Packet.InTransferLength < UFS_INQUIRY_MIN_RESPONSE_LEN) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsUtils] INQUIRY response too short: %u bytes (need %u)\n",
      Packet.InTransferLength,
      (UINT32)UFS_INQUIRY_MIN_RESPONSE_LEN
      ));
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }

  UfsCopyInquiryField (Vendor, InquiryData, UFS_INQUIRY_VENDOR_ID_OFFSET, UFS_INQUIRY_VENDOR_ID_BYTES);
  UfsCopyInquiryField (Product, InquiryData, UFS_INQUIRY_PRODUCT_ID_OFFSET, UFS_INQUIRY_PRODUCT_ID_BYTES);
  UfsCopyInquiryField (Revision, InquiryData, UFS_INQUIRY_REV_LEVEL_OFFSET, UFS_INQUIRY_REV_LEVEL_BYTES);

  if (InquiryRaw != NULL) {
    CopyMem (InquiryRaw, InquiryData, MIN (Packet.InTransferLength, InquiryRawSize));
  }

  Status = EFI_SUCCESS;

Exit:
  FreePool (InquiryData);
  return Status;
}

/**
  Performs a hardware reset of the UFS device.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsDeviceReset (
  IN UFS_CONFIG_DATA  *Config
  )
{
  EFI_PHYSICAL_ADDRESS  ResetRegister;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ResetRegister = Config->RegisterConfig.ResetRegister;
  if (ResetRegister == 0) {
    DEBUG ((DEBUG_INFO, "[UfsHc] UfsDeviceReset: no device reset register, reset skipped\n"));
    return EFI_SUCCESS;
  }

  MmioWrite32 (ResetRegister, 0);
  MicroSecondDelay (UFS_DEVICE_RESET_PULSE_US);
  MmioWrite32 (ResetRegister, 1);
  MicroSecondDelay (UFS_DEVICE_RESET_SETTLE_US);

  return EFI_SUCCESS;
}

/**
  Enables the shared PHY for the UFS controller.

  This function writes to the shared PHY enable register if one is configured.
  If no shared PHY register is configured, the function returns EFI_SUCCESS
  without performing any action.

  @param[in]  Config            Pointer to the UFS configuration data.

  @retval EFI_SUCCESS           Shared PHY enabled successfully, or not required.
  @retval EFI_INVALID_PARAMETER Config is NULL.
  @retval EFI_DEVICE_ERROR      The shared PHY register did not read back as enabled.
**/
EFI_STATUS
EFIAPI
UfsEnableSharedPhy (
  IN UFS_CONFIG_DATA  *Config
  )
{
  UINT32               Value;
  UFS_REGISTER_CONFIG  *RegisterConfig;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  RegisterConfig = &Config->RegisterConfig;

  if (RegisterConfig->SharedPhyRegister == 0) {
    //
    // SharedPhyRegister == 0 means this platform has a single (non-shared)
    // PHY -- a normal, expected configuration. Log at INFO, not ERROR.
    //
    DEBUG ((DEBUG_INFO, "[UfsHc] UfsEnableSharedPhy: SharedPhyRegister=0, single PHY, mux skipped\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_VERBOSE,
    "[UfsHc] UfsEnableSharedPhy: MUX reg 0x%Lx before write = 0x%x\n",
    RegisterConfig->SharedPhyRegister,
    MmioRead32 (RegisterConfig->SharedPhyRegister)
    ));

  Value = 1;
  MmioWrite32 (RegisterConfig->SharedPhyRegister, Value);

  Value = MmioRead32 (RegisterConfig->SharedPhyRegister);

  DEBUG ((
    DEBUG_VERBOSE,
    "[UfsHc] UfsEnableSharedPhy: MUX reg 0x%Lx after write = 0x%x (expected 1)\n",
    RegisterConfig->SharedPhyRegister,
    Value
    ));

  if (Value != 1) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] UfsEnableSharedPhy: MUX register readback 0x%x != 1; hardware may not have accepted the write\n",
      Value
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read a 32-bit UFSHCI register through the host controller protocol.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
UfsHciRead32 (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN  UINTN                               Offset,
  OUT UINT32                              *Value
  )
{
  if ((UfsHc == NULL) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return UfsHc->Read (UfsHc, EfiUfsHcWidthUint32, Offset, 1, Value);
}

/**
  Write a 32-bit UFSHCI register through the host controller protocol.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
UfsHciWrite32 (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN UINTN                               Offset,
  IN UINT32                              Value
  )
{
  if (UfsHc == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return UfsHc->Write (UfsHc, EfiUfsHcWidthUint32, Offset, 1, &Value);
}

/**
  Wait until (UFSHCI register & Mask) == Expected.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
UfsHciPoll32 (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN  UINTN                               Offset,
  IN  UINT32                              Mask,
  IN  UINT32                              Expected,
  IN  UINT32                              TimeoutUs,
  OUT UINT32                              *LastValue OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      Value;

  if ((UfsHc == NULL) || (TimeoutUs == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  while (TRUE) {
    Status = UfsHciRead32 (UfsHc, Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (LastValue != NULL) {
      *LastValue = Value;
    }

    if ((Value & Mask) == Expected) {
      return EFI_SUCCESS;
    }

    if (TimeoutUs < UFS_POLL_INTERVAL_US) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (UFS_POLL_INTERVAL_US);
    TimeoutUs -= UFS_POLL_INTERVAL_US;
  }
}

/**
  Wait until (MmioRead32 (Address) & Mask) == Expected.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
UfsMmioPoll32 (
  IN  UINTN   Address,
  IN  UINT32  Mask,
  IN  UINT32  Expected,
  IN  UINT32  TimeoutUs,
  OUT UINT32  *LastValue OPTIONAL
  )
{
  UINT32  Value;

  if (TimeoutUs == 0) {
    return EFI_INVALID_PARAMETER;
  }

  while (TRUE) {
    Value = MmioRead32 (Address);
    if (LastValue != NULL) {
      *LastValue = Value;
    }

    if ((Value & Mask) == Expected) {
      return EFI_SUCCESS;
    }

    if (TimeoutUs < UFS_POLL_INTERVAL_US) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (UFS_POLL_INTERVAL_US);
    TimeoutUs -= UFS_POLL_INTERVAL_US;
  }
}

/**
  Make sure a range of physical address space is described in the GCD as
  uncached memory-mapped I/O.

  See UfsUtils.h for the contract.

**/
EFI_STATUS
UfsRegisterMmioRange (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_PHYSICAL_ADDRESS             End;
  UINT64                           Length;

  if ((Size == 0) || (Base + Size - 1 < Base)) {
    return EFI_INVALID_PARAMETER;
  }

  End = Base + Size;
  for (Address = Base; Address < End; Address += Length) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Length = MIN (End, Desc.BaseAddress + Desc.Length) - Address;

    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Address, Length, EFI_MEMORY_UC);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Desc.Attributes = 0;
    } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc] 0x%Lx..0x%Lx is GCD memory type %d, not MMIO\n",
        Address,
        Address + Length - 1,
        Desc.GcdMemoryType
        ));
      return EFI_ACCESS_DENIED;
    }

    Status = gDS->SetMemorySpaceAttributes (
                    Address,
                    Length,
                    (Desc.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}
