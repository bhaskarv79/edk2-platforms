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
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
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
STATIC CONST CHAR8  * CONST mPhyTableNames[UfsPhyTableTypeMax] = {
  "InitTable",
  "Lane0Table",
  "Lane1Table",
  "EndTable",
  "HsLss"
};

#define UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST  "DeEmphasisVendorList"
#define UFS_DT_NODE_VENDOR_PREFIX            "Vendor"

/**
  Frees all heap allocations made by UfsDtGetConfigData() into Config.

  Safe to call on a partially-populated Config (NULL pointers are skipped).
  Does NOT free Config itself.

  @param[in,out] Config  Pointer to the config structure whose members to free.

**/
STATIC
VOID
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
  }

  VendorList = &Config->DeEmphasisVendorList;
  if (VendorList->List != NULL) {
    for (Loop = 0; Loop < VendorList->NumVendors; Loop++) {
      ProductList = &VendorList->List[Loop];
      if (ProductList->VendorName != NULL) {
        FreePool (ProductList->VendorName);
        ProductList->VendorName = NULL;
      }

      if (ProductList->Products != NULL) {
        FreePool (ProductList->Products);
        ProductList->Products = NULL;
      }
    }

    FreePool (VendorList->List);
    VendorList->List       = NULL;
    VendorList->NumVendors = 0;
  }
}

#define UFS_DT_NODE_VENDOR_ID  "VendorId"
#define UFS_DT_NODE_MODEL_ID   "ModelId"

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
  IN  DTB_EXTN_PROTOCOL  *DtbExtn,
  IN  UFS_CONTROLLER_ID      ControllerId,
  IN  CONST CHAR8            *NodeSuffix,
  OUT FDT_NODE_HANDLE        *Node
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

  Status = DtbExtn->FdtGetNodeHandle (Node, NodeName);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetNodeHandle(%a) failed: %r\n", NodeName, Status));
  }

  return Status;
}

/**
  Builds the DT node path, retrieves the node handle, and reads its properties.

  @param[in]  DtbExtn      DTB extension protocol instance.
  @param[in]  ControllerId UFS controller identifier.
  @param[in]  NodeSuffix   Sub-node name suffix (e.g. "SpeedConfig").
  @param[in]  FormatStr    FdtGetPropValuesOfNode format string.
  @param[out] Dest         Destination buffer for the property values.
  @param[in]  DestSize     Size of Dest in bytes.

  @retval EFI_SUCCESS      Properties read into Dest successfully.
  @retval EFI_DEVICE_ERROR Node path construction failed.
  @retval other            Status from FdtGetNodeHandle or FdtGetPropValuesOfNode.

**/
STATIC
EFI_STATUS
UfsDtReadNodeProps (
  IN  DTB_EXTN_PROTOCOL  *DtbExtn,
  IN  UFS_CONTROLLER_ID      ControllerId,
  IN  CONST CHAR8            *NodeSuffix,
  IN  CONST CHAR8            *FormatStr,
  OUT VOID                   *Dest,
  IN  UINTN                  DestSize
  )
{
  EFI_STATUS       Status;
  FDT_NODE_HANDLE  Node;

  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, NodeSuffix, &Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DtbExtn->FdtGetPropValuesOfNode (&Node, FormatStr, Dest, DestSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropValuesOfNode(%a) failed: %r\n", NodeSuffix, Status));
  }

  return Status;
}

/**
  Retrieves the base config for UFS Driver from device tree.

  This function returns the physical base addresses for the UFS memory region,
  Host Controller Interface (HCI) registers, Enable callback phases for
  platform callback on specified controller.

  @param[in]  ControllerId      The unique identifier of the UFS controller.
  @param[out] BaseConfig        Pointer to store the UFS base config.

  @retval EFI_SUCCESS         Base addresses retrieved successfully.
  @retval EFI_INVALID_PARAMETER Invalid ControllerId or NULL pointer parameters.
  @retval EFI_NOT_FOUND       Base addresses not found for the specified controller.
  @retval EFI_DEVICE_ERROR    Failed to retrieve base addresses due to hardware error.

**/
EFI_STATUS
EFIAPI
UfsDtGetBaseConfig (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_BASE_CONFIG  *BaseConfig
  )
{
  EFI_STATUS             Status;
  DTB_EXTN_PROTOCOL  *DtbExtn;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (BaseConfig == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DtbExtn = NULL;
  Status  = gBS->LocateProtocol (
                   &gDtbExtnProtocolGuid,
                   NULL,
                   (VOID **)&DtbExtn
                   );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] LocateProtocol(gDtbExtnProtocolGuid) failed: %r\n", Status));
    goto Exit;
  }

  if (DtbExtn == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] DtbExtn protocol not accessible\n"));
    Status = EFI_NOT_FOUND;
    goto Exit;
  }

  //
  // Format string "ddbbbbb" maps directly onto UFS_BASE_CONFIG (pragma pack 1):
  //   d = UINT64 UfsMemBase
  //   d = UINT64 UfsHciBase
  //   b = UINT8  PreHceEnable
  //   b = UINT8  PostLinkUpEnable
  //   b = UINT8  RefClkFreq        <-- added in current DTB schema revision
  //   b = UINT8  SkipHceReenable   <-- added in current DTB schema revision
  //   b = UINT8  SkipLinkStartup   <-- added in current DTB schema revision
  //
  // BACKWARD COMPATIBILITY: This format string is NOT backward-compatible with
  // DTBs that used the earlier "ddbb" schema (UfsMemBase, UfsHciBase,
  // PreHceEnable, PostLinkUpEnable only). An older DTB will cause
  // FdtGetPropValuesOfNode to return an error (EFI_DEVICE_ERROR or similar),
  // which is caught below. The static PCD fallback in UfsGetBaseConfig() will
  // then supply RefClkFreq, SkipHceReenable, and SkipLinkStartup from their
  // compile-time defaults.
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
    goto Exit;
  }

  if ((BaseConfig->UfsMemBase == 0) || (BaseConfig->UfsHciBase == 0)) {
    Status = EFI_NO_MEDIA;
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc][DT] Invalid base addresses: MemBase=0x%Lx HciBase=0x%Lx\n",
      (UINT64)BaseConfig->UfsMemBase,
      (UINT64)BaseConfig->UfsHciBase
      ));
    goto Exit;
  }

  Status = EFI_SUCCESS;
