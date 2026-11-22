/** @file
  Lab DTB_EXTN_PROTOCOL for QcomUfsHcDxe on the QEMU Qualcomm UFS model.

  QcomUfsHcDxe reads its configuration through DTB_EXTN_PROTOCOL, which the
  Qualcomm DTFramework provides on silicon. On QEMU this driver builds the
  configuration device tree QcomUfsHcDxe expects (/soc/UfsController0/...)
  with FdtLib, taking the UFS memory window from QEMU's "qcom,sc7280-ufshc"
  node and the PHY programming from the Linux SC7280 tables
  (UfsLabPhyTables.h), and implements the protocol calls QcomUfsHcDxe makes
  with their documented semantics. Other calls log and return
  -DTB_ERR_NOTSUPPORTED.

  Lab code, not intended for upstream.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DeviceTreeExtension.h>
#include <Protocol/FdtClient.h>

#include "UfsLabPhyTables.h"

#define LAB_FDT_SIZE         SIZE_16KB
#define QCOM_UFS_COMPATIBLE  "qcom,sc7280-ufshc"
#define QCOM_UFS_MISC        0x4100     // misc registers of the QEMU model
#define QCOM_UFS_WINDOW      SIZE_64KB  // UFS memory window of the QEMU model

STATIC VOID  *mLabFdt;

//
// Property helpers: DT values are big-endian.
//
STATIC
INT32
LabSetU8 (
  IN INT32        Node,
  IN CONST CHAR8  *Name,
  IN UINT8        Value
  )
{
  return FdtSetProp (mLabFdt, Node, Name, &Value, sizeof (Value));
}

STATIC
INT32
LabSetU32 (
  IN INT32        Node,
  IN CONST CHAR8  *Name,
  IN UINT32       Value
  )
{
  Value = SwapBytes32 (Value);
  return FdtSetProp (mLabFdt, Node, Name, &Value, sizeof (Value));
}

STATIC
INT32
LabSetU64 (
  IN INT32        Node,
  IN CONST CHAR8  *Name,
  IN UINT64       Value
  )
{
  Value = SwapBytes64 (Value);
  return FdtSetProp (mLabFdt, Node, Name, &Value, sizeof (Value));
}

STATIC
INT32
LabSetU16List (
  IN INT32         Node,
  IN CONST CHAR8   *Name,
  IN CONST UINT16  *List,
  IN UINTN         Count
  )
{
  UINT16  Buffer[256 * 3];
  UINTN   Index;

  if (Count > ARRAY_SIZE (Buffer)) {
    return -1;
  }

  for (Index = 0; Index < Count; Index++) {
    Buffer[Index] = SwapBytes16 (List[Index]);
  }

  return FdtSetProp (mLabFdt, Node, Name, Buffer, (UINT32)(Count * sizeof (UINT16)));
}

typedef enum {
  LabU8,
  LabU32,
  LabU64,
  LabU16List
} LAB_PROP_TYPE;

typedef struct {
  CONST CHAR8     *Name;
  LAB_PROP_TYPE   Type;
  UINT64          Value;
  CONST UINT16    *List;
  UINTN           Count;
} LAB_PROP;

#define U8(N, V)       { N, LabU8, (V), NULL, 0 }
#define U32(N, V)      { N, LabU32, (V), NULL, 0 }
#define U64(N, V)      { N, LabU64, (V), NULL, 0 }
#define U16LIST(N, L)  { N, LabU16List, 0, L, ARRAY_SIZE (L) }

/**
  Add a node with the given properties, in the given order.

  FdtSetProp() inserts a new property first in its node, so the properties
  are set in reverse to keep the order the driver reads them in.

  @param[in] Parent  Parent node offset.
  @param[in] Name    Node name.
  @param[in] Props   Properties.
  @param[in] Count   Number of properties.

  @retval 0     Success.
  @retval <0    FdtLib error.
**/
STATIC
INT32
LabAddNode (
  IN INT32           Parent,
  IN CONST CHAR8     *Name,
  IN CONST LAB_PROP  *Props,
  IN UINTN           Count
  )
{
  INT32  Node;
  INT32  Err;
  UINTN  Index;

  Node = FdtAddSubnode (mLabFdt, Parent, Name);
  if (Node < 0) {
    return Node;
  }

  for (Index = Count; Index > 0; Index--) {
    switch (Props[Index - 1].Type) {
      case LabU8:
        Err = LabSetU8 (Node, Props[Index - 1].Name, (UINT8)Props[Index - 1].Value);
        break;
      case LabU32:
        Err = LabSetU32 (Node, Props[Index - 1].Name, (UINT32)Props[Index - 1].Value);
        break;
      case LabU64:
        Err = LabSetU64 (Node, Props[Index - 1].Name, Props[Index - 1].Value);
        break;
      default:
        Err = LabSetU16List (Node, Props[Index - 1].Name, Props[Index - 1].List, Props[Index - 1].Count);
        break;
    }

    if (Err < 0) {
      return Err;
    }
  }

  return 0;
}

