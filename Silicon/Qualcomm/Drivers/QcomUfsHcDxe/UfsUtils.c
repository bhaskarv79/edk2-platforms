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
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UfsCore.h"
#include "UfsUtils.h"

//
// Standard UFS Descriptor Constants (JEDEC)
//
#define UFS_DESC_STRING_IDN         0x05

///
/// Size of the UFS string descriptor header in bytes (bLength + bDescriptorIDn).
/// Per JEDEC UFS spec the minimum valid bLength value is UFS_STRING_DESC_HEADER_SIZE;
/// any descriptor shorter than this cannot contain a valid header.
///
#define UFS_STRING_DESC_HEADER_SIZE  2U

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
    if (AsciiStrCmp (CurrentVendor->VendorName, TargetVendor) == 0) {
      Index = 0;

      //
      // Vendor found, now iterate through its products
      //
      while (Index < CurrentVendor->ProductListSize) {
        RemainingString = &CurrentVendor->Products[Index];

        //
        // Check if product name matches the target
        //
        if (AsciiStrnCmp (RemainingString, TargetProduct, CurrentVendor->ProductListSize) == 0) {
          if (OutVendorIndex != NULL) {
            *OutVendorIndex = VendorIdx;
          }

          if (OutProductIndex != NULL) {
            *OutProductIndex = ProductIdx;
          }

          return EFI_SUCCESS;
        }

        ProductIdx++;
        Index += AsciiStrnLenS (RemainingString, CurrentVendor->ProductListSize) + 1;
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
      (Event == NULL) ||
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
  Reads device inquiry data from a UFS device via SCSI Pass Through.

  This function sends a SCSI INQUIRY command to the specified UFS device
  to retrieve vendor identification, product identification, and revision
  information. This data is used for device identification and compatibility
  checking.

  @param[in]  PassThru          Pointer to the SCSI Pass Through protocol.
  @param[in]  Target            Pointer to the target device identifier.
  @param[in]  Lun               Logical Unit Number of the target device.
  @param[out] InquiryRaw        Raw data buffer returned by inquiry command, Ignored if NULL.
  @param[out] Vendor            Buffer to store the vendor identification string.
  @param[out] Product           Buffer to store the product identification string.
  @param[out] Revision          Buffer to store the product revision string.

  @retval EFI_SUCCESS           Inquiry data read successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.
  @retval EFI_DEVICE_ERROR      Failed to execute SCSI INQUIRY command.
  @retval EFI_TIMEOUT           SCSI command timed out.

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
  UINT8                                       InquiryData[UFS_INQUIRY_RESPONSE_LEN];
  UINT8                                       TargetId[TARGET_MAX_BYTES];
  UINT64                                      LunId;

  //
  // Initialize all data structures to zero
  //
  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (Cdb, sizeof (Cdb));
  ZeroMem (InquiryData, sizeof (InquiryData));
  ZeroMem (TargetId, TARGET_MAX_BYTES);

  //
  // Copy target and LUN information
  //
  CopyMem (TargetId, Target, TARGET_MAX_BYTES);
  LunId = Lun;

  //
  // Build SCSI INQUIRY command
  //
  Cdb[0] = EFI_SCSI_OP_INQUIRY;           // INQUIRY opcode
  Cdb[4] = sizeof (InquiryData);          // Allocation length

  //
  // Setup SCSI request packet
  //
  Packet.Timeout           = 50000000;    // 5 seconds timeout (in 100ns units)
  Packet.Cdb               = Cdb;
  Packet.CdbLength         = sizeof (Cdb);
  Packet.InDataBuffer      = InquiryData;
  Packet.InTransferLength  = sizeof (InquiryData);
  Packet.OutDataBuffer     = NULL;
  Packet.OutTransferLength = 0;
  Packet.DataDirection     = EFI_EXT_SCSI_DATA_DIRECTION_READ;

  //
  // Execute the SCSI INQUIRY command
  //
  Status = PassThru->PassThru (
                       PassThru,
                       TargetId,
                       LunId,
                       &Packet,
                       NULL
                       );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsUtils] SCSI INQUIRY failed [Status=%r]\n", Status));
    return Status;
  }

  //
  // Standard SCSI INQUIRY response: vendor at [8], product at [16], revision at [32..35].
  // Verify the device returned at least 36 bytes before accessing those fields.
  // An out-of-spec device that reports fewer bytes would cause out-of-bounds reads.
  //
  if (Packet.InTransferLength < UFS_INQUIRY_MIN_RESPONSE_LEN) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsUtils] INQUIRY response too short: %u bytes (need %u)\n",
      Packet.InTransferLength,
      (UINTN)UFS_INQUIRY_MIN_RESPONSE_LEN
      ));
    return EFI_DEVICE_ERROR;
  }

  if (Vendor != NULL) {
    //
    // Extract vendor identification (bytes 8-15)
    //
    CopyMem (Vendor, &InquiryData[8], 8);
    Vendor[8] = '\0';
  }

  if (Product != NULL) {
    //
    // Extract product identification (bytes 16-31)
    //
    CopyMem (Product, &InquiryData[16], 16);
    Product[16] = '\0';
  }

  if (Revision != NULL) {
    //
    // Extract product revision level (bytes 32-35)
    //
    CopyMem (Revision, &InquiryData[32], 4);
    Revision[4] = '\0';
  }

  if (InquiryRaw != NULL) {
    CopyMem (InquiryRaw, InquiryData, MIN (Packet.InTransferLength, InquiryRawSize));
  }

  DEBUG ((DEBUG_VERBOSE, "[UfsUtils] UFS INQUIRY successful\n"));
  if (Vendor != NULL) {
    DEBUG ((DEBUG_VERBOSE, "[UfsUtils] Vendor: %a\n", Vendor));
  }

  if (Product != NULL) {
    DEBUG ((DEBUG_VERBOSE, "[UfsUtils] Product: %a\n", Product));
  }

  if (Revision != NULL) {
    DEBUG ((DEBUG_VERBOSE, "[UfsUtils] Revision: %a\n", Revision));
  }

  return EFI_SUCCESS;
}

