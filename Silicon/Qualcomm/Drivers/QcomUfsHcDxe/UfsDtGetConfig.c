/** @file
  Device tree-based UFS configuration retrieval implementation.

  Provides functions to obtain controller-specific configuration and base
  addresses by parsing the platform device tree (DTB).

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage
  - DTB - Device Tree Blob

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DeviceTreeExtension.h>

#include "UfsCore.h"
#include "UfsDtGetConfig.h"

#define UFS_DT_MAX_NODE_NAME_LENGTH  128

#define DT_ROOT_NODE                 "/soc"
#define UFS_DT_ROOT_NODE_PREFIX      DT_ROOT_NODE "/" "UfsController"
#define UFS_DT_NODE_BASE_CONFIG      "BaseConfig"
#define UFS_DT_NODE_SPEED_CONFIG     "SpeedConfig"
#define UFS_DT_NODE_RESOURCE_CONFIG  "ResourceConfig"
#define UFS_DT_NODE_TIMEOUT_CONFIG   "TimeoutConfig"
#define UFS_DT_NODE_REGISTER_CONFIG  "RegisterConfig"
#define UFS_DT_NODE_PHY_TABLES       "PhyConfig"

///
/// UFS Phy Initialization table names
///
STATIC CONST CHAR8  *CONST  mPhyTableNames[UfsPhyTableTypeMax] = {
  "InitTable",
  "Lane0Table",
  "Lane1Table",
  "EndTable",
  "HsLss"
};

#define UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST  "DeEmphasisVendorList"
#define UFS_DT_NODE_VENDOR_ID                "VendorId"
#define UFS_DT_NODE_MODEL_ID                 "ModelId"

///
/// libfdt "node or property does not exist" (FDT_ERR_NOTFOUND), which
/// DTB_EXTN_PROTOCOL returns negated.
///
#define UFS_DT_FDT_ERR_NOTFOUND  (-1)

//
// The DT nodes below are copied with FdtGetPropValuesOfNode() straight into
// these structures, one format letter per field (b = 1, w = 4, d = 8 bytes).
// A field added, removed or retyped in UfsCore.h must change the matching
// format string; these checks catch a size change at build time, and
// UfsDtReadNodeProps() checks the node's own packed size at run time.
//
STATIC_ASSERT (sizeof (UFS_BASE_CONFIG) == 21, "\"ddbbbbb\" must match UFS_BASE_CONFIG");
STATIC_ASSERT (sizeof (UFS_SPEED_CONFIG) == 7, "\"bbbbbbb\" must match UFS_SPEED_CONFIG");
STATIC_ASSERT (sizeof (UFS_RESOURCE_CONFIG) == 1, "\"b\" must match UFS_RESOURCE_CONFIG");
STATIC_ASSERT (sizeof (UFS_TIMEOUT_CONFIG) == 16, "\"wwww\" must match UFS_TIMEOUT_CONFIG");
STATIC_ASSERT (sizeof (UFS_REGISTER_CONFIG) == 24, "\"ddd\" must match UFS_REGISTER_CONFIG");
STATIC_ASSERT (sizeof (UFS_PHY_TABLE) == 6, "PHY table entries are three UINT16 values");

/**
  Frees all heap allocations made by UfsDtGetConfigData() into Config.

  See UfsDtGetConfig.h for the contract.

**/
VOID
EFIAPI
UfsDtFreeConfigData (
  IN OUT UFS_CONFIG_DATA  *Config
  )
{
  UINTN             Loop;
  UFS_VENDOR_LIST   *VendorList;
  UFS_PRODUCT_LIST  *ProductList;

  if (Config == NULL) {
    return;
  }

  for (Loop = 0; Loop < UfsPhyTableTypeMax; Loop++) {
    if (Config->PhyConfig.PhyTable[Loop].Entries != NULL) {
      FreePool (Config->PhyConfig.PhyTable[Loop].Entries);
      Config->PhyConfig.PhyTable[Loop].Entries = NULL;
    }

    Config->PhyConfig.PhyTable[Loop].NumEntries = 0;
  }

  Config->PhyConfig.RegisterWindowSize = 0;

  VendorList = &Config->DeEmphasisVendorList;
  if (VendorList->List != NULL) {
    for (Loop = 0; Loop < VendorList->NumVendors; Loop++) {
      ProductList = &VendorList->List[Loop];
      if (ProductList->VendorName != NULL) {
        FreePool (ProductList->VendorName);
      }

      if (ProductList->Products != NULL) {
        FreePool (ProductList->Products);
      }
    }

    FreePool (VendorList->List);
    VendorList->List = NULL;
  }

  VendorList->NumVendors = 0;
}

