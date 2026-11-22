/** @file
  Host-based unit tests for QcomUfsHcDxe.

  Runs the configuration and PHY code of the driver (UfsDtGetConfig.c,
  UfsGetConfig.c, UfsPhy.c, UfsUtils.c) on the build host against:
  - a DTB_EXTN_PROTOCOL mock holding the /soc/UfsController0 nodes, which a
    test can remove, resize or make fail one at a time;
  - IoLib and TimerLib replacements over a 64 KiB fake register window, with
    a log of every write and a scripted PCS-ready register.

  Built twice by QcomUfsHostTest.dsc: with the device tree configuration
  source, and PCD-only with the PreHce phase enabled.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UnitTestLib.h>
#include <Protocol/DeviceTreeExtension.h>

#include "../UfsCore.h"
#include "../UfsDtGetConfig.h"
#include "../UfsGetConfig.h"
#include "../UfsPhy.h"
#include "../UfsUtpCore.h"

#define UNIT_TEST_APP_NAME     "QcomUfsHcDxe host unit tests"
#define UNIT_TEST_APP_VERSION  "1.0"

//
// UfsUtils.c references gDS for its MMIO mapping helper, which these tests
// do not call; the host build has no DXE services table.
//
EFI_DXE_SERVICES  *gDS = NULL;

// ---------------------------------------------------------------------------
// Fake register window (IoLib and TimerLib replacements)
// ---------------------------------------------------------------------------

#define FAKE_MEM_BASE     0x1D80000ULL
#define FAKE_WINDOW_SIZE  SIZE_64KB
#define MAX_WRITES        512

typedef struct {
  UINT32    Offset;
  UINT32    Value;
} FAKE_WRITE;

STATIC UINT32      mRegs[FAKE_WINDOW_SIZE / sizeof (UINT32)];
STATIC FAKE_WRITE  mWrites[MAX_WRITES];
STATIC UINTN       mWriteCount;
STATIC BOOLEAN     mWriteOverflow;
STATIC UINTN       mPcsReadyAfterReads;     // 0: never ready
STATIC UINTN       mPcsReads;
STATIC UINT64      mDelayUs;

STATIC
UINT32 *
FakeReg (
  IN UINTN  Address
  )
{
  ASSERT ((Address >= FAKE_MEM_BASE) && (Address + sizeof (UINT32) <= FAKE_MEM_BASE + FAKE_WINDOW_SIZE));
  ASSERT ((Address & 3) == 0);
  return &mRegs[(Address - FAKE_MEM_BASE) / sizeof (UINT32)];
}

UINT32
EFIAPI
MmioRead32 (
  IN UINTN  Address
  )
{
  if (Address == FAKE_MEM_BASE + UFS_MEM_MPHY_PCS_READY_OFFSET) {
    mPcsReads++;
    if ((mPcsReadyAfterReads != 0) && (mPcsReads >= mPcsReadyAfterReads)) {
      *FakeReg (Address) |= UFS_MEM_MPHY_PCS_READY;
    }
  }

  return *FakeReg (Address);
}

UINT32
EFIAPI
MmioWrite32 (
  IN UINTN   Address,
  IN UINT32  Value
  )
{
  *FakeReg (Address) = Value;
  if (mWriteCount < MAX_WRITES) {
    mWrites[mWriteCount].Offset = (UINT32)(Address - FAKE_MEM_BASE);
    mWrites[mWriteCount].Value  = Value;
    mWriteCount++;
  } else {
    mWriteOverflow = TRUE;
  }

  return Value;
}

UINT32
EFIAPI
MmioOr32 (
  IN UINTN   Address,
  IN UINT32  OrData
  )
{
  return MmioWrite32 (Address, *FakeReg (Address) | OrData);
}

UINT32
EFIAPI
MmioAnd32 (
  IN UINTN   Address,
  IN UINT32  AndData
  )
{
  return MmioWrite32 (Address, *FakeReg (Address) & AndData);
}

UINT32
EFIAPI
MmioAndThenOr32 (
  IN UINTN   Address,
  IN UINT32  AndData,
  IN UINT32  OrData
  )
{
  return MmioWrite32 (Address, (*FakeReg (Address) & AndData) | OrData);
}

UINTN
EFIAPI
MicroSecondDelay (
  IN UINTN  MicroSeconds
  )
{
  mDelayUs += MicroSeconds;
  return MicroSeconds;
}

UINTN
EFIAPI
NanoSecondDelay (
  IN UINTN  NanoSeconds
  )
{
  mDelayUs += NanoSeconds / 1000;
  return NanoSeconds;
}

STATIC
VOID
FakeRegsReset (
  VOID
  )
{
  ZeroMem (mRegs, sizeof (mRegs));
  ZeroMem (mWrites, sizeof (mWrites));
  mWriteCount         = 0;
  mWriteOverflow      = FALSE;
  mPcsReadyAfterReads = 0;
  mPcsReads           = 0;
  mDelayUs            = 0;
}

// ---------------------------------------------------------------------------
// DTB_EXTN_PROTOCOL mock
// ---------------------------------------------------------------------------

#define NODE_PREFIX      "/soc/UfsController0/"
#define MAX_PHY_ENTRIES  64
#define MAX_VENDORS      40

typedef enum {
  NodeBase,
  NodeSpeed,
  NodeResource,
  NodeTimeout,
  NodeRegister,
  NodeStructMax,
  NodePhy = NodeStructMax,
  NodeVendorList,
  NodeMax,
  NodeVendorFirst = 100
} MOCK_NODE_ID;

STATIC CONST CHAR8  *CONST  mNodeNames[NodeMax] = {
  "BaseConfig",
  "SpeedConfig",
  "ResourceConfig",
  "TimeoutConfig",
  "RegisterConfig",
  "PhyConfig",
  "DeEmphasisVendorList"
};

STATIC CONST CHAR8  *CONST  mNodeFormats[NodeStructMax] = {
  "ddbbbbb", "bbbbbbb", "b", "wwww", "ddd"
};

STATIC CONST CHAR8  *CONST  mPhyNames[UfsPhyTableTypeMax] = {
  "InitTable", "Lane0Table", "Lane1Table", "EndTable", "HsLss"
};

typedef struct {
  BOOLEAN    Present;
  UINT32     PackedSize;          // FdtGetPropValuesSizeOfNode
  INT32      ValuesError;         // FdtGetPropValuesOfNode result
  UINT8      Values[32];          // host-order structure contents
} MOCK_STRUCT_NODE;

typedef struct {
  BOOLEAN          Present;
  UINT32           Size;          // bytes
  UFS_PHY_TABLE    Entries[MAX_PHY_ENTRIES];
} MOCK_PHY_PROP;

typedef struct {
  CHAR8     VendorId[16];
  UINT32    VendorIdSize;
  CHAR8     ModelId[64];
  UINT32    ModelIdSize;
} MOCK_VENDOR;

typedef struct {
  BOOLEAN             ProtocolPresent;
  MOCK_STRUCT_NODE    Struct[NodeStructMax];
  BOOLEAN             PhyPresent;
  MOCK_PHY_PROP       Phy[UfsPhyTableTypeMax];
  BOOLEAN             VendorListPresent;
  UINT32              VendorCount;
  MOCK_VENDOR         Vendor[MAX_VENDORS];
  CHAR8               LastFormat[NodeStructMax][16];
} MOCK_DT;

STATIC MOCK_DT            mDt;
STATIC DTB_EXTN_PROTOCOL  mDtbExtn;
STATIC CONST CHAR8        mDtbBlob[] = "mock";

STATIC
INT32
MockNodeId (
  IN DTB_EXTN_NODE_HANDLE  *Node
  )
{
  if ((Node == NULL) || (Node->Blob != mDtbBlob)) {
    return -1;
  }

  return Node->Offset;
}

STATIC
INT32
MockGetNodeHandle (
  IN OUT DTB_EXTN_NODE_HANDLE  *Node,
  IN CHAR8                     *Name
  )
{
  UINTN  Id;
  UINTN  PrefixLen;

  PrefixLen = AsciiStrLen (NODE_PREFIX);
  if ((Node == NULL) || (Name == NULL) || (AsciiStrnCmp (Name, NODE_PREFIX, PrefixLen) != 0)) {
    return -1;
  }

  for (Id = 0; Id < NodeMax; Id++) {
    if (AsciiStrCmp (Name + PrefixLen, mNodeNames[Id]) == 0) {
      break;
    }
  }

  if ((Id == NodeMax) ||
      ((Id < NodeStructMax) && !mDt.Struct[Id].Present) ||
      ((Id == NodePhy) && !mDt.PhyPresent) ||
      ((Id == NodeVendorList) && !mDt.VendorListPresent))
  {
    return -1;                                   // -FDT_ERR_NOTFOUND
  }

  Node->Blob   = mDtbBlob;
  Node->Offset = (INT32)Id;
  return DTB_ERR_NOERROR;
}

STATIC
INT32
MockGetPropValuesSizeOfNode (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN OUT UINT32            *Size
  )
{
  INT32  Id;

  Id = MockNodeId (Node);
  if ((Id < 0) || (Id >= NodeStructMax) || (Size == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  *Size = mDt.Struct[Id].PackedSize;
  return DTB_ERR_NOERROR;
}

STATIC
INT32
MockGetPropValuesOfNode (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CHAR8                 *Format,
  IN OUT VOID              *PackedPropValues,
  IN UINT32                Size
  )
{
  INT32  Id;

  Id = MockNodeId (Node);
  if ((Id < 0) || (Id >= NodeStructMax) || (Format == NULL) || (PackedPropValues == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  AsciiStrCpyS (mDt.LastFormat[Id], sizeof (mDt.LastFormat[Id]), Format);
  if (mDt.Struct[Id].ValuesError != DTB_ERR_NOERROR) {
    return mDt.Struct[Id].ValuesError;
  }

  if (Size < mDt.Struct[Id].PackedSize) {
    return -DTB_ERR_BUF2SMALL;
  }

  CopyMem (PackedPropValues, mDt.Struct[Id].Values, mDt.Struct[Id].PackedSize);
  return DTB_ERR_NOERROR;
}

STATIC
MOCK_PHY_PROP *
MockPhyProp (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName
  )
{
  UINTN  Index;

  if ((MockNodeId (Node) != NodePhy) || (PropName == NULL)) {
    return NULL;
  }

  for (Index = 0; Index < UfsPhyTableTypeMax; Index++) {
    if ((AsciiStrCmp (PropName, mPhyNames[Index]) == 0) && mDt.Phy[Index].Present) {
      return &mDt.Phy[Index];
    }
  }

  return NULL;
}

STATIC
MOCK_VENDOR *
MockVendor (
  IN DTB_EXTN_NODE_HANDLE  *Node
  )
{
  INT32  Id;

  Id = MockNodeId (Node);
  if ((Id < NodeVendorFirst) || ((UINT32)(Id - NodeVendorFirst) >= mDt.VendorCount)) {
    return NULL;
  }

  return &mDt.Vendor[Id - NodeVendorFirst];
}

STATIC
INT32
MockGetPropSize (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT UINT32            *Size
  )
{
  MOCK_PHY_PROP  *Phy;
  MOCK_VENDOR    *Vendor;

  if (Size == NULL) {
    return -DTB_ERR_NULLPTR;
  }

  Phy = MockPhyProp (Node, PropName);
  if (Phy != NULL) {
    *Size = Phy->Size;
    return DTB_ERR_NOERROR;
  }

  Vendor = MockVendor (Node);
  if ((Vendor != NULL) && (AsciiStrCmp (PropName, "VendorId") == 0)) {
    *Size = Vendor->VendorIdSize;
    return DTB_ERR_NOERROR;
  }

  if ((Vendor != NULL) && (AsciiStrCmp (PropName, "ModelId") == 0)) {
    *Size = Vendor->ModelIdSize;
    return DTB_ERR_NOERROR;
  }

  return -1;
}

STATIC
INT32
MockGetUint16PropList (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT UINT16            *PropList,
  IN UINT32                Size
  )
{
  MOCK_PHY_PROP  *Phy;

  Phy = MockPhyProp (Node, PropName);
  if (Phy == NULL) {
    return -1;
  }

  if ((PropList == NULL) || (Size < Phy->Size)) {
    return -DTB_ERR_BUF2SMALL;
  }

  CopyMem (PropList, Phy->Entries, Phy->Size);
  return DTB_ERR_NOERROR;
}

STATIC
INT32
MockGetStringPropList (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN CONST CHAR8           *PropName,
  IN OUT CHAR8             *StringList,
  IN UINT32                Size
  )
{
  MOCK_VENDOR  *Vendor;
  CONST CHAR8  *Src;
  UINT32       SrcSize;

  Vendor = MockVendor (Node);
  if ((Vendor == NULL) || (StringList == NULL)) {
    return -1;
  }

  if (AsciiStrCmp (PropName, "VendorId") == 0) {
    Src     = Vendor->VendorId;
    SrcSize = Vendor->VendorIdSize;
  } else if (AsciiStrCmp (PropName, "ModelId") == 0) {
    Src     = Vendor->ModelId;
    SrcSize = Vendor->ModelIdSize;
  } else {
    return -1;
  }

  if (Size < SrcSize) {
    return -DTB_ERR_BUF2SMALL;
  }

  CopyMem (StringList, Src, SrcSize);
  return DTB_ERR_NOERROR;
}

STATIC
INT32
MockGetCountOfSubnodes (
  IN DTB_EXTN_NODE_HANDLE  *Node,
  IN OUT UINT32            *Count
  )
{
  if ((MockNodeId (Node) != NodeVendorList) || (Count == NULL)) {
    return -DTB_ERR_NULLPTR;
  }

  *Count = mDt.VendorCount;
  return DTB_ERR_NOERROR;
}

STATIC
INT32
MockGetCacheOfSubnodes (
  IN DTB_EXTN_NODE_HANDLE      *Node,
  IN OUT DTB_EXTN_NODE_HANDLE  *Cache,
  IN UINT32                    Count
  )
{
  UINT32  Index;

  if ((MockNodeId (Node) != NodeVendorList) || (Cache == NULL) || (Count < mDt.VendorCount)) {
    return -DTB_ERR_NULLPTR;
  }

  for (Index = 0; Index < mDt.VendorCount; Index++) {
    Cache[Index].Blob   = mDtbBlob;
    Cache[Index].Offset = (INT32)(NodeVendorFirst + Index);
  }

  return DTB_ERR_NOERROR;
}

STATIC
EFI_STATUS
EFIAPI
MockLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration OPTIONAL,
  OUT VOID      **Interface
  )
{
  if (!CompareGuid (Protocol, &gDtbExtnProtocolGuid) || !mDt.ProtocolPresent) {
    *Interface = NULL;
    return EFI_NOT_FOUND;
  }

  *Interface = &mDtbExtn;
  return EFI_SUCCESS;
}

//
// The configuration the tests start from: one lane, HS-G4 rate B, nominal
// corner, the same layout as the QEMU lab device tree.
//
STATIC CONST UFS_BASE_CONFIG  mGoodBase = {
  FAKE_MEM_BASE, FAKE_MEM_BASE, 1, 1, 0, 1, 0
};

STATIC CONST UFS_SPEED_CONFIG  mGoodSpeed = {
  UfsRateB, UfsHsGear4, TRUE, 1, 0, UfsLinkStartupModeLsLss, FALSE
};

STATIC CONST UFS_RESOURCE_CONFIG  mGoodResource = { UfsVoltageCornerNominal };

STATIC CONST UFS_TIMEOUT_CONFIG  mGoodTimeout = { 1000, 1000, 1000, 1000 };

STATIC CONST UFS_REGISTER_CONFIG  mGoodRegister = {
  FAKE_MEM_BASE + 0x4100, FAKE_MEM_BASE + 0x4104, FAKE_MEM_BASE + 0x4108
};

STATIC
VOID
MockSetStruct (
  IN MOCK_NODE_ID  Id,
  IN CONST VOID    *Values,
  IN UINT32        Size
  )
{
  mDt.Struct[Id].Present     = TRUE;
  mDt.Struct[Id].PackedSize  = Size;
  mDt.Struct[Id].ValuesError = DTB_ERR_NOERROR;
  CopyMem (mDt.Struct[Id].Values, Values, Size);
}

STATIC
VOID
MockSetPhy (
  IN UFS_PHY_TABLE_TYPE   Type,
  IN CONST UFS_PHY_TABLE  *Entries,
  IN UINT32               Count
  )
{
  ASSERT (Count <= MAX_PHY_ENTRIES);
  mDt.Phy[Type].Present = TRUE;
  mDt.Phy[Type].Size    = Count * sizeof (UFS_PHY_TABLE);
  CopyMem (mDt.Phy[Type].Entries, Entries, mDt.Phy[Type].Size);
}

STATIC
VOID
MockSetVendor (
  IN UINT32       Index,
  IN CONST CHAR8  *VendorId,
  IN CONST CHAR8  *ModelIds,
  IN UINT32       ModelIdsSize
  )
{
  MOCK_VENDOR  *Vendor;

  Vendor               = &mDt.Vendor[Index];
  Vendor->VendorIdSize = (UINT32)AsciiStrSize (VendorId);
  CopyMem (Vendor->VendorId, VendorId, Vendor->VendorIdSize);
  Vendor->ModelIdSize = ModelIdsSize;
  CopyMem (Vendor->ModelId, ModelIds, ModelIdsSize);
}

//
// PHY tables: Init has a rate-A-only and a rate-B-only entry, Lane0 a
// gear-4, a gear-5 and a default-gear entry, End a stop marker followed by
// an entry that must not be written.
//
#define PHY_FLAG_RATE_A   (UfsRateA << UFS_RATE_SHIFT)
#define PHY_FLAG_RATE_B   (UfsRateB << UFS_RATE_SHIFT)
#define PHY_FLAG_GEAR4    (UfsGearMode4 << UFS_GEAR_SHIFT)
#define PHY_FLAG_GEAR5    (UfsGearMode5 << UFS_GEAR_SHIFT)
#define PHY_FLAG_GEAR_DF  (UfsGearModeDefault << UFS_GEAR_SHIFT)

STATIC CONST UFS_PHY_TABLE  mInitTable[] = {
  { 0,                             0x3000, 0x01 },
  { PHY_FLAG_RATE_A,               0x3004, 0xA0 },
  { PHY_FLAG_RATE_B,               0x3008, 0xB0 },
  { 0,                             0x300C, 0x02 },
  { UFS_PHY_CONFIG_STOP_CONDITION, 0,      0    }
};

STATIC CONST UFS_PHY_TABLE  mLane0Table[] = {
  { PHY_FLAG_GEAR4,                0x3400, 0x40 },
  { PHY_FLAG_GEAR5,                0x3404, 0x50 },
  { PHY_FLAG_GEAR_DF,              0x3408, 0xDF },
  { UFS_PHY_CONFIG_STOP_CONDITION, 0,      0    }
};

STATIC CONST UFS_PHY_TABLE  mLane1Table[] = {
  { 0,                             0x3800, 0x11 },
  { UFS_PHY_CONFIG_STOP_CONDITION, 0,      0    }
};

STATIC CONST UFS_PHY_TABLE  mEndTable[] = {
  { 0,                             0x3C00, 0xE0  },
  { UFS_PHY_CONFIG_STOP_CONDITION, 0,      0     },
  { 0,                             0x3C04, 0xBAD }
};

STATIC CONST UFS_PHY_TABLE  mHsLssTable[] = {
  { 0,                             0x3010, 0x55 },
  { UFS_PHY_CONFIG_STOP_CONDITION, 0,      0    }
};

STATIC CONST CHAR8  mModelIds[] = "MODEL_A\0MODEL_B";

STATIC
VOID
MockDtReset (
  VOID
  )
{
  ZeroMem (&mDt, sizeof (mDt));
  mDt.ProtocolPresent = TRUE;
  MockSetStruct (NodeBase, &mGoodBase, sizeof (mGoodBase));
  MockSetStruct (NodeSpeed, &mGoodSpeed, sizeof (mGoodSpeed));
  MockSetStruct (NodeResource, &mGoodResource, sizeof (mGoodResource));
  MockSetStruct (NodeTimeout, &mGoodTimeout, sizeof (mGoodTimeout));
  MockSetStruct (NodeRegister, &mGoodRegister, sizeof (mGoodRegister));
  mDt.PhyPresent = TRUE;
  MockSetPhy (UfsPhyTableInit, mInitTable, ARRAY_SIZE (mInitTable));
  MockSetPhy (UfsPhyTableInitLane0, mLane0Table, ARRAY_SIZE (mLane0Table));
  MockSetPhy (UfsPhyTableInitLane1, mLane1Table, ARRAY_SIZE (mLane1Table));
  MockSetPhy (UfsPhyTableInitEnd, mEndTable, ARRAY_SIZE (mEndTable));
  MockSetPhy (UfsPhyTableHsLss, mHsLssTable, ARRAY_SIZE (mHsLssTable));
  mDt.VendorListPresent = TRUE;
  mDt.VendorCount       = 1;
  MockSetVendor (0, "VENDOR1", mModelIds, sizeof (mModelIds));

  ZeroMem (&mDtbExtn, sizeof (mDtbExtn));
  mDtbExtn.FdtGetNodeHandle           = MockGetNodeHandle;
  mDtbExtn.FdtGetPropSize             = MockGetPropSize;
  mDtbExtn.FdtGetPropValuesSizeOfNode = MockGetPropValuesSizeOfNode;
  mDtbExtn.FdtGetPropValuesOfNode     = MockGetPropValuesOfNode;
  mDtbExtn.FdtGetStringPropList       = MockGetStringPropList;
  mDtbExtn.FdtGetUint16PropList       = MockGetUint16PropList;
  mDtbExtn.FdtGetCountOfSubnodes      = MockGetCountOfSubnodes;
  mDtbExtn.FdtGetCacheOfSubnodes      = MockGetCacheOfSubnodes;
  gBS->LocateProtocol                 = MockLocateProtocol;
}

/**
  Prerequisite of every test case: the good device tree and an empty
  register window.
**/
STATIC
UNIT_TEST_STATUS
EFIAPI
TestSetup (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  MockDtReset ();
  FakeRegsReset ();
  return UNIT_TEST_PASSED;
}

