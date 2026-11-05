/** @file
  Declarations for UFS utility functions.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS  - Universal Flash Storage
  - SCSI - Small Computer System Interface

**/

#pragma once

#include <Uefi.h>
#include <IndustryStandard/Scsi.h>
#include <IndustryStandard/UfsHci.h>
#include "UfsCore.h"
#include <Protocol/ScsiPassThruExt.h>

//
// UFS Utility Functions
//

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
  );

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
  );

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
  );

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
  @param[out] Vendor            Buffer to store the vendor identification string, Ignored if NULL.
  @param[out] Product           Buffer to store the product identification string, Ignored if NULL.
  @param[out] Revision          Buffer to store the product revision string, Ignored if NULL.

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
  );

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
  );

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
  );

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
  );