/**
  Converts a DTB_EXTN_PROTOCOL return code to an EFI_STATUS.

  DTB_EXTN_PROTOCOL services return DTB_ERR_NOERROR on success, and either a
  DTB_ERR_* code or a negative libfdt code on failure. Neither maps onto
  EFI_ERROR(), so every call result must go through this helper.

  @param[in] DtbRet  Value returned by a DTB_EXTN_PROTOCOL service.

  @retval EFI_SUCCESS       DtbRet is DTB_ERR_NOERROR.
  @retval EFI_NOT_FOUND     The node or property does not exist.
  @retval EFI_DEVICE_ERROR  Any other failure, including a malformed DTB.
**/
STATIC
EFI_STATUS
UfsDtStatus (
  IN INT32  DtbRet
  )
{
  if (DtbRet == DTB_ERR_NOERROR) {
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "[UfsHc][DT] DtbExtn error %d\n", DtbRet));

  if (DtbRet == UFS_DT_FDT_ERR_NOTFOUND) {
    return EFI_NOT_FOUND;
  }

  return EFI_DEVICE_ERROR;
}

/**
  Builds the DT node path for a controller sub-node and retrieves its handle.

  @param[in]  DtbExtn      DTB extension protocol instance.
  @param[in]  ControllerId UFS controller identifier.
  @param[in]  NodeSuffix   Sub-node name suffix (e.g. "SpeedConfig").
  @param[out] Node         Receives the node handle on success.

  @retval EFI_SUCCESS      Node handle retrieved successfully.
  @retval EFI_DEVICE_ERROR Node path construction failed.
  @retval other            Status from FdtGetNodeHandle.

**/
STATIC
EFI_STATUS
UfsDtBuildAndGetNode (
  IN  DTB_EXTN_PROTOCOL     *DtbExtn,
  IN  UFS_CONTROLLER_ID     ControllerId,
  IN  CONST CHAR8           *NodeSuffix,
  OUT DTB_EXTN_NODE_HANDLE  *Node
  )
{
  EFI_STATUS  Status;
  CHAR8       NodeName[UFS_DT_MAX_NODE_NAME_LENGTH];
  UINTN       NodeNameLen;

  NodeNameLen = AsciiSPrint (
                  NodeName,
                  sizeof (NodeName),
                  UFS_DT_ROOT_NODE_PREFIX "%d" "/" "%a",
                  ControllerId,
                  NodeSuffix
                  );

  if ((NodeNameLen == 0) || (NodeNameLen >= sizeof (NodeName) - 1)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] Failed to build %a node path for controller %u\n", NodeSuffix, ControllerId));
    return EFI_DEVICE_ERROR;
  }

  Status = UfsDtStatus (DtbExtn->FdtGetNodeHandle (Node, NodeName));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[UfsHc][DT] FdtGetNodeHandle(%a) failed: %r\n", NodeName, Status));
  }

  return Status;
}

/**
  Builds the DT node path, retrieves the node handle, and reads its properties.

  Dest is zeroed first, and the node's packed property size must equal
  DestSize, so a node with fewer or more properties than the structure has
  fields is rejected rather than leaving fields stale or silently truncated.

  @param[in]  DtbExtn      DTB extension protocol instance.
  @param[in]  ControllerId UFS controller identifier.
  @param[in]  NodeSuffix   Sub-node name suffix (e.g. "SpeedConfig").
  @param[in]  FormatStr    FdtGetPropValuesOfNode format string.
  @param[out] Dest         Destination buffer for the property values.
  @param[in]  DestSize     Size of Dest in bytes.

  @retval EFI_SUCCESS           Properties read into Dest successfully.
  @retval EFI_NOT_FOUND         The node does not exist.
  @retval EFI_COMPROMISED_DATA  The node's packed size does not match DestSize.
  @retval EFI_DEVICE_ERROR      The node path could not be built or the DTB is malformed.
**/
STATIC
EFI_STATUS
UfsDtReadNodeProps (
  IN  DTB_EXTN_PROTOCOL  *DtbExtn,
  IN  UFS_CONTROLLER_ID  ControllerId,
  IN  CONST CHAR8        *NodeSuffix,
  IN  CONST CHAR8        *FormatStr,
  OUT VOID               *Dest,
  IN  UINTN              DestSize
  )
{
  EFI_STATUS            Status;
  DTB_EXTN_NODE_HANDLE  Node;
  UINT32                PackedSize;

  ZeroMem (Dest, DestSize);

  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, NodeSuffix, &Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsDtStatus (DtbExtn->FdtGetPropValuesSizeOfNode (&Node, &PackedSize));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropValuesSizeOfNode(%a) failed: %r\n", NodeSuffix, Status));
    return Status;
  }

  if (PackedSize != DestSize) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc][DT] %a holds %u bytes of properties, expected %u (format \"%a\")\n",
      NodeSuffix,
      PackedSize,
      (UINT32)DestSize,
      FormatStr
      ));
    return EFI_COMPROMISED_DATA;
  }

  Status = UfsDtStatus (DtbExtn->FdtGetPropValuesOfNode (&Node, (CHAR8 *)FormatStr, Dest, (UINT32)DestSize));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropValuesOfNode(%a) failed: %r\n", NodeSuffix, Status));
  }

  return Status;
}