//
// A configuration that failed to load must hold no allocations.
//
STATIC
BOOLEAN
ConfigIsEmpty (
  IN CONST UFS_CONFIG_DATA  *Config
  )
{
  UINTN  Index;

  for (Index = 0; Index < UfsPhyTableTypeMax; Index++) {
    if ((Config->PhyConfig.PhyTable[Index].Entries != NULL) || (Config->PhyConfig.PhyTable[Index].NumEntries != 0)) {
      return FALSE;
    }
  }

  return (Config->DeEmphasisVendorList.List == NULL) && (Config->DeEmphasisVendorList.NumVendors == 0);
}

STATIC
EFI_STATUS
LoadConfig (
  OUT UFS_CONFIG_DATA  *Config
  )
{
  ZeroMem (Config, sizeof (*Config));
  return UfsGetConfigData (UfsController0, Config);
}

// ---------------------------------------------------------------------------
// Configuration resolution (device tree source)
// ---------------------------------------------------------------------------

UNIT_TEST_STATUS
EFIAPI
BaseConfigFromDt (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_BASE_CONFIG  Base;

  UT_ASSERT_NOT_EFI_ERROR (UfsGetBaseConfig (UfsController0, &Base));
  UT_ASSERT_MEM_EQUAL (&Base, &mGoodBase, sizeof (Base));
  UT_ASSERT_EQUAL (AsciiStrCmp (mDt.LastFormat[NodeBase], "ddbbbbb"), 0);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
BaseConfigInvalidParameters (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_BASE_CONFIG  Base;
  UFS_CONFIG_DATA  Config;

  UT_ASSERT_STATUS_EQUAL (UfsGetBaseConfig (UfsNumControllers, &Base), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsGetBaseConfig (UfsController0, NULL), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsNumControllers, &Base), EFI_INVALID_PARAMETER);
  ZeroMem (&Config, sizeof (Config));
  UT_ASSERT_STATUS_EQUAL (UfsGetConfigData (UfsNumControllers, &Config), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsGetConfigData (UfsController0, NULL), EFI_INVALID_PARAMETER);
  return UNIT_TEST_PASSED;
}

