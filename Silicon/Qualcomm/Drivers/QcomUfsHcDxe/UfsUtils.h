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
// Standard SCSI INQUIRY response fields (SPC-4, 6.6.2): T10 vendor
// identification, product identification and product revision level.
//
#define UFS_INQUIRY_VENDOR_ID_OFFSET   8
#define UFS_INQUIRY_VENDOR_ID_BYTES    8
#define UFS_INQUIRY_PRODUCT_ID_OFFSET  16
#define UFS_INQUIRY_PRODUCT_ID_BYTES   16
#define UFS_INQUIRY_REV_LEVEL_OFFSET   32
#define UFS_INQUIRY_REV_LEVEL_BYTES    4
#define UFS_INQUIRY_MIN_RESPONSE_LEN   (UFS_INQUIRY_REV_LEVEL_OFFSET + UFS_INQUIRY_REV_LEVEL_BYTES)

///
/// Interval between register samples in the polling helpers, in microseconds.
///
#define UFS_POLL_INTERVAL_US  10

/**
  Read a 32-bit UFSHCI register through the host controller protocol, which
  bounds-checks the access against the controller's register window.

  @param[in]   UfsHc   The UFS host controller protocol.
  @param[in]   Offset  The register offset in the UFSHCI register window.
  @param[out]  Value   The value read.

  @retval EFI_SUCCESS            The register was read.
  @retval EFI_INVALID_PARAMETER  UfsHc or Value is NULL.
  @retval other                  The host controller protocol rejected the access.

**/
EFI_STATUS
UfsHciRead32 (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN  UINTN                               Offset,
  OUT UINT32                              *Value
  );

/**
  Write a 32-bit UFSHCI register through the host controller protocol.

  @param[in]  UfsHc   The UFS host controller protocol.
  @param[in]  Offset  The register offset in the UFSHCI register window.
  @param[in]  Value   The value to write.

  @retval EFI_SUCCESS            The register was written.
  @retval EFI_INVALID_PARAMETER  UfsHc is NULL.
  @retval other                  The host controller protocol rejected the access.

**/
EFI_STATUS
UfsHciWrite32 (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN UINTN                               Offset,
  IN UINT32                              Value
  );

/**
  Wait until (UFSHCI register & Mask) == Expected.

  The register is sampled every UFS_POLL_INTERVAL_US microseconds and the
  remaining budget is reduced by that interval, so TimeoutUs is a lower
  bound on the time waited (register access time comes on top).

  @param[in]   UfsHc      The UFS host controller protocol.
  @param[in]   Offset     The register offset in the UFSHCI register window.
  @param[in]   Mask       The bits to test.
  @param[in]   Expected   The value the masked bits must reach.
  @param[in]   TimeoutUs  The time to wait, in microseconds. Must not be 0.
  @param[out]  LastValue  Optional; the last value read.

  @retval EFI_SUCCESS            The bits reached the expected value.
  @retval EFI_TIMEOUT            They did not within TimeoutUs.
  @retval EFI_INVALID_PARAMETER  UfsHc is NULL or TimeoutUs is 0.
  @retval other                  A register read failed.

**/
EFI_STATUS
UfsHciPoll32 (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN  UINTN                               Offset,
  IN  UINT32                              Mask,
  IN  UINT32                              Expected,
  IN  UINT32                              TimeoutUs,
  OUT UINT32                              *LastValue OPTIONAL
  );

/**
  Wait until (MmioRead32 (Address) & Mask) == Expected.

  For registers outside the UFSHCI register window (the PHY and vendor
  registers relative to UfsMemBase), which the host controller protocol does
  not cover. Same timing rules as UfsHciPoll32().

  @param[in]   Address    The register address.
  @param[in]   Mask       The bits to test.
  @param[in]   Expected   The value the masked bits must reach.
  @param[in]   TimeoutUs  The time to wait, in microseconds. Must not be 0.
  @param[out]  LastValue  Optional; the last value read.

  @retval EFI_SUCCESS            The bits reached the expected value.
  @retval EFI_TIMEOUT            They did not within TimeoutUs.
  @retval EFI_INVALID_PARAMETER  TimeoutUs is 0.

**/
EFI_STATUS
UfsMmioPoll32 (
  IN  UINTN   Address,
  IN  UINT32  Mask,
  IN  UINT32  Expected,
  IN  UINT32  TimeoutUs,
  OUT UINT32  *LastValue OPTIONAL
  );

/**
  Make sure a range of physical address space is described in the GCD as
  uncached memory-mapped I/O.

  Every GCD descriptor overlapping the range is examined: a part that is not
  in the GCD yet is added as MMIO, a part already described as MMIO is kept,
  and any other memory type fails. Each part is then set uncached while its
  existing non-cache attributes (for example EFI_MEMORY_XP) are preserved.

  The CPU Architectural Protocol must be installed; callers that run from a
  driver entry point declare it in their [Depex].

  @param[in]  Base  The start of the range.
  @param[in]  Size  The size of the range in bytes.

  @retval EFI_SUCCESS            The range is uncached MMIO.
  @retval EFI_INVALID_PARAMETER  Size is 0 or the range wraps.
  @retval EFI_ACCESS_DENIED      Part of the range is described as something
                                 other than MMIO (for example system memory).
  @retval other                  A GCD service failed.

**/
EFI_STATUS
UfsRegisterMmioRange (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  );

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
  @param[in]  InquiryRawSize    Size of InquiryRaw in bytes.
  @param[out] InquiryRaw        Raw data buffer returned by inquiry command, Ignored if NULL.
  @param[out] Vendor            Buffer of at least UFS_INQUIRY_VENDOR_ID_BYTES + 1 bytes
                                that receives the NUL-terminated vendor identification.
                                Ignored if NULL.
  @param[out] Product           Buffer of at least UFS_INQUIRY_PRODUCT_ID_BYTES + 1 bytes
                                that receives the NUL-terminated product identification.
                                Ignored if NULL.
  @param[out] Revision          Buffer of at least UFS_INQUIRY_REV_LEVEL_BYTES + 1 bytes
                                that receives the NUL-terminated product revision.
                                Ignored if NULL.

  @retval EFI_SUCCESS           Inquiry data read successfully.
  @retval EFI_INVALID_PARAMETER PassThru, PassThru->Mode or Target is NULL.
  @retval EFI_UNSUPPORTED       The pass-thru IoAlign cannot be met by a pool buffer.
  @retval EFI_OUT_OF_RESOURCES  The response buffer could not be allocated.
  @retval EFI_DEVICE_ERROR      The command completed with a non-GOOD status, or
                                returned a response too short to hold the fields.
  @retval other                 Status returned by PassThru().

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
  Performs a hardware reset of the UFS device.

  Drives the device reset (RST_n) line low and back high through the
  device reset register. A ResetRegister of 0 means the SoC has no such
  register (see UFS_REGISTER_CONFIG); the reset is then skipped.

  @param[in]  Config            Pointer to the UFS configuration data.

  @retval EFI_SUCCESS           The device was reset, or there is no reset register.
  @retval EFI_INVALID_PARAMETER Config is NULL.

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
  @retval EFI_DEVICE_ERROR      The shared PHY register did not read back as enabled.

**/
EFI_STATUS
EFIAPI
UfsEnableSharedPhy (
  IN UFS_CONFIG_DATA  *Config
  );