/**
  Locates DTB_EXTN_PROTOCOL.

  @param[out]  DtbExtn  The protocol instance.

  @retval EFI_SUCCESS    The protocol was found.
  @retval EFI_NOT_FOUND  The protocol is not installed.
**/
STATIC
EFI_STATUS
UfsDtLocateProtocol (
  OUT DTB_EXTN_PROTOCOL  **DtbExtn
  )
{
  EFI_STATUS  Status;

  *DtbExtn = NULL;
  Status   = gBS->LocateProtocol (&gDtbExtnProtocolGuid, NULL, (VOID **)DtbExtn);
  if (EFI_ERROR (Status) || (*DtbExtn == NULL)) {
    DEBUG ((DEBUG_WARN, "[UfsHc][DT] DTB extension protocol not available [Status=%r]\n", Status));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Retrieves the base config for UFS Driver from device tree.

  See UfsDtGetConfig.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsDtGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG    *BaseConfig
  )
{
  EFI_STATUS         Status;
  DTB_EXTN_PROTOCOL  *DtbExtn;

  if ((ControllerId >= UfsNumControllers) || (BaseConfig == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = UfsDtLocateProtocol (&DtbExtn);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // "ddbbbbb": UfsMemBase, UfsHciBase, PreHceEnable, PostLinkUpEnable,
  // RefClkFreq, SkipHceReenable, SkipLinkStartup. All seven properties are
  // required; a node with a different set is rejected by the size check.
  //
  Status = UfsDtReadNodeProps (
             DtbExtn,
             ControllerId,
             UFS_DT_NODE_BASE_CONFIG,
             "ddbbbbb",
             BaseConfig,
             sizeof (UFS_BASE_CONFIG)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((BaseConfig->UfsMemBase == 0) || (BaseConfig->UfsHciBase == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc][DT] Invalid base addresses: MemBase=0x%Lx HciBase=0x%Lx\n",
      (UINT64)BaseConfig->UfsMemBase,
      (UINT64)BaseConfig->UfsHciBase
      ));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Reads the PHY initialization tables.

  Each table is a list of (Flags, Offset, Value) UINT16 triplets. Entries up
  to the stop marker must have a 4-byte aligned Offset inside the PHY
  register window, since UfsPhyControl() writes them with MmioWrite32 at
  UfsMemBase + Offset. The size of register space the tables need is
  returned in PhyConfig->RegisterWindowSize so the caller can map it.

  @param[in]   DtbExtn       DTB extension protocol instance.
  @param[in]   ControllerId  UFS controller identifier.
  @param[out]  PhyConfig     Receives the tables.

  @retval EFI_SUCCESS           The tables were read.
  @retval EFI_COMPROMISED_DATA  A table is empty, too large, truncated, or
                                has an entry outside the PHY register window.
  @retval EFI_OUT_OF_RESOURCES  A table could not be allocated.
  @retval other                 A DT read failed.
**/
STATIC
EFI_STATUS
UfsDtReadPhyTables (
  IN  DTB_EXTN_PROTOCOL  *DtbExtn,
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_PHY_CONFIG     *PhyConfig
  )
{
  EFI_STATUS            Status;
  DTB_EXTN_NODE_HANDLE  Node;
  UINTN                 Loop;
  UINT32                Index;
  UINT32                NodeSize;
  UFS_PHY_TABLE_LIST    *PhyTable;
  UINT32                End;

  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, UFS_DT_NODE_PHY_TABLES, &Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Loop = 0; Loop < UfsPhyTableTypeMax; Loop++) {
    PhyTable = &PhyConfig->PhyTable[Loop];

    Status = UfsDtStatus (DtbExtn->FdtGetPropSize (&Node, mPhyTableNames[Loop], &NodeSize));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropSize(%a) failed %r\n", mPhyTableNames[Loop], Status));
      return Status;
    }

    if ((NodeSize == 0) || (NodeSize > UFS_PHY_TABLE_MAX_SIZE) ||
        ((NodeSize % sizeof (UFS_PHY_TABLE)) != 0))
    {
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc][DT] PhyTable %a size %u is not a whole number of %u-byte entries in 1..%u\n",
        mPhyTableNames[Loop],
        NodeSize,
        (UINT32)sizeof (UFS_PHY_TABLE),
        (UINT32)UFS_PHY_TABLE_MAX_SIZE
        ));
      return EFI_COMPROMISED_DATA;
    }

    PhyTable->Entries = AllocatePool (NodeSize);
    if (PhyTable->Entries == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    PhyTable->NumEntries = NodeSize / sizeof (UFS_PHY_TABLE);

    Status = UfsDtStatus (
               DtbExtn->FdtGetUint16PropList (
                          &Node,
                          mPhyTableNames[Loop],
                          (UINT16 *)PhyTable->Entries,
                          NodeSize
                          )
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetUint16PropList(%a) failed %r\n", mPhyTableNames[Loop], Status));
      return Status;
    }

    for (Index = 0; Index < PhyTable->NumEntries; Index++) {
      if (PhyTable->Entries[Index].Flags == UFS_PHY_CONFIG_STOP_CONDITION) {
        break;
      }

      End = (UINT32)PhyTable->Entries[Index].Offset + sizeof (UINT32);
      if (((PhyTable->Entries[Index].Offset & (sizeof (UINT32) - 1)) != 0) ||
          (End > UFS_PHY_REGISTER_WINDOW_MAX))
      {
        DEBUG ((
          DEBUG_ERROR,
          "[UfsHc][DT] PhyTable %a entry %u: offset 0x%x is unaligned or outside the PHY window\n",
          mPhyTableNames[Loop],
          Index,
          PhyTable->Entries[Index].Offset
          ));
        return EFI_COMPROMISED_DATA;
      }

      PhyConfig->RegisterWindowSize = MAX (PhyConfig->RegisterWindowSize, End);
    }

    DEBUG ((DEBUG_INFO, "[UfsHc][DT] PhyTable %a: %u entries\n", mPhyTableNames[Loop], PhyTable->NumEntries));
  }

  return EFI_SUCCESS;
}