//
// Without a usable BaseConfig node the PCDs are used (PreHce and PostLinkUp
// disabled in the DSC for this variant).
//
STATIC
UNIT_TEST_STATUS
CheckPcdBase (
  IN CONST UFS_BASE_CONFIG  *Base
  )
{
  UT_ASSERT_EQUAL (Base->UfsMemBase, FixedPcdGet64 (PcdUfs0MemBaseAddress));
  UT_ASSERT_EQUAL (Base->UfsHciBase, FixedPcdGet64 (PcdUfs0MemBaseAddress) + FixedPcdGet32 (PcdUfs0HciOffset));
  UT_ASSERT_EQUAL (Base->PreHceEnable, FixedPcdGetBool (PcdUfs0PreHceCallbackEn));
  UT_ASSERT_EQUAL (Base->PostLinkUpEnable, FixedPcdGetBool (PcdUfs0PostLinkCallbackEn));
  UT_ASSERT_EQUAL (Base->RefClkFreq, FixedPcdGet8 (PcdUfs0RefClkFreq));
  UT_ASSERT_EQUAL (Base->SkipHceReenable, FixedPcdGetBool (PcdUfs0SkipHceReenable));
  UT_ASSERT_EQUAL (Base->SkipLinkStartup, FixedPcdGetBool (PcdUfs0SkipLinkStartup));
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
BaseConfigPcdFallback (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_BASE_CONFIG  Base;

  // No BaseConfig node
  mDt.Struct[NodeBase].Present = FALSE;
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_NOT_FOUND);
  UT_ASSERT_NOT_EFI_ERROR (UfsGetBaseConfig (UfsController0, &Base));
  UT_ASSERT_EQUAL (CheckPcdBase (&Base), UNIT_TEST_PASSED);

  // No DTB_EXTN_PROTOCOL
  MockDtReset ();
  mDt.ProtocolPresent = FALSE;
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_NOT_FOUND);
  UT_ASSERT_NOT_EFI_ERROR (UfsGetBaseConfig (UfsController0, &Base));
  UT_ASSERT_EQUAL (CheckPcdBase (&Base), UNIT_TEST_PASSED);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
