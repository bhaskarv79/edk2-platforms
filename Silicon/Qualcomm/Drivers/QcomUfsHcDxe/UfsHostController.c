/** @file
  EDKII UFS Host Controller Protocol implementation for Qualcomm platforms.

  Implements MMIO access, DMA buffer management, and UFS host controller
  register read/write operations.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/SafeIntLib.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#include "QcomUfsHcDxe.h"
#include "UfsHostController.h"

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
  )
{
  QCOM_UFS_HC_PRIVATE_DATA  *Private;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MmioBar == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private = QCOM_UFS_HC_PRIVATE_DATA_FROM_UFSHC (This);
  if (Private == NULL) {
    return EFI_DEVICE_ERROR;
  }

  *MmioBar = (UINTN)Private->BaseConfig.UfsHciBase;

  return EFI_SUCCESS;
}

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
  )
{
  QCOM_UFS_MAP_INFO  *MapInfo;

  if ((HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHcMap] Invalid parameter\n"));
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((
    DEBUG_VERBOSE,
    "[UfsHcMap] Op=%d Host=0x%p Bytes=0x%x\n",
    Operation,
    HostAddress,
    (UINT32)*NumberOfBytes
    ));

  MapInfo = AllocatePool (sizeof (QCOM_UFS_MAP_INFO));
  if (MapInfo == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHcMap] Out of resources allocating map token\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->HostAddress   = HostAddress;
  MapInfo->NumberOfBytes = *NumberOfBytes;
  MapInfo->Operation     = Operation;

  //
  // For BusMasterRead and CommonBuffer the device will DMA-read from this
  // buffer, so push any dirty cache lines to memory before handing the
  // address to the controller.
  //
  if ((Operation == EdkiiUfsHcOperationBusMasterRead) ||
      (Operation == EdkiiUfsHcOperationBusMasterCommonBuffer))
  {
    WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
  }

  //
  // Identity mapping: assumes a flat physical/device address space with no
  // SMMU/IOMMU translation. The host virtual address equals the device
  // physical address. This implementation is NOT suitable for platforms where
  // a Stage-2 SMMU or IOMMU is enabled at boot time; replace with DmaMap()
  // from DmaLib if SMMU support is required.
  //
  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = MapInfo;

  DEBUG ((
    DEBUG_VERBOSE,
    "[UfsHcMap] Device=0x%Lx Mapping=%p\n",
    *DeviceAddress,
    *Mapping
    ));

  return EFI_SUCCESS;
}

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
  )
{
  QCOM_UFS_MAP_INFO  *MapInfo;

  if (Mapping == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHcUnmap] Mapping is NULL\n"));
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = (QCOM_UFS_MAP_INFO *)Mapping;

  DEBUG ((
    DEBUG_VERBOSE,
    "[UfsHcUnmap] Op=%d Host=0x%p Bytes=0x%x\n",
    MapInfo->Operation,
    MapInfo->HostAddress,
    (UINT32)MapInfo->NumberOfBytes
    ));

  //
  // For BusMasterWrite and CommonBuffer the device has DMA-written to this
  // buffer; invalidate stale cache lines so the CPU sees the new data.
  //
  if ((MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) ||
      (MapInfo->Operation == EdkiiUfsHcOperationBusMasterCommonBuffer))
  {
    InvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->NumberOfBytes);
  }

  FreePool (MapInfo);
  return EFI_SUCCESS;
}

/**
  Allocates pages that are suitable for an EfiUfsHcOperationBusMasterCommonBuffer
  mapping.

  This function allocates memory pages suitable for DMA operations by the UFS
  host controller. The allocated memory can be used for common buffer operations
  where both the host and the UFS controller need to access the same memory.

  @param[in]  This                  Pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Type                  This parameter is not used and must be ignored.
  @param[in]  MemoryType            The type of memory to allocate, EfiBootServicesData or
                                    EfiRuntimeServicesData.
  @param[in]  Pages                 The number of pages to allocate.
  @param[out] HostAddress           Pointer to store the base system memory address of the
                                    allocated range.
  @param[in]  Attributes            The requested bit mask of attributes for the allocated range.

  @retval EFI_SUCCESS               The requested memory pages were allocated.
  @retval EFI_UNSUPPORTED           Attributes is unsupported. The only legal attribute bits are
                                    MEMORY_WRITE_COMBINE and MEMORY_CACHED.
  @retval EFI_INVALID_PARAMETER     One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES      The memory pages could not be allocated.

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
  )
{
  (VOID)Type;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MemoryType != EfiBootServicesData) &&
      (MemoryType != EfiRuntimeServicesData))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Only MEMORY_WRITE_COMBINE and MEMORY_CACHED are legal per the protocol
  // contract. This implementation delegates to DmaAllocateBuffer which
  // produces UC/WC memory suitable for CommonBuffer DMA; reject any caller
  // that requests cache attributes we cannot honour.
  //
  if ((Attributes & ~((UINT64)(EFI_MEMORY_WC | EFI_MEMORY_WB))) != 0) {
    return EFI_UNSUPPORTED;
  }

  return DmaAllocateBuffer (MemoryType, Pages, HostAddress);
}

/**
  Frees memory that was allocated with AllocateBuffer().

  This function frees memory pages that were previously allocated using the
  AllocateBuffer() function. The memory range specified by HostAddress and
  Pages must exactly match a previous AllocateBuffer() call.

  @param[in]  This                  Pointer to the EFI_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Pages                 The number of pages to free.
  @param[in]  HostAddress           The base system memory address of the allocated range.

  @retval EFI_SUCCESS               The requested memory pages were freed.
  @retval EFI_INVALID_PARAMETER     The memory range specified by HostAddress and Pages
                                    was not allocated with AllocateBuffer().

**/
EFI_STATUS
EFIAPI
UfsHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  )
{
  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return DmaFreeBuffer (Pages, HostAddress);
}

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
  )
{
  return EFI_SUCCESS;
}