/**
  Reads a NUL-terminated string property into a newly allocated buffer.

  The buffer is one byte larger than the property and always terminated, so
  it is safe to scan even if the DTB omitted the terminator.

  @param[in]   DtbExtn  DTB extension protocol instance.
  @param[in]   Node     The node holding the property.
  @param[in]   Name     The property name.
  @param[in]   MaxSize  Largest accepted property size in bytes.
  @param[out]  Buffer   Receives the allocated string.
  @param[out]  Size     Receives the property size in bytes.

  @retval EFI_SUCCESS           The property was read.
  @retval EFI_COMPROMISED_DATA  The property is empty or larger than MaxSize.
  @retval EFI_OUT_OF_RESOURCES  The buffer could not be allocated.
  @retval other                 A DT read failed.
**/
STATIC
EFI_STATUS
UfsDtReadString (
  IN  DTB_EXTN_PROTOCOL     *DtbExtn,
  IN  DTB_EXTN_NODE_HANDLE  *Node,
  IN  CONST CHAR8           *Name,
  IN  UINT32                MaxSize,
  OUT CHAR8                 **Buffer,
  OUT UINT32                *Size
  )
{
  EFI_STATUS  Status;

  Status = UfsDtStatus (DtbExtn->FdtGetPropSize (Node, (CHAR8 *)Name, Size));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropSize(%a) failed: %r\n", Name, Status));
    return Status;
  }

  if ((*Size == 0) || (*Size > MaxSize)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] %a size %u out of range 1..%u\n", Name, *Size, MaxSize));
    return EFI_COMPROMISED_DATA;
  }

  *Buffer = AllocateZeroPool (*Size + 1);
  if (*Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = UfsDtStatus (DtbExtn->FdtGetStringPropList (Node, (CHAR8 *)Name, *Buffer, *Size));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetStringPropList(%a) failed: %r\n", Name, Status));
  }

  (*Buffer)[*Size] = '\0';
  return Status;
}