BaseConfigMalformed (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_BASE_CONFIG  Base;
  UFS_BASE_CONFIG  ZeroBase;

  // A property missing (20 instead of 21 bytes)
  mDt.Struct[NodeBase].PackedSize = sizeof (UFS_BASE_CONFIG) - 1;
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_COMPROMISED_DATA);

  // An extra property
  mDt.Struct[NodeBase].PackedSize = sizeof (UFS_BASE_CONFIG) + 1;
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_COMPROMISED_DATA);

  // DTB parse error
  MockDtReset ();
  mDt.Struct[NodeBase].ValuesError = -DTB_ERR_BADFORMAT;
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_DEVICE_ERROR);

  // Zero base address
  MockDtReset ();
  CopyMem (&ZeroBase, &mGoodBase, sizeof (ZeroBase));
  ZeroBase.UfsHciBase = 0;
  MockSetStruct (NodeBase, &ZeroBase, sizeof (ZeroBase));
  UT_ASSERT_STATUS_EQUAL (UfsDtGetBaseConfig (UfsController0, &Base), EFI_NOT_FOUND);

  // A node without base addresses counts as missing: the PCDs are used
  UT_ASSERT_NOT_EFI_ERROR (UfsGetBaseConfig (UfsController0, &Base));
  UT_ASSERT_EQUAL (CheckPcdBase (&Base), UNIT_TEST_PASSED);

  // A malformed or unparsable node is an error, not a PCD fallback
  MockDtReset ();
  mDt.Struct[NodeBase].PackedSize = sizeof (UFS_BASE_CONFIG) - 1;
  UT_ASSERT_STATUS_EQUAL (UfsGetBaseConfig (UfsController0, &Base), EFI_COMPROMISED_DATA);
  MockDtReset ();
  mDt.Struct[NodeBase].ValuesError = -DTB_ERR_BADFORMAT;
  UT_ASSERT_STATUS_EQUAL (UfsGetBaseConfig (UfsController0, &Base), EFI_DEVICE_ERROR);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