/**
  Build the QcomUfsHcDxe configuration tree for a UFS window at MemBase.

  @param[in] MemBase  UFS memory base (the UFSHCI registers, offset 0).

  @retval EFI_SUCCESS  Tree built.
  @retval Others       Out of space or an FdtLib error.
**/
STATIC
EFI_STATUS
LabBuildFdt (
  IN UINT64  MemBase
  )
{
  INT32  Soc;
  INT32  Ctl;
  INT32  Err;

  //
  // Property order is the field order of the driver's structures.
  //
  CONST LAB_PROP  Base[] = {
    U64 ("UfsMemBase", MemBase),
    U64 ("UfsHciBase", MemBase),
    U8 ("PreHceEnable", 1),
    U8 ("PostLinkUpEnable", 1),
    U8 ("RefClkFreq", 0),                      // 19.2 MHz
    U8 ("SkipHceReenable", 0),
    U8 ("SkipLinkStartup", 0)
  };
  CONST LAB_PROP  Speed[] = {
    U8 ("Rate", 1),                            // UfsRateA
    U8 ("Gear", 1),                            // UfsHsGear1
    U8 ("HsEnable", 1),
    U8 ("NumLanes", 1),
    U8 ("AdaptEnable", 0),
    U8 ("LinkStartupMode", 0),                 // UfsLinkStartupModeLsLss
    U8 ("WriteBoosterEnable", 0)
  };
  CONST LAB_PROP  Resource[] = {
    U8 ("VoltageCorner", 4)                    // UfsVoltageCornerNominal
  };
  CONST LAB_PROP  Timeout[] = {
    U32 ("HcEnablePollTimeout", 100000),
    U32 ("HcPcsReadyPollTimeout", 100000),
    U32 ("HibernatePollTimeout", 100000),
    U32 ("UmpsPollTimeout", 100000)
  };
  CONST LAB_PROP  Register[] = {
    U64 ("ResetRegister", MemBase + QCOM_UFS_MISC),
    U64 ("SharedPhyRegister", MemBase + QCOM_UFS_MISC + 4),
    U64 ("SpareRegister", MemBase + QCOM_UFS_MISC + 8)
  };
  CONST LAB_PROP  Phy[] = {
    U16LIST ("InitTable", mUfsLabInitTable),
    U16LIST ("Lane0Table", mUfsLabLane0Table),
    U16LIST ("Lane1Table", mUfsLabLane1Table),
    U16LIST ("EndTable", mUfsLabEndTable),
    U16LIST ("HsLss", mUfsLabHsLss)
  };

  mLabFdt = AllocateZeroPool (LAB_FDT_SIZE);
  if (mLabFdt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Err = FdtCreateEmptyTree (mLabFdt, LAB_FDT_SIZE);
  Soc = (Err == 0) ? FdtAddSubnode (mLabFdt, 0, "soc") : Err;
  Ctl = (Soc >= 0) ? FdtAddSubnode (mLabFdt, Soc, "UfsController0") : Soc;
  if (Ctl < 0) {
    return EFI_DEVICE_ERROR;
  }

  //
  // FdtAddSubnode() also inserts first, so add the nodes in reverse too.
  //
  Err = LabAddNode (Ctl, "DeEmphasisVendorList", NULL, 0);
  if (Err == 0) {
    Err = LabAddNode (Ctl, "PhyConfig", Phy, ARRAY_SIZE (Phy));
  }

  if (Err == 0) {
    Err = LabAddNode (Ctl, "RegisterConfig", Register, ARRAY_SIZE (Register));
  }

  if (Err == 0) {
    Err = LabAddNode (Ctl, "TimeoutConfig", Timeout, ARRAY_SIZE (Timeout));
  }

  if (Err == 0) {
    Err = LabAddNode (Ctl, "ResourceConfig", Resource, ARRAY_SIZE (Resource));
  }

  if (Err == 0) {
    Err = LabAddNode (Ctl, "SpeedConfig", Speed, ARRAY_SIZE (Speed));
  }

  if (Err == 0) {
    Err = LabAddNode (Ctl, "BaseConfig", Base, ARRAY_SIZE (Base));
  }

  return (Err == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

//
// DTB_EXTN_PROTOCOL calls used by QcomUfsHcDxe.
//
STATIC
INT32
LabNodeOk (
  IN DTB_EXTN_NODE_HANDLE  *Node
  )
{
  return ((Node != NULL) && (Node->Blob == mLabFdt) && (Node->Offset >= 0)) ? 0 : -DTB_ERR_NULLPTR;
}

STATIC
INT32
EFIAPI
LabGetNodeHandle (
  IN OUT DTB_EXTN_NODE_HANDLE  *Node,
  IN CHAR8                     *Name
  )
{
  INT32  Offset;

  if ((Node == NULL) || (Name == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  Offset = FdtPathOffset (mLabFdt, Name);
  if (Offset < 0) {
    return Offset;
  }

  Node->Blob   = mLabFdt;
  Node->Offset = Offset;
  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetPropSize (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT UINT32            *Size
  )
{
  INT32       Len;
  CONST VOID  *Value;

  if ((LabNodeOk (Node) != 0) || (PropName == NULL) || (Size == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  Value = FdtGetProp (mLabFdt, Node->Offset, PropName, &Len);
  if (Value == NULL) {
    return Len;
  }

  *Size = (UINT32)Len;
  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetPropValuesSizeOfNode (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN OUT UINT32            *Size
  )
{
  INT32  Prop;
  INT32  Len;

  if ((LabNodeOk (Node) != 0) || (Size == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  *Size = 0;
  for (Prop = FdtFirstPropertyOffset (mLabFdt, Node->Offset); Prop >= 0;
       Prop = FdtNextPropertyOffset (mLabFdt, Prop))
  {
    if (FdtGetPropertyByOffset (mLabFdt, Prop, &Len) == NULL) {
      return -DTB_ERR_SDNULL;
    }

    *Size += (UINT32)Len;
  }

  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetPropValuesOfNode (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CHAR8                 *Format,
  IN OUT VOID              *PackedPropValues,
  IN UINT32                Size
  )
{
  CONST FDT_PROPERTY  *Property;
  INT32               Prop;
  INT32               Len;
  UINT8               *Out;
  UINT32              Used;
  CONST UINT8         *Data;

  if ((LabNodeOk (Node) != 0) || (Format == NULL) || (PackedPropValues == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  Out  = PackedPropValues;
  Used = 0;
  for (Prop = FdtFirstPropertyOffset (mLabFdt, Node->Offset); Prop >= 0;
       Prop = FdtNextPropertyOffset (mLabFdt, Prop), Format++)
  {
    Property = FdtGetPropertyByOffset (mLabFdt, Prop, &Len);
    if (Property == NULL) {
      return -DTB_ERR_SDNULL;
    }

    if (Len == 0) {
      return -DTB_ERR_NILVALUE;
    }

    if (*Format == '\0') {
      return -DTB_ERR_BADFORMAT;
    }

    if ((*Format == 'i') || (*Format == 'I')) {
      continue;
    }

    if (Used + (UINT32)Len > Size) {
      return -DTB_ERR_TRUNCATED;
    }

    Data = (CONST UINT8 *)Property->Data;
    switch (*Format) {
      case 'b':
      case 'B':
        CopyMem (Out + Used, Data, Len);
        break;
      case 'h':
      case 'H':
        if (Len != sizeof (UINT16)) {
          return -DTB_ERR_BADFORMAT;
        }

        WriteUnaligned16 ((UINT16 *)(Out + Used), SwapBytes16 (ReadUnaligned16 ((CONST UINT16 *)Data)));
        break;
      case 'w':
      case 'W':
        if (Len != sizeof (UINT32)) {
          return -DTB_ERR_BADFORMAT;
        }

        WriteUnaligned32 ((UINT32 *)(Out + Used), SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)Data)));
        break;
      case 'd':
      case 'D':
        if (Len != sizeof (UINT64)) {
          return -DTB_ERR_BADFORMAT;
        }

        WriteUnaligned64 ((UINT64 *)(Out + Used), SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Data)));
        break;
      default:
        return -DTB_ERR_BADFORMAT;
    }

    Used += (UINT32)Len;
  }

  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetStringPropList (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT CHAR8             *StringList,
  IN UINT32                Size
  )
{
  INT32       Len;
  CONST VOID  *Value;

  if ((LabNodeOk (Node) != 0) || (PropName == NULL) || (StringList == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  Value = FdtGetProp (mLabFdt, Node->Offset, PropName, &Len);
  if (Value == NULL) {
    return Len;
  }

  if ((UINT32)Len > Size) {
    return -DTB_ERR_BUF2SMALL;
  }

  CopyMem (StringList, Value, Len);
  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetUint16PropList (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT UINT16            *PropList,
  IN UINT32                Size
  )
{
  INT32         Len;
  CONST UINT16  *Value;
  INT32         Index;

  if ((LabNodeOk (Node) != 0) || (PropName == NULL) || (PropList == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  Value = FdtGetProp (mLabFdt, Node->Offset, PropName, &Len);
  if (Value == NULL) {
    return Len;
  }

  if (((UINT32)Len > Size) || ((Len % sizeof (UINT16)) != 0)) {
    return -DTB_ERR_BUF2SMALL;
  }

  for (Index = 0; Index < Len / (INT32)sizeof (UINT16); Index++) {
    PropList[Index] = SwapBytes16 (ReadUnaligned16 (&Value[Index]));
  }

  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetCountOfSubnodes (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN OUT UINT32            *Count
  )
{
  INT32  Sub;

  if ((LabNodeOk (Node) != 0) || (Count == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  *Count = 0;
  for (Sub = FdtFirstSubnode (mLabFdt, Node->Offset); Sub >= 0; Sub = FdtNextSubnode (mLabFdt, Sub)) {
    (*Count)++;
  }

  return DTB_ERR_NOERROR;
}

STATIC
INT32
EFIAPI
LabGetCacheOfSubnodes (
  IN DTB_EXTN_NODE_HANDLE      *Node,
  IN OUT DTB_EXTN_NODE_HANDLE  *Cache,
  IN UINT32                    Count
  )
{
  INT32   Sub;
  UINT32  Index;

  if ((LabNodeOk (Node) != 0) || ((Cache == NULL) && (Count != 0))) {
    return -DTB_ERR_NULLPTR;
  }

  Index = 0;
  for (Sub = FdtFirstSubnode (mLabFdt, Node->Offset); (Sub >= 0) && (Index < Count);
       Sub = FdtNextSubnode (mLabFdt, Sub), Index++)
  {
    Cache[Index].Blob   = mLabFdt;
    Cache[Index].Offset = Sub;
  }

  return (Index == Count) ? DTB_ERR_NOERROR : -DTB_ERR_INPUT_ARG_ERR;
}

/**
  Stand-in for every protocol call QcomUfsHcDxe does not use. All
  DTB_EXTN_PROTOCOL calls return INT32 in the same register, so one stub
  serves them regardless of their arguments.

  @return -DTB_ERR_NOTSUPPORTED.
**/
STATIC
INT32
EFIAPI
LabNotSupported (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "QemuDtbExtnDxe: unsupported DTB_EXTN_PROTOCOL call\n"));
  return -DTB_ERR_NOTSUPPORTED;
}

#define NS(Type)  ((Type)(VOID *)LabNotSupported)

STATIC DTB_EXTN_PROTOCOL  mLabDtbExtn = {
  DTB_EXTN_PROTOCOL_VERSION,
  NS (DTB_INIT_ROOT_HANDLE_FOR_DRIVER),
  NS (DTB_GET_NAME_INDEX),
  NS (DTB_GET_NEXT_NODE_HANDLE_FOR_COMPATIBLE),
  NS (DTB_GET_REG),
  LabGetCountOfSubnodes,
  LabGetNodeHandle,
  NS (DTB_GET_PARENT_NODE),
  NS (DTB_GET_PHANDLE_NODE),
  NS (DTB_GET_PROP_NAMES_OF_NODE),
  NS (DTB_GET_PROP_NAMES_SIZE_OF_NODE),
  LabGetPropValuesOfNode,
  LabGetPropValuesSizeOfNode,
  NS (DTB_GET_SIZE_OF_SUBNODE_NAMES),
  NS (DTB_GET_SUBNODE_NAMES),
  NS (DTB_NODE_CMP),
  NS (DTB_NODE_COPY),
  NS (DTB_GET_BOOLEAN_PROP),
  LabGetPropSize,
  LabGetStringPropList,
  NS (DTB_GET_UINT32_PROP),
  NS (DTB_GET_UINT32_PROP_LIST),
  NS (DTB_GET_UINT32_PROP_LIST_SLICE),
  NS (DTB_GET_UINT64_PROP),
  NS (DTB_GET_UINT64_PROP_LIST),
  NS (DTB_GET_UINT8_PROP),
  NS (DTB_GET_UINT8_PROP_LIST),
  NS (DTB_GET_UINT8_PROP_LIST_SLICE),
  NS (DTB_GET_UINT16_PROP),
  LabGetUint16PropList,
  NS (DTB_GET_UINT16_PROP_LIST_SLICE),
  LabGetCacheOfSubnodes,
  NS (DTB_GET_UINT64_PROP_LIST_SLICE),
  NS (DTB_SET_BLOB_HANDLE),
  NS (DTB_CHECK_VALID_BLOB),
  NS (DTB_MERGE_OVERLAY),
  NS (DTB_GET_BLOB_HANDLE),
  NS (DTB_GET_BLOB_SIZE),
  NS (DTB_INIT_ROOT_HANDLE_FOR_DRIVER_BY_ID),
  NS (DTB_GET_NODE_HANDLE_BY_BLOB),
  NS (DTB_INIT_ROOT_HANDLE_FOR_DRIVER_BY_BLOB),
  NS (DTB_GET_BOOL_PROP)
};

/**
  Entry point: build the configuration tree from QEMU's UFS node and
  install the protocol. Without the node nothing is installed, which
  QcomUfsHcDxe reports as a missing configuration.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to the EFI system table.

  @retval EFI_SUCCESS  Installed, or no Qualcomm UFS node.
  @retval Others       The tree could not be built or the protocol installed.
**/
EFI_STATUS
EFIAPI
QemuDtbExtnDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  CONST VOID           *Reg;
  UINT32               RegSize;
  UINT64               MemBase;
  EFI_HANDLE           Handle;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&FdtClient);
  if (!EFI_ERROR (Status)) {
    Status = FdtClient->FindCompatibleNode (FdtClient, QCOM_UFS_COMPATIBLE, &Node);
  }

  if (!EFI_ERROR (Status)) {
    Status = FdtClient->GetNodeProperty (FdtClient, Node, "reg", &Reg, &RegSize);
  }

  if (EFI_ERROR (Status) || (RegSize < 2 * sizeof (UINT64))) {
    DEBUG ((DEBUG_INFO, "QemuDtbExtnDxe: no %a node, protocol not installed\n", QCOM_UFS_COMPATIBLE));
    return EFI_SUCCESS;
  }

  //
  // QcomUfsHcDxe maps the HCI, PHY and RegisterConfig registers it uses
  // itself, as it must on silicon; nothing is mapped here.
  //
  MemBase = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Reg));
  Status  = LabBuildFdt (MemBase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "QemuDtbExtnDxe: building the configuration failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "QemuDtbExtnDxe: UfsController0 at 0x%lx, %u bytes of configuration\n", MemBase, FdtTotalSize (mLabFdt)));
  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gDtbExtnProtocolGuid, &mLabDtbExtn, NULL);
}