/**
  Reads the list of devices that need TX de-emphasis.

  An empty list is valid and leaves VendorList->List NULL.

  @param[in]   DtbExtn       DTB extension protocol instance.
  @param[in]   ControllerId  UFS controller identifier.
  @param[out]  VendorList    Receives the list.

  @retval EFI_SUCCESS           The list was read.
  @retval EFI_COMPROMISED_DATA  The list is too long or holds a malformed entry.
  @retval EFI_OUT_OF_RESOURCES  The list could not be allocated.
  @retval other                 A DT read failed.
**/
STATIC
EFI_STATUS
UfsDtReadVendorList (
  IN  DTB_EXTN_PROTOCOL  *DtbExtn,
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_VENDOR_LIST    *VendorList
  )
{
  EFI_STATUS            Status;
  DTB_EXTN_NODE_HANDLE  Node;
  DTB_EXTN_NODE_HANDLE  *SubNode;
  UINT32                SubNodeCount;
  UINT32                Size;
  UINTN                 Loop;
  UFS_PRODUCT_LIST      *ProductList;

  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST, &Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsDtStatus (DtbExtn->FdtGetCountOfSubnodes (&Node, &SubNodeCount));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (SubNodeCount == 0) {
    return EFI_SUCCESS;
  }

  if (SubNodeCount > UFS_MAX_VENDOR_COUNT) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc][DT] De-emphasis list has %u vendors, maximum %u\n",
      SubNodeCount,
      (UINT32)UFS_MAX_VENDOR_COUNT
      ));
    return EFI_COMPROMISED_DATA;
  }

  SubNode = AllocatePool (sizeof (DTB_EXTN_NODE_HANDLE) * SubNodeCount);
  if (SubNode == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  VendorList->List = AllocateZeroPool (sizeof (UFS_PRODUCT_LIST) * SubNodeCount);
  if (VendorList->List == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  //
  // Count only after the list exists, so List and NumVendors always agree.
  //
  VendorList->NumVendors = SubNodeCount;

  Status = UfsDtStatus (DtbExtn->FdtGetCacheOfSubnodes (&Node, SubNode, SubNodeCount));
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  for (Loop = 0; Loop < SubNodeCount; Loop++) {
    ProductList = &VendorList->List[Loop];

    Status = UfsDtReadString (
               DtbExtn,
               &SubNode[Loop],
               UFS_DT_NODE_VENDOR_ID,
               UFS_CONFIG_MAX_VENDOR_NAME_LEN + 1,
               &ProductList->VendorName,
               &Size
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = UfsDtReadString (
               DtbExtn,
               &SubNode[Loop],
               UFS_DT_NODE_MODEL_ID,
               UFS_PHY_TABLE_MAX_SIZE,
               &ProductList->Products,
               &Size
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    ProductList->ProductListSize = Size;
  }

Exit:
  FreePool (SubNode);
  return Status;
}

/**
  Retrieves the UFS configuration data for the specified controller from device tree.

  See UfsDtGetConfig.h for the contract.

**/
EFI_STATUS
EFIAPI
UfsDtGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA    *Config
  )
{
  EFI_STATUS         Status;
  DTB_EXTN_PROTOCOL  *DtbExtn;

  if ((ControllerId >= UfsNumControllers) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Config is re-populated on every call; release the tables from any
  // previous call first so they are not leaked.
  //
  UfsDtFreeConfigData (Config);

  Status = UfsDtLocateProtocol (&DtbExtn);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsDtReadNodeProps (
             DtbExtn,
             ControllerId,
             UFS_DT_NODE_SPEED_CONFIG,
             "bbbbbbb",
             &Config->SpeedConfig,
             sizeof (UFS_SPEED_CONFIG)
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = UfsDtReadNodeProps (
             DtbExtn,
             ControllerId,
             UFS_DT_NODE_RESOURCE_CONFIG,
             "b",
             &Config->ResourceConfig,
             sizeof (UFS_RESOURCE_CONFIG)
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = UfsDtReadNodeProps (
             DtbExtn,
             ControllerId,
             UFS_DT_NODE_TIMEOUT_CONFIG,
             "wwww",
             &Config->TimeoutConfig,
             sizeof (UFS_TIMEOUT_CONFIG)
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = UfsDtReadNodeProps (
             DtbExtn,
             ControllerId,
             UFS_DT_NODE_REGISTER_CONFIG,
             "ddd",
             &Config->RegisterConfig,
             sizeof (UFS_REGISTER_CONFIG)
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = UfsDtReadPhyTables (DtbExtn, ControllerId, &Config->PhyConfig);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = UfsDtReadVendorList (DtbExtn, ControllerId, &Config->DeEmphasisVendorList);

Exit:
  if (EFI_ERROR (Status)) {
    UfsDtFreeConfigData (Config);
  }

  return Status;
}