ConfigDataFromDt (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONFIG_DATA  Config;
  UINTN            Index;

  UT_ASSERT_NOT_EFI_ERROR (LoadConfig (&Config));
  UT_ASSERT_MEM_EQUAL (&Config.SpeedConfig, &mGoodSpeed, sizeof (mGoodSpeed));
  UT_ASSERT_MEM_EQUAL (&Config.ResourceConfig, &mGoodResource, sizeof (mGoodResource));
  UT_ASSERT_MEM_EQUAL (&Config.TimeoutConfig, &mGoodTimeout, sizeof (mGoodTimeout));
  UT_ASSERT_MEM_EQUAL (&Config.RegisterConfig, &mGoodRegister, sizeof (mGoodRegister));
  for (Index = 0; Index < NodeStructMax; Index++) {
    if (Index != NodeBase) {
      UT_ASSERT_EQUAL (AsciiStrCmp (mDt.LastFormat[Index], mNodeFormats[Index]), 0);
    }
  }

  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInit].NumEntries, ARRAY_SIZE (mInitTable));
  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInitLane0].NumEntries, ARRAY_SIZE (mLane0Table));
  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInitLane1].NumEntries, ARRAY_SIZE (mLane1Table));
  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInitEnd].NumEntries, ARRAY_SIZE (mEndTable));
  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableHsLss].NumEntries, ARRAY_SIZE (mHsLssTable));
  UT_ASSERT_MEM_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInitLane0].Entries, mLane0Table, sizeof (mLane0Table));
  //
  // The window covers the highest entry before each table's stop marker
  // (Lane1 0x3800, End 0x3C00; End's 0x3C04 is after its stop marker).
  //
  UT_ASSERT_EQUAL (Config.PhyConfig.RegisterWindowSize, 0x3C04);

  UT_ASSERT_EQUAL (Config.DeEmphasisVendorList.NumVendors, 1);
  UT_ASSERT_NOT_NULL (Config.DeEmphasisVendorList.List);
  UT_ASSERT_EQUAL (AsciiStrCmp (Config.DeEmphasisVendorList.List[0].VendorName, "VENDOR1"), 0);
  UT_ASSERT_EQUAL (Config.DeEmphasisVendorList.List[0].ProductListSize, sizeof (mModelIds));
  UT_ASSERT_MEM_EQUAL (Config.DeEmphasisVendorList.List[0].Products, mModelIds, sizeof (mModelIds));

  UT_ASSERT_NOT_NULL (Config.PmConfig);
  UT_ASSERT_EQUAL (Config.PmConfig->VoltageCorner, UfsVoltageCornerNominal);
  UT_ASSERT_EQUAL (Config.PmConfig->MaxHsGear, UfsHsGear5);
  UT_ASSERT_EQUAL (Config.PmConfig->TimingConfig.DmeMax1uS, 403);

  // A second load replaces the first one's tables
  UT_ASSERT_NOT_EFI_ERROR (UfsGetConfigData (UfsController0, &Config));
  UT_ASSERT_EQUAL (Config.PhyConfig.PhyTable[UfsPhyTableInit].NumEntries, ARRAY_SIZE (mInitTable));

  UfsFreeConfigData (&Config);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  UT_ASSERT_EQUAL ((UINTN)Config.PmConfig, 0);
  UfsFreeConfigData (&Config);           // freeing twice is safe
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
ConfigDataEmptyVendorList (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONFIG_DATA  Config;

  mDt.VendorCount = 0;
  UT_ASSERT_NOT_EFI_ERROR (LoadConfig (&Config));
  UT_ASSERT_EQUAL (Config.DeEmphasisVendorList.NumVendors, 0);
  UT_ASSERT_EQUAL ((UINTN)Config.DeEmphasisVendorList.List, 0);
  UfsFreeConfigData (&Config);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
ConfigDataMissingNode (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONFIG_DATA  Config;
  UINTN            Id;

  for (Id = NodeSpeed; Id < NodeMax; Id++) {
    MockDtReset ();
    if (Id < NodeStructMax) {
      mDt.Struct[Id].Present = FALSE;
    } else if (Id == NodePhy) {
      mDt.PhyPresent = FALSE;
    } else {
      mDt.VendorListPresent = FALSE;
    }

    DEBUG ((DEBUG_INFO, "Without %a\n", mNodeNames[Id]));
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_NOT_FOUND);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  }

  // Each PHY table is required
  for (Id = 0; Id < UfsPhyTableTypeMax; Id++) {
    MockDtReset ();
    mDt.Phy[Id].Present = FALSE;
    DEBUG ((DEBUG_INFO, "Without %a\n", mPhyNames[Id]));
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_NOT_FOUND);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  }

  MockDtReset ();
  mDt.ProtocolPresent = FALSE;
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_NOT_FOUND);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
ConfigDataMalformed (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONFIG_DATA  Config;
  UINTN            Id;
  UFS_PHY_TABLE    Bad[2];

  // Each structured node with one byte too few, and failing to parse
  for (Id = NodeSpeed; Id < NodeStructMax; Id++) {
    MockDtReset ();
    mDt.Struct[Id].PackedSize--;
    DEBUG ((DEBUG_INFO, "Short %a\n", mNodeNames[Id]));
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));

    MockDtReset ();
    mDt.Struct[Id].ValuesError = -DTB_ERR_BADFORMAT;
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_DEVICE_ERROR);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  }

  // PHY table not a whole number of entries, and empty
  MockDtReset ();
  mDt.Phy[UfsPhyTableInitLane0].Size -= 2;
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  MockDtReset ();
  mDt.Phy[UfsPhyTableInitEnd].Size = 0;
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));

  // PHY table larger than UFS_PHY_TABLE_MAX_SIZE
  MockDtReset ();
  mDt.Phy[UfsPhyTableInit].Size = UFS_PHY_TABLE_MAX_SIZE + sizeof (UFS_PHY_TABLE) - (UFS_PHY_TABLE_MAX_SIZE % sizeof (UFS_PHY_TABLE));
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));

  // Unaligned offset, and an offset past the 64 KiB register window
  Bad[1].Flags  = UFS_PHY_CONFIG_STOP_CONDITION;
  Bad[1].Offset = 0;
  Bad[1].Value  = 0;
  Bad[0].Flags  = 0;
  Bad[0].Value  = 1;
  Bad[0].Offset = 0x3002;
  MockDtReset ();
  MockSetPhy (UfsPhyTableInit, Bad, 2);
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  Bad[0].Offset = 0xFFFC;
  MockDtReset ();
  MockSetPhy (UfsPhyTableInit, Bad, 2);
  UT_ASSERT_NOT_EFI_ERROR (LoadConfig (&Config));    // last word of the window
  UT_ASSERT_EQUAL (Config.PhyConfig.RegisterWindowSize, SIZE_64KB);
  UfsFreeConfigData (&Config);

  // Too many de-emphasis vendors
  MockDtReset ();
  mDt.VendorCount = UFS_MAX_VENDOR_COUNT + 1;
  for (Id = 0; Id < mDt.VendorCount; Id++) {
    MockSetVendor ((UINT32)Id, "V", mModelIds, sizeof (mModelIds));
  }

  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));

  // Empty ModelId, and a VendorId longer than the name limit
  MockDtReset ();
  mDt.Vendor[0].ModelIdSize = 0;
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  MockDtReset ();
  MockSetVendor (0, "VENDOR_TOO_LONG", mModelIds, sizeof (mModelIds));
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
ConfigDataOutOfRange (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONFIG_DATA      Config;
  UFS_SPEED_CONFIG     Speed;
  UFS_TIMEOUT_CONFIG   Timeout;
  UFS_RESOURCE_CONFIG  Resource;
  UINTN                Index;

  // Lanes 0 and 5, HS gear 0 and 6, HS rate undefined, link start-up mode 2
  for (Index = 0; Index < 6; Index++) {
    MockDtReset ();
    CopyMem (&Speed, &mGoodSpeed, sizeof (Speed));
    switch (Index) {
      case 0:
        Speed.NumLanes = 0;
        break;
      case 1:
        Speed.NumLanes = UFS_MAX_DATA_LANES + 1;
        break;
      case 2:
        Speed.Gear = 0;
        break;
      case 3:
        Speed.Gear = UfsHsGearMax + 1;
        break;
      case 4:
        Speed.Rate = UfsRateUndefined;
        break;
      default:
        Speed.LinkStartupMode = UfsLinkStartupModeMax;
        break;
    }

    MockSetStruct (NodeSpeed, &Speed, sizeof (Speed));
    DEBUG ((DEBUG_INFO, "Speed case %u\n", (UINT32)Index));
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  }

  // Each timeout 0
  for (Index = 0; Index < 4; Index++) {
    MockDtReset ();
    CopyMem (&Timeout, &mGoodTimeout, sizeof (Timeout));
    ((UINT32 *)&Timeout)[Index] = 0;
    MockSetStruct (NodeTimeout, &Timeout, sizeof (Timeout));
    UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_COMPROMISED_DATA);
    UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  }

  // Voltage corner undefined and past the last one
  MockDtReset ();
  Resource.VoltageCorner = UfsVoltageCornerUndefined;
  MockSetStruct (NodeResource, &Resource, sizeof (Resource));
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_INVALID_PARAMETER);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  Resource.VoltageCorner = UfsVoltageCornerMax;
  MockSetStruct (NodeResource, &Resource, sizeof (Resource));
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_INVALID_PARAMETER);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
PmConfigLookup (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_PM_CONFIG  *Pm;
  UINT8          Corner;

  for (Corner = UfsVoltageCornerMinSVS; Corner < UfsVoltageCornerMax; Corner++) {
    Pm = NULL;
    UT_ASSERT_NOT_EFI_ERROR (UfsGetPmConfig (Corner, &Pm));
    UT_ASSERT_NOT_NULL (Pm);
    UT_ASSERT_EQUAL (Pm->VoltageCorner, Corner);
  }

  UT_ASSERT_STATUS_EQUAL (UfsGetPmConfig (UfsVoltageCornerSVS, NULL), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsGetPmConfig (UfsVoltageCornerUndefined, &Pm), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsGetPmConfig (UfsVoltageCornerMax, &Pm), EFI_INVALID_PARAMETER);
  return UNIT_TEST_PASSED;
}