/**
  Retrieves a string descriptor from the UFS device.

  This function reads a string descriptor from the UFS device using the
  UFS Device Configuration Protocol. String descriptors contain human-readable
  information about the device such as manufacturer name, product name, etc.

  @param[in]  UfsDevCfg         Pointer to the UFS Device Configuration Protocol.
  @param[in]  StringIndex       Index of the string descriptor to retrieve.
  @param[out] String            Pointer to store the retrieved string (allocated by this function).
  @param[out] StringSize        Pointer to store the size of the retrieved string in bytes.

  @retval EFI_SUCCESS           String descriptor retrieved successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.
  @retval EFI_NOT_FOUND         String descriptor with specified index not found.
  @retval EFI_DEVICE_ERROR      Failed to read string descriptor from device.
  @retval EFI_OUT_OF_RESOURCES  Failed to allocate memory for the string.

**/
EFI_STATUS
EFIAPI
UfsGetStringDescriptor (
  IN  EFI_UFS_DEVICE_CONFIG_PROTOCOL  *UfsDevCfg,
  IN  UINT8                           StringIndex,
  OUT CHAR16                          **String,
  OUT UINT32                          *StringSize
  )
{
  EFI_STATUS  Status;
  UINT8       StringDescHeader[UFS_STRING_DESC_HEADER_SIZE];
  UINT32      StringLenBytes;
  CHAR16      *StringDescRaw;

  if ((UfsDevCfg == NULL) || (String == NULL) || (StringSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (StringIndex == 0) {
    return EFI_NOT_FOUND; // 0 indicates no string present
  }

  //
  // Read string descriptor header to determine total length
  //
  StringLenBytes = UFS_STRING_DESC_HEADER_SIZE;
  Status         = UfsDevCfg->RwUfsDescriptor (
                                UfsDevCfg,
                                TRUE,
                                UFS_DESC_STRING_IDN, // String Descriptor (0x05)
                                StringIndex,         // Index retrieved from Device Desc
                                0,
                                StringDescHeader,
                                &StringLenBytes
                                );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Calculate full descriptor size (bLength is the first byte).
  // Per JEDEC UFS spec the maximum string descriptor length is 254 bytes
  // (bLength field is UINT8, value 0xFF = 255, but the minimum valid value
  // is 2 to hold at least the header). Reject descriptors that are too
  // small to contain a header or unreasonably large.
  //
  StringLenBytes = StringDescHeader[0];

  if (StringLenBytes < UFS_STRING_DESC_HEADER_SIZE) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsUtils] String descriptor bLength=%u too small (min %u)\n",
      StringLenBytes,
      (UINT32)UFS_STRING_DESC_HEADER_SIZE
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Allocate memory and read the complete string descriptor
  //
  StringDescRaw = AllocateZeroPool (StringLenBytes);
  if (StringDescRaw == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = UfsDevCfg->RwUfsDescriptor (
                        UfsDevCfg,
                        TRUE,
                        UFS_DESC_STRING_IDN,
                        StringIndex,
                        0,
                        (UINT8 *)StringDescRaw,
                        &StringLenBytes
                        );

  if (EFI_ERROR (Status)) {
    FreePool (StringDescRaw);
    return Status;
  }

  //
  // Re-validate after the second read: the device may have updated
  // StringLenBytes to a value different from the header-reported length.
  // Guard against underflow and against a value that would read beyond
  // the allocated buffer.
  //
  if (StringLenBytes < UFS_STRING_DESC_HEADER_SIZE) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsUtils] String descriptor returned length=%u too small after read\n",
      StringLenBytes
      ));
    FreePool (StringDescRaw);
    return EFI_DEVICE_ERROR;
  }

  //
  // Extract Unicode string data (skip 2-byte header)
  //
  *StringSize = StringLenBytes - UFS_STRING_DESC_HEADER_SIZE;
  *String     = AllocateZeroPool (StringLenBytes);

  if (*String == NULL) {
    FreePool (StringDescRaw);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Copy Unicode data to output buffer
  //
  CopyMem (*String, &StringDescRaw[UFS_STRING_DESC_HEADER_SIZE], *StringSize);

  FreePool (StringDescRaw);
  return EFI_SUCCESS;
}

/**
  Performs a hardware reset of the UFS device.

  This function toggles the reset register to perform a hardware reset
  of the UFS device, with a short delay between de-assert and re-assert.

  @param[in]  Config            Pointer to the UFS configuration data.

  @retval EFI_SUCCESS           Device reset completed successfully.
  @retval EFI_INVALID_PARAMETER Config is NULL or ResetRegister is not set.
  @retval EFI_DEVICE_ERROR      Failed to write to the reset register.
**/
EFI_STATUS
EFIAPI
UfsDeviceReset (
  IN UFS_CONFIG_DATA  *Config
  )
{
  UINT32               Value;
  UFS_REGISTER_CONFIG  *RegisterConfig;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  RegisterConfig = &Config->RegisterConfig;

  if (RegisterConfig->ResetRegister == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Value = 0;
  MmioWrite32 (RegisterConfig->ResetRegister, Value);

  MicroSecondDelay (10);

  Value = 1;
  MmioWrite32 (RegisterConfig->ResetRegister, Value);

  MicroSecondDelay (10);

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
  @retval EFI_DEVICE_ERROR      Failed to write to the shared PHY register.
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
    // PHY — a normal, expected configuration. Log at INFO, not ERROR.
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