Exit:
  return Status;
}

/**
  Retrieves the UFS configuration data for the specified controller from device tree.

  Parses platform-specific configuration parameters required for UFS host
  controller initialization, including speed, resource, timeout, and register
  configurations, PHY tables, and de-emphasis vendor/product lists.

  @param[in]  ControllerId    The unique identifier of the UFS controller.
  @param[out] Config          Pointer to the UFS_CONFIG_DATA structure to be
                              populated with configuration parameters.

  @retval EFI_SUCCESS            Configuration data retrieved successfully.
  @retval EFI_INVALID_PARAMETER  Invalid ControllerId or Config pointer is NULL.
  @retval EFI_NOT_FOUND          Required DT nodes/properties not found.
  @retval EFI_DEVICE_ERROR       Failed to parse DT properties.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure while building tables.
**/
EFI_STATUS
EFIAPI
UfsDtGetConfigData (
  IN  UFS_CONTROLLER_ID  ControllerId,
  OUT UFS_CONFIG_DATA  *Config
  )
{
  EFI_STATUS             Status;
  DTB_EXTN_PROTOCOL  *DtbExtn;
  FDT_NODE_HANDLE        Node;
  FDT_NODE_HANDLE        *SubNode;

  UINTN               Loop;
  UINT32              NodeSize;
  UINT32              SubNodeCount;
  UFS_PHY_TABLE_LIST  *PhyTable;
  UFS_VENDOR_LIST     *VendorList;
  UFS_PRODUCT_LIST    *ProductList;

  SubNode = NULL;

  if (ControllerId >= UfsNumControllers) {
    return EFI_INVALID_PARAMETER;
  }

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DtbExtn = NULL;
  Status  = gBS->LocateProtocol (
                   &gDtbExtnProtocolGuid,
                   NULL,
                   (VOID **)&DtbExtn
                   );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] LocateProtocol(gDtbExtnProtocolGuid) failed: %r\n", Status));
    goto Exit;
  }

  if (DtbExtn == NULL) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] DtbExtn protocol not accessible\n"));
    Status = EFI_NOT_FOUND;
    goto Exit;
  }

  //
  // Get Speed Config
  //
  Status = UfsDtReadNodeProps (DtbExtn, ControllerId, UFS_DT_NODE_SPEED_CONFIG,
             "bbbbbbb", &Config->SpeedConfig, sizeof (UFS_SPEED_CONFIG));
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // Get Resource Config
  //
  Status = UfsDtReadNodeProps (DtbExtn, ControllerId, UFS_DT_NODE_RESOURCE_CONFIG,
             "b", &Config->ResourceConfig, sizeof (UFS_RESOURCE_CONFIG));
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // Get Timeout Config
  //
  Status = UfsDtReadNodeProps (DtbExtn, ControllerId, UFS_DT_NODE_TIMEOUT_CONFIG,
             "wwww", &Config->TimeoutConfig, sizeof (UFS_TIMEOUT_CONFIG));
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // Get Register Config
  //
  Status = UfsDtReadNodeProps (DtbExtn, ControllerId, UFS_DT_NODE_REGISTER_CONFIG,
             "ddd", &Config->RegisterConfig, sizeof (UFS_REGISTER_CONFIG));
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // Get Phy Table Config
  //
  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, UFS_DT_NODE_PHY_TABLES, &Node);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  for (Loop = 0; Loop < UfsPhyTableTypeMax; Loop++) {
    PhyTable = &Config->PhyConfig.PhyTable[Loop];

    Status = DtbExtn->FdtGetPropSize (&Node, mPhyTableNames[Loop], &NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropSize(%a) failed %r\n", mPhyTableNames[Loop], Status));
      goto Exit;
    }

    if ((NodeSize == 0) || (NodeSize > UFS_PHY_TABLE_MAX_SIZE)) {
      Status = EFI_COMPROMISED_DATA;
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc][DT] PhyTable %a NodeSize=%u out of valid range (1..%u)\n",
        mPhyTableNames[Loop],
        NodeSize,
        (UINT32)UFS_PHY_TABLE_MAX_SIZE
        ));
      goto Exit;
    }

    PhyTable->Entries = AllocatePool (NodeSize);
    if (PhyTable->Entries == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc][DT] Out of resources allocating PhyTable %a (size=%u): %r\n",
        mPhyTableNames[Loop],
        NodeSize,
        Status
        ));
      goto Exit;
    }

    PhyTable->NumEntries = NodeSize / sizeof (UFS_PHY_TABLE);

    DEBUG ((DEBUG_INFO, "[UfsHc][DT] PhyTable%u entries = %u\n", Loop, PhyTable->NumEntries));

    Status = DtbExtn->FdtGetUint16PropList (&Node, mPhyTableNames[Loop], (UINT16 *)PhyTable->Entries, NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetUint16PropList(%a) failed %r\n", mPhyTableNames[Loop], Status));
      goto Exit;
    }
  }

  //
  // Get De-Emphasis Setting Config
  //
  Status = UfsDtBuildAndGetNode (DtbExtn, ControllerId, UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST, &Node);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = DtbExtn->FdtGetCountOfSubnodes (&Node, &SubNodeCount);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetCountOfSubnodes(%a) failed: %r\n", UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST, Status));
    goto Exit;
  }

  if (SubNodeCount > UFS_MAX_VENDOR_COUNT) {
    Status = EFI_COMPROMISED_DATA;
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc][DT] DeEmphasis SubNodeCount=%u exceeds maximum (%u)\n",
      SubNodeCount,
      (UINT32)UFS_MAX_VENDOR_COUNT
      ));
    goto Exit;
  }

  VendorList             = &Config->DeEmphasisVendorList;
  VendorList->NumVendors = SubNodeCount;
  VendorList->List       = (UFS_PRODUCT_LIST *)AllocateZeroPool (sizeof (UFS_PRODUCT_LIST) * SubNodeCount);
  if (VendorList->List == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] Out of resources allocating VendorList (count=%u)\n", SubNodeCount));
    goto Exit;
  }

  SubNode = (FDT_NODE_HANDLE *)AllocatePool (sizeof (FDT_NODE_HANDLE) * SubNodeCount);
  if (SubNode == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] Out of resources allocating SubNode cache (count=%u)\n", SubNodeCount));
    FreePool (VendorList->List);
    VendorList->List = NULL;
    goto Exit;
  }

  Status = DtbExtn->FdtGetCacheOfSubnodes (&Node, SubNode, SubNodeCount);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetCacheOfSubnodes failed for %a: %r\n", UFS_DT_NODE_DE_EMPHASIS_VENDOR_LIST, Status));
    goto Exit;
  }

  for (Loop = 0; Loop < SubNodeCount; Loop++) {
    ProductList = &VendorList->List[Loop];

    Status = DtbExtn->FdtGetPropSize (&SubNode[Loop], UFS_DT_NODE_VENDOR_ID, &NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropSize(%a) failed: %r\n", UFS_DT_NODE_VENDOR_ID, Status));
      goto Exit;
    }

    ProductList->VendorName = AllocatePool (NodeSize);
    if (ProductList->VendorName == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] Out of resources allocating VendorName (size=%u)\n", NodeSize));
      goto Exit;
    }

    Status = DtbExtn->FdtGetStringPropList (&SubNode[Loop], UFS_DT_NODE_VENDOR_ID, ProductList->VendorName, NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetStringPropList(%a) failed: %r\n", UFS_DT_NODE_VENDOR_ID, Status));
      goto Exit;
    }

    Status = DtbExtn->FdtGetPropSize (&SubNode[Loop], UFS_DT_NODE_MODEL_ID, &NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetPropSize(%a) failed: %r\n", UFS_DT_NODE_MODEL_ID, Status));
      goto Exit;
    }

    ProductList->ProductListSize = NodeSize;

    ProductList->Products = AllocatePool (NodeSize);
    if (ProductList->Products == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] Out of resources allocating Products buffer (size=%u)\n", NodeSize));
      goto Exit;
    }

    Status = DtbExtn->FdtGetStringPropList (&SubNode[Loop], UFS_DT_NODE_MODEL_ID, ProductList->Products, NodeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc][DT] FdtGetStringPropList(%a) failed: %r\n", UFS_DT_NODE_MODEL_ID, Status));
      goto Exit;
    }
  }

  Status = EFI_SUCCESS;
Exit:
  if (EFI_ERROR (Status)) {
    UfsDtFreeConfigData (Config);
  }

  if (SubNode != NULL) {
    FreePool ((VOID *)SubNode);
  }

  return Status;
}