// ---------------------------------------------------------------------------
// PCD-only build (PcdUfsConfigSourceDeviceTree = 0, PreHce enabled)
// ---------------------------------------------------------------------------

UNIT_TEST_STATUS
EFIAPI
PcdOnlyConfig (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_BASE_CONFIG  Base;
  UFS_CONFIG_DATA  Config;

  // The DT is there, but this build does not use it
  UT_ASSERT_STATUS_EQUAL (LoadConfig (&Config), EFI_UNSUPPORTED);
  UT_ASSERT_TRUE (ConfigIsEmpty (&Config));

  // The PCD fallback must not enable a bring-up phase
  UT_ASSERT_TRUE (FixedPcdGetBool (PcdUfs0PreHceCallbackEn));
  UT_ASSERT_STATUS_EQUAL (UfsGetBaseConfig (UfsController0, &Base), EFI_UNSUPPORTED);
  return UNIT_TEST_PASSED;
}

// ---------------------------------------------------------------------------
// PHY programming
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
LoadController (
  OUT UFS_CONTROLLER_DATA  *Ctl
  )
{
  ZeroMem (Ctl, sizeof (*Ctl));
  Ctl->UfsMemBase = FAKE_MEM_BASE;
  Ctl->UfsHciBase = FAKE_MEM_BASE;
  Ctl->Version    = UFS_VERSION_3_1;
  return UfsGetConfigData (UfsController0, &Ctl->Config);
}

