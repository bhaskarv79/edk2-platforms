/** @file
  Declarations for the EDKII UFS Host Controller Protocol implementation.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "QcomUfsHcDxe.h"

///
/// Minimal map context stored in the Mapping cookie so Unmap() can perform
/// the matching post-transfer cache operation without any extra allocations.
///
typedef struct {
  VOID                                  *HostAddress;
  UINTN                                 NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation;
} QCOM_UFS_MAP_INFO;

//
// UFS Host Controller Protocol Implementation Functions
//

/**
  Get the MMIO base of the UFS host controller.

  @param[in]   This             A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[out]  MmioBar          The MMIO base address of UFS host controller.

  @retval EFI_SUCCESS           The operation succeeds.
  @retval others                The operation fails.
**/
EFI_STATUS
EFIAPI
UfsHcGetMmioBar (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT    UINTN                               *MmioBar
  );

/**
  Provides the UFS controller-specific addresses needed to access system memory.

  @param  This                  A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Operation             Indicates if the bus master is going to read or write to system memory.
  @param  HostAddress           The system memory address to map to the UFS controller.
  @param  NumberOfBytes         On input the number of bytes to map. On output the number of bytes
                                that were mapped.
  @param  DeviceAddress         The resulting map address for the bus master UFS controller to use to
                                access the hosts HostAddress.
  @param  Mapping               A resulting value to pass to Unmap().

  @retval EFI_SUCCESS           The range was mapped for the returned NumberOfBytes.
  @retval EFI_UNSUPPORTED       The HostAddress cannot be mapped as a common buffer.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  The request could not be completed due to a lack of resources.
  @retval EFI_DEVICE_ERROR      The system hardware could not map the requested address.

**/
EFI_STATUS
EFIAPI
UfsHcMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  );

/**
  Completes the Map() operation and releases any corresponding resources.

  @param  This                  A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Mapping               The mapping value returned from Map().

  @retval EFI_SUCCESS           The range was unmapped.
  @retval EFI_DEVICE_ERROR      The data was not committed to the target system memory.

**/
EFI_STATUS
EFIAPI
UfsHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  );

/**
  Allocates pages that are suitable for an EfiUfsHcOperationBusMasterCommonBuffer
  mapping.

  @param  This                  A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Type                  This parameter is not used and must be ignored.
  @param  MemoryType            The type of memory to allocate, EfiBootServicesData or
                                EfiRuntimeServicesData.
  @param  Pages                 The number of pages to allocate.
  @param  HostAddress           A pointer to store the base system memory address of the
                                allocated range.
  @param  Attributes            The requested bit mask of attributes for the allocated range.

  @retval EFI_SUCCESS           The requested memory pages were allocated.
  @retval EFI_UNSUPPORTED       Attributes is unsupported. The only legal attribute bits are
                                MEMORY_WRITE_COMBINE and MEMORY_CACHED.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  The memory pages could not be allocated.

**/
EFI_STATUS
EFIAPI
UfsHcAllocateBuffer (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE                   Type,
  IN     EFI_MEMORY_TYPE                     MemoryType,
  IN     UINTN                               Pages,
  OUT    VOID                                **HostAddress,
  IN     UINT64                              Attributes
  );

/**
  Frees memory that was allocated with AllocateBuffer().

  @param  This                  A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Pages                 The number of pages to free.
  @param  HostAddress           The base system memory address of the allocated range.

  @retval EFI_SUCCESS           The requested memory pages were freed.
  @retval EFI_INVALID_PARAMETER The memory range specified by HostAddress and Pages
                                was not allocated with AllocateBuffer().

**/
EFI_STATUS
EFIAPI
UfsHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  );

/**
  Flushes all posted write transactions from the UFS bus to attached UFS device.

  @param  This                  A pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.

  @retval EFI_SUCCESS           The posted write transactions were flushed from the UFS bus
                                to attached UFS device.
  @retval EFI_DEVICE_ERROR      The posted write transactions were not flushed from the UFS
                                bus to attached UFS device due to a hardware error.

**/
EFI_STATUS
EFIAPI
UfsHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  );

/**
  Enable a UFS bus driver to access UFS MMIO registers in the UFS Host Controller memory space.

  @param  This                  A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Width                 Signifies the width of the memory operations.
  @param  Offset                The offset within the UFS Host Controller MMIO space to start the
                                memory operation.
  @param  Count                 The number of memory operations to perform.
  @param  Buffer                For read operations, the destination buffer to store the results.
                                For write operations, the source buffer to write data from.

  @retval EFI_SUCCESS           The data was read from or written to the UFS host controller.
  @retval EFI_UNSUPPORTED       The address range specified by Offset, Width, and Count is not
                                valid for the UFS Host Controller memory space.
  @retval EFI_OUT_OF_RESOURCES  The request could not be completed due to a lack of resources.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsHcMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

/**
  Enable a UFS bus driver to access UFS MMIO registers in the UFS Host Controller memory space.

  @param  This                  A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Width                 Signifies the width of the memory operations.
  @param  Offset                The offset within the UFS Host Controller MMIO space to start the
                                memory operation.
  @param  Count                 The number of memory operations to perform.
  @param  Buffer                Source buffer of values to write to the MMIO registers.

  @retval EFI_SUCCESS           The data was read from or written to the UFS host controller.
  @retval EFI_UNSUPPORTED       The address range specified by Offset, Width, and Count is not
                                valid for the UFS Host Controller memory space.
  @retval EFI_OUT_OF_RESOURCES  The request could not be completed due to a lack of resources.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsHcMmioWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