/**
  Shared MMIO access helper for UfsHcMmioRead and UfsHcMmioWrite.

  Validates parameters, performs an overflow-safe bounds check, then
  dispatches on Width outside the Count loop so that Width is evaluated
  exactly once regardless of Count.

  @param[in]     This    Protocol instance.
  @param[in]     Width   Register width.
  @param[in]     Offset  Byte offset from HCI base.
  @param[in]     Count   Number of register accesses.
  @param[in,out] Buffer  Read destination or write source.
  @param[in]     Read    TRUE for read, FALSE for write.

  @retval EFI_SUCCESS            Operation completed.
  @retval EFI_INVALID_PARAMETER  Parameter validation or bounds check failed.
  @retval EFI_DEVICE_ERROR       Private data recovery failed.
  @retval EFI_UNSUPPORTED        Unsupported Width value.

**/
STATIC
EFI_STATUS
UfsHcMmioAccess (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer,
  IN     BOOLEAN                                   Read
  )
{
  UINTN                     Index;
  UINT64                    AccessEnd;
  UINT64                    IterOffset;
  RETURN_STATUS             SafeStatus;
  QCOM_UFS_HC_PRIVATE_DATA  *Private;

  if ((This == NULL) || (Buffer == NULL) || (Count == 0)) {
    DEBUG ((DEBUG_ERROR, "[UfsHcMmioAccess] Invalid parameter: This=%p Buffer=%p Count=%u\n", This, Buffer, (UINT32)Count));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Overflow-safe bounds check: plain addition can wrap when Offset is near
  // MAX_UINT64, allowing a wrapped value to pass the comparison.
  //
  SafeStatus = SafeUint64Mult ((UINT64)Count, sizeof (UINT32), &AccessEnd);
  if (RETURN_ERROR (SafeStatus)) {
    DEBUG ((DEBUG_ERROR, "[UfsHcMmioAccess] Count=0x%x overflows range check\n", (UINT32)Count));
    return EFI_INVALID_PARAMETER;
  }

  SafeStatus = SafeUint64Add (Offset, AccessEnd, &AccessEnd);
  if (RETURN_ERROR (SafeStatus) || (AccessEnd > UFS_HC_MMIO_WINDOW_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHcMmioAccess] Out of range: Offset=0x%Lx Count=0x%x AccessEnd=0x%Lx Limit=0x%x\n",
      Offset,
      (UINT32)Count,
      AccessEnd,
      (UINT32)UFS_HC_MMIO_WINDOW_SIZE
      ));
    return EFI_INVALID_PARAMETER;
  }

  Private = QCOM_UFS_HC_PRIVATE_DATA_FROM_UFSHC (This);
  if (Private == NULL) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Dispatch on Width outside the Count loop: Width is loop-invariant and
  // should be evaluated once. Each case loops over Count independently.
  //
  switch (Width) {
    case EfiUfsHcWidthUint32:
      for (Index = 0; Index < Count; Index++) {
        //
        // Compute the per-iteration offset in UINT64 to prevent UINTN wrap
        // on 32-bit UEFI builds.
        //
        IterOffset = Offset + (UINT64)Index * sizeof (UINT32);
        if (Read) {
          *((UINT32 *)Buffer + Index) = MmioRead32 ((UINTN)(Private->BaseConfig.UfsHciBase + IterOffset));
        } else {
          MmioWrite32 ((UINTN)(Private->BaseConfig.UfsHciBase + IterOffset), *((UINT32 *)Buffer + Index));
        }
      }

      break;
    default:
      return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

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
  )
{
  return UfsHcMmioAccess (This, Width, Offset, Count, Buffer, TRUE);
}

/**
  Writes UFS host controller MMIO registers.

  @param  This                  A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param  Width                 Signifies the width of the memory operations.
  @param  Offset                The offset within the UFS Host Controller MMIO space to start the
                                memory operation.
  @param  Count                 The number of memory operations to perform.
  @param  Buffer                Source buffer of values to write to the MMIO registers.

  @retval EFI_SUCCESS           The data was written to the UFS host controller.
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
  )
{
  return UfsHcMmioAccess (This, Width, Offset, Count, Buffer, FALSE);
}