STATIC
UNIT_TEST_STATUS
CheckWrites (
  IN CONST UINT32  *Expected,
  IN UINTN         Count
  )
{
  UINTN  Index;

  UT_ASSERT_FALSE (mWriteOverflow);
  UT_ASSERT_EQUAL (mWriteCount, Count);
  for (Index = 0; Index < Count; Index++) {
    DEBUG ((DEBUG_INFO, "write %u: 0x%04x <- 0x%x\n", (UINT32)Index, mWrites[Index].Offset, mWrites[Index].Value));
    UT_ASSERT_EQUAL (mWrites[Index].Offset, Expected[2 * Index]);
    UT_ASSERT_EQUAL (mWrites[Index].Value, Expected[2 * Index + 1]);
  }

  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
PhyTableOrder (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONTROLLER_DATA  Ctl;
  //
  // Rate B, HS-G4, one lane, LS-LSS: Init (unconditional and rate B), Lane0
  // (gear 4), End up to the stop marker. No Lane1, no HsLss.
  //
  STATIC CONST UINT32  Expected1[] = {
    0x3000, 0x01, 0x3008, 0xB0, 0x300C, 0x02, 0x3400, 0x40, 0x3C00, 0xE0
  };
  //
  // Two configured lanes on UFS 3.1 add Lane1; HS-LSS adds HsLss last.
  //
  STATIC CONST UINT32  Expected2[] = {
    0x3000, 0x01, 0x3008, 0xB0, 0x300C, 0x02, 0x3400, 0x40, 0x3800, 0x11, 0x3C00, 0xE0, 0x3010, 0x55
  };

  UT_ASSERT_NOT_EFI_ERROR (LoadController (&Ctl));
  UT_ASSERT_NOT_EFI_ERROR (UfsMphyConfigure (&Ctl));
  UT_ASSERT_EQUAL (CheckWrites (Expected1, ARRAY_SIZE (Expected1) / 2), UNIT_TEST_PASSED);

  FakeRegsReset ();
  Ctl.Config.SpeedConfig.NumLanes        = UFS_LANE_COUNT_DUAL;
  Ctl.Config.SpeedConfig.LinkStartupMode = UfsLinkStartupModeHsLss;
  UT_ASSERT_NOT_EFI_ERROR (UfsMphyConfigure (&Ctl));
  UT_ASSERT_EQUAL (CheckWrites (Expected2, ARRAY_SIZE (Expected2) / 2), UNIT_TEST_PASSED);

  // Two lanes before UFS 3.1: no Lane1
  FakeRegsReset ();
  Ctl.Version                            = UFS_VERSION_3_1 - 1;
  Ctl.Config.SpeedConfig.LinkStartupMode = UfsLinkStartupModeLsLss;
  UT_ASSERT_NOT_EFI_ERROR (UfsMphyConfigure (&Ctl));
  UT_ASSERT_EQUAL (CheckWrites (Expected1, ARRAY_SIZE (Expected1) / 2), UNIT_TEST_PASSED);

  UfsFreeConfigData (&Ctl.Config);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
PhyEntryConditions (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONTROLLER_DATA  Ctl;
  UINTN                Index;
  STATIC CONST UINT32  RateA[]   = { 0x3000, 0x01, 0x3004, 0xA0, 0x300C, 0x02 };
  STATIC CONST UINT32  Gear5[]   = { 0x3404, 0x50 };
  STATIC CONST UINT32  PwmGear[] = { 0x3408, 0xDF };

  UT_ASSERT_NOT_EFI_ERROR (LoadController (&Ctl));

  Ctl.Config.SpeedConfig.Rate = UfsRateA;
  UT_ASSERT_NOT_EFI_ERROR (UfsPhyControl (&Ctl, UfsPhyTableInit));
  UT_ASSERT_EQUAL (CheckWrites (RateA, ARRAY_SIZE (RateA) / 2), UNIT_TEST_PASSED);

  FakeRegsReset ();
  Ctl.Config.SpeedConfig.Gear = UfsHsGear5;
  UT_ASSERT_NOT_EFI_ERROR (UfsPhyControl (&Ctl, UfsPhyTableInitLane0));
  UT_ASSERT_EQUAL (CheckWrites (Gear5, ARRAY_SIZE (Gear5) / 2), UNIT_TEST_PASSED);

  // PWM (HS off): only default-gear entries
  FakeRegsReset ();
  Ctl.Config.SpeedConfig.HsEnable = FALSE;
  Ctl.Config.SpeedConfig.Gear     = 1;
  UT_ASSERT_NOT_EFI_ERROR (UfsPhyControl (&Ctl, UfsPhyTableInitLane0));
  UT_ASSERT_EQUAL (CheckWrites (PwmGear, ARRAY_SIZE (PwmGear) / 2), UNIT_TEST_PASSED);

  // Bad arguments write nothing
  FakeRegsReset ();
  UT_ASSERT_STATUS_EQUAL (UfsPhyControl (NULL, UfsPhyTableInit), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsPhyControl (&Ctl, UfsPhyTableTypeMax), EFI_INVALID_PARAMETER);
  FreePool (Ctl.Config.PhyConfig.PhyTable[UfsPhyTableInit].Entries);
  Ctl.Config.PhyConfig.PhyTable[UfsPhyTableInit].Entries = NULL;
  UT_ASSERT_STATUS_EQUAL (UfsPhyControl (&Ctl, UfsPhyTableInit), EFI_INVALID_PARAMETER);
  UT_ASSERT_EQUAL (mWriteCount, 0);

  UT_ASSERT_STATUS_EQUAL (UfsMphyConfigure (NULL), EFI_INVALID_PARAMETER);
  UT_ASSERT_EQUAL (mWriteCount, 0);

  //
  // The PHY is programmed before HCE, without UIC commands: unread host
  // capabilities must not matter, and the lane count is the configured one
  // even if the capabilities say otherwise.
  //
  Ctl.Config.PhyConfig.PhyTable[UfsPhyTableInit].NumEntries = 0;
  Ctl.HostCaps.Valid                                        = FALSE;
  Ctl.HostCaps.TxLanes                                      = UFS_LANE_COUNT_DUAL;
  UT_ASSERT_NOT_EFI_ERROR (UfsMphyConfigure (&Ctl));
  UT_ASSERT_FALSE (mWriteCount == 0);
  for (Index = 0; Index < mWriteCount; Index++) {
    UT_ASSERT_TRUE ((mWrites[Index].Offset < 0x3800) || (mWrites[Index].Offset >= 0x3C00));
  }

  UfsFreeConfigData (&Ctl.Config);
  return UNIT_TEST_PASSED;
}

UNIT_TEST_STATUS
EFIAPI
MphyStartPcsReady (
  IN UNIT_TEST_CONTEXT  Context
  )
{
  UFS_CONTROLLER_DATA  Ctl;
  STATIC CONST UINT32  Start[] = { UFS_MEM_MPHY_START_OFFSET, UFS_MEM_MPHY_START };

  UT_ASSERT_NOT_EFI_ERROR (LoadController (&Ctl));

  // Ready on the third poll
  mPcsReadyAfterReads = 3;
  UT_ASSERT_NOT_EFI_ERROR (UfsMphyStart (&Ctl));
  UT_ASSERT_EQUAL (CheckWrites (Start, 1), UNIT_TEST_PASSED);
  UT_ASSERT_EQUAL (mPcsReads, 3);

  // Never ready: times out after about HcPcsReadyPollTimeout
  FakeRegsReset ();
  UT_ASSERT_STATUS_EQUAL (UfsMphyStart (&Ctl), EFI_TIMEOUT);
  UT_ASSERT_TRUE (mDelayUs >= Ctl.Config.TimeoutConfig.HcPcsReadyPollTimeout);
  UT_ASSERT_TRUE (mDelayUs <= Ctl.Config.TimeoutConfig.HcPcsReadyPollTimeout + 100);

  // A zero timeout is rejected
  FakeRegsReset ();
  Ctl.Config.TimeoutConfig.HcPcsReadyPollTimeout = 0;
  UT_ASSERT_STATUS_EQUAL (UfsMphyStart (&Ctl), EFI_INVALID_PARAMETER);
  UT_ASSERT_STATUS_EQUAL (UfsMphyStart (NULL), EFI_INVALID_PARAMETER);

  UfsFreeConfigData (&Ctl.Config);
  return UNIT_TEST_PASSED;
}

// ---------------------------------------------------------------------------

/**
  Register and run the tests of this build variant.

  @retval EFI_SUCCESS  The tests ran (results are reported by the framework).
  @retval others       The framework could not be set up.
**/
STATIC
EFI_STATUS
EFIAPI
UnitTestingEntry (
  VOID
  )
{
  EFI_STATUS                  Status;
  UNIT_TEST_FRAMEWORK_HANDLE  Framework;
  UNIT_TEST_SUITE_HANDLE      Config;
  UNIT_TEST_SUITE_HANDLE      Phy;

  Framework = NULL;
  DEBUG ((DEBUG_INFO, "%a v%a\n", UNIT_TEST_APP_NAME, UNIT_TEST_APP_VERSION));

  Status = InitUnitTestFramework (&Framework, UNIT_TEST_APP_NAME, gEfiCallerBaseName, UNIT_TEST_APP_VERSION);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = CreateUnitTestSuite (&Config, Framework, "Configuration resolution", "QcomUfsHcDxe.Config", NULL, NULL);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  if (FixedPcdGet8 (PcdUfsConfigSourceDeviceTree) != 0) {
    AddTestCase (Config, "Base config from the DT", "BaseFromDt", BaseConfigFromDt, TestSetup, NULL, NULL);
    AddTestCase (Config, "Invalid parameters", "InvalidParameters", BaseConfigInvalidParameters, TestSetup, NULL, NULL);
    AddTestCase (Config, "Base config PCD fallback", "BasePcdFallback", BaseConfigPcdFallback, TestSetup, NULL, NULL);
    AddTestCase (Config, "Malformed base config", "BaseMalformed", BaseConfigMalformed, TestSetup, NULL, NULL);
    AddTestCase (Config, "Config data from the DT", "ConfigFromDt", ConfigDataFromDt, TestSetup, NULL, NULL);
    AddTestCase (Config, "Empty de-emphasis list", "EmptyVendorList", ConfigDataEmptyVendorList, TestSetup, NULL, NULL);
    AddTestCase (Config, "Each node missing", "MissingNode", ConfigDataMissingNode, TestSetup, NULL, NULL);
    AddTestCase (Config, "Malformed nodes and tables", "Malformed", ConfigDataMalformed, TestSetup, NULL, NULL);
    AddTestCase (Config, "Out-of-range values", "OutOfRange", ConfigDataOutOfRange, TestSetup, NULL, NULL);
  } else {
    AddTestCase (Config, "PCD-only build", "PcdOnly", PcdOnlyConfig, TestSetup, NULL, NULL);
  }

  AddTestCase (Config, "PM config lookup", "PmConfig", PmConfigLookup, TestSetup, NULL, NULL);

  if (FixedPcdGet8 (PcdUfsConfigSourceDeviceTree) != 0) {
    Status = CreateUnitTestSuite (&Phy, Framework, "PHY programming", "QcomUfsHcDxe.Phy", NULL, NULL);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    AddTestCase (Phy, "Table order", "TableOrder", PhyTableOrder, TestSetup, NULL, NULL);
    AddTestCase (Phy, "Entry conditions", "EntryConditions", PhyEntryConditions, TestSetup, NULL, NULL);
    AddTestCase (Phy, "M-PHY start and PCS ready", "MphyStart", MphyStartPcsReady, TestSetup, NULL, NULL);
  }

  Status = RunAllTestSuites (Framework);

Exit:
  if (Framework != NULL) {
    FreeUnitTestFramework (Framework);
  }

  return Status;
}

/**
  Standard POSIX C entry point for host based unit test execution.
**/
int
main (
  int   argc,
  char  *argv[]
  )
{
  return UnitTestingEntry ();
}
