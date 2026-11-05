/** @file
  Internal declarations for UFS core initialization and callback functions.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS - Universal Flash Storage
  - HCI - Host Controller Interface
  - PHY - Physical Layer

**/

#pragma once

#include <Uefi.h>
#include <IndustryStandard/Ufs.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

///
/// UFS controller identifiers. This driver supports a single on-die UFS
/// controller (UfsController0); UfsNumControllers is also used as the
/// bounds check for ControllerId everywhere in this module.
///
typedef enum {
  UfsController0,
  UfsNumControllers,
} UFS_CONTROLLER_ID;

#define UFS_HC_USER_LUN_MAX  UFS_LUN_7

#pragma pack (1)

///
/// UFS base configuration, Includes base addresses and enabled callback phases
///
typedef struct {
  EFI_PHYSICAL_ADDRESS    UfsMemBase;               ///< UFS Memory Base Address
  EFI_PHYSICAL_ADDRESS    UfsHciBase;               ///< UFS Hci Base Address
  UINT8                   PreHceEnable;             ///< Enable EdkiiUfsHcPreHce phase in platform callback
  UINT8                   PostLinkUpEnable;         ///< Enable EdkiiUfsHcPostLinkStartup phase in platform callback
  UINT8                   RefClkFreq;               ///< EDKII_UFS_CARD_REF_CLK_FREQ_ATTRIBUTE value for EDKII_UFS_HC_PLATFORM_PROTOCOL.RefClkFreq
  UINT8                   SkipHceReenable;          ///< EDKII_UFS_HC_PLATFORM_PROTOCOL.SkipHceReenable
  UINT8                   SkipLinkStartup;          ///< EDKII_UFS_HC_PLATFORM_PROTOCOL.SkipLinkStartup
} UFS_BASE_CONFIG;

#pragma pack ()

///
/// Maximum length for UFS vendor name string
///
#define UFS_CONFIG_MAX_VENDOR_NAME_LEN  8

///
/// Maximum length for UFS product name string
///
#define UFS_CONFIG_MAX_PRODUCT_NAME_LEN  16

///
/// Maximum number of bytes in a SCSI INQUIRY response buffer.
///
#define UFS_INQUIRY_RESPONSE_LEN  96

///
/// UFS Gear enumeration defines the speed modes supported by UFS devices
///
typedef UINT8 UFS_GEAR;
enum UFS_GEAR_VALUES {
  UfsGearPWM        = 0,                  ///< PWM (Pulse Width Modulation) gear mode
  UfsHsGearInvalid  = UfsGearPWM,         ///< Invalid/uninitialized High Speed gear
  UfsHsGear1        = 1,                  ///< High Speed gear 1
  UfsHsGear2        = 2,                  ///< High Speed gear 2
  UfsHsGear3        = 3,                  ///< High Speed gear 3
  UfsHsGear4        = 4,                  ///< High Speed gear 4
  UfsHsGear5        = 5,                  ///< High Speed gear 5
  UfsHsGearMax      = UfsHsGear5,         ///< Maximum supported High Speed gear
};

///
/// UFS gear mode enumeration
///
typedef enum {
  UfsGearModeInvalid,
  UfsGearModeDefault,                     ///< Represents HS-G3 and below
  UfsGearMode4,
  UfsGearMode5,
  UfsGearModeMax,
} UFS_GEAR_MODE;

///
/// UFS Rate enumeration defines the rate series for High Speed mode
///
typedef UINT8 UFS_RATE;
enum UFS_RATE_VALUES {
  UfsRateUndefined  = 0,                  ///< Rate is not defined
  UfsRateA          = 1,                  ///< Rate A series
  UfsRateB          = 2,                  ///< Rate B series
  UfsRateMax        = 3,                  ///< Maximum rate value
};

///
/// UFS voltage corner enumeration
///
typedef UINT8 UFS_VOLTAGE_CORNER;
enum UFS_VOLTAGE_CORNER_VALUES {
  UfsVoltageCornerUndefined  = 0,
  UfsVoltageCornerMinSVS     = 1,
  UfsVoltageCornerLowSVS     = 2,
  UfsVoltageCornerSVS        = 3,
  UfsVoltageCornerNominal    = 4,
  UfsVoltageCornerTurbo      = 5,
  UfsVoltageCornerMax        = 6,
};

///
/// UFS Capabilities structure contains the host controller capabilities
/// and device information obtained during initialization
///
typedef struct {
  BOOLEAN    Valid;                     ///< Indicates if the capabilities data is valid
  UINT8      MaxPwmGear;                ///< Maximum PWM gear supported by the device
  UINT8      MaxHsGear;                 ///< Maximum High Speed gear supported by the device
  UINT8      LastHsRate;                ///< Last configured High Speed rate (A or B)
  UINT8      TxLanes;                   ///< Number of transmit lanes available
  UINT8      RxLanes;                   ///< Number of receive lanes available
  UINT16     UniproVersionLocal;        ///< Local UniPro version
  UINT16     UniproVersionRemote;       ///< Remote UniPro version
} UFS_CAPABILITIES;

///
/// UFS Product List structure contains vendor-specific product information
///
typedef struct {
  CHAR8    *VendorName;             ///< Pointer to vendor name string
  CHAR8    *Products;               ///< Pointer to array of product name strings
  UINTN    ProductListSize;         ///< Number of products for this vendor
} UFS_PRODUCT_LIST;

///
/// UFS Vendor List structure
///
typedef struct {
  UFS_PRODUCT_LIST    *List;            ///< Pointer to array of product lists
  UINTN               NumVendors;       ///< Number of vendors in the list
} UFS_VENDOR_LIST;

///
/// UFS Timing register values
///
typedef struct {
  UINT32    DmeMax1uS;
  UINT32    PaVs40nS;
  UINT32    Sysclk1uS;
} UFS_PM_TIMING_CONFIG;

///
/// UFS Power management config
///
typedef struct {
  UFS_VOLTAGE_CORNER      VoltageCorner;
  UFS_GEAR                MaxHsGear;
  UFS_PM_TIMING_CONFIG    TimingConfig;
} UFS_PM_CONFIG;

///
/// UFS Phy Initialization type enumertion
///
typedef enum {
  UfsPhyTableInit,
  UfsPhyTableInitLane0,
  UfsPhyTableInitLane1,
  UfsPhyTableInitEnd,
  UfsPhyTableHsLss,
  UfsPhyTableTypeMax,
} UFS_PHY_TABLE_TYPE;

///
/// UFS Link startup modes
///
typedef UINT8 UFS_LINK_STARTUP_MODES;
enum UFS_LINK_STARTUP_MODE_VALUES {
  UfsLinkStartupModeLsLss  = 0,
  UfsLinkStartupModeHsLss  = 1,
  UfsLinkStartupModeMax    = 2,
};

#pragma pack (1)

///
/// UFS Phy table element definition
///
typedef struct {
  UINT16    Flags;
  UINT16    Offset;
  UINT16    Value;
} UFS_PHY_TABLE;

///
/// UFS Phy table
///
typedef struct {
  UINT32           NumEntries;
  UFS_PHY_TABLE    *Entries;
} UFS_PHY_TABLE_LIST;

typedef struct {
  UFS_VOLTAGE_CORNER    VoltageCorner;
} UFS_RESOURCE_CONFIG;

typedef struct {
  UINT32    HcEnablePollTimeout;
  UINT32    HcPcsReadyPollTimeout;
  UINT32    HibernatePollTimeout;
  UINT32    UmpsPollTimeout;
} UFS_TIMEOUT_CONFIG;

typedef struct {
  EFI_PHYSICAL_ADDRESS    ResetRegister;
  EFI_PHYSICAL_ADDRESS    SharedPhyRegister;
  EFI_PHYSICAL_ADDRESS    SpareRegister;
} UFS_REGISTER_CONFIG;

typedef struct {
  UFS_PHY_TABLE_LIST    PhyTable[UfsPhyTableTypeMax];
} UFS_PHY_CONFIG;

typedef struct {
  UFS_RATE                  Rate;                       ///< Rate series (A or B) for High Speed mode
  UFS_GEAR                  Gear;                       ///< Gear mode for data transfer speed
  BOOLEAN                   HsEnable;                   ///< Enable High Speed mode
  UINT8                     NumLanes;                   ///< Number of lanes to use for data transfer
  UINT8                     AdaptEnable;                ///< Enable adaptive configuration
  UFS_LINK_STARTUP_MODES    LinkStartupMode;            ///< Link Startup Mode
  BOOLEAN                   WriteBoosterEnable;         ///< Enable write booster in initialization
} UFS_SPEED_CONFIG;

#pragma pack ()

///
/// UFS Configuration Data structure contains all configuration parameters
/// for UFS host controller initialization and operation
///
typedef struct {
  UFS_SPEED_CONFIG       SpeedConfig;                  ///< UFS Speed Config
  UFS_RESOURCE_CONFIG    ResourceConfig;               ///< UFS Resources configuration
  UFS_TIMEOUT_CONFIG     TimeoutConfig;                ///< UFS Timeout configuration
  UFS_REGISTER_CONFIG    RegisterConfig;               ///< UFS Register addresses
  UFS_PHY_CONFIG         PhyConfig;                    ///< UFS Phy configuration tables
  UFS_VENDOR_LIST        DeEmphasisVendorList;         ///< List of vendors requiring de-emphasis configuration
  UFS_PM_CONFIG          *PmConfig;                    ///< Power management configuration
} UFS_CONFIG_DATA;

///
/// Active UFS Configuration Data - typedef for currently active configuration
///
typedef UFS_CONFIG_DATA UFS_ACTIVE_CONFIG_DATA;

///
/// UFS SCSI Pass-Through Event Context structure for event handling
///
typedef struct {
  EFI_HANDLE                       ControllerHandle;    ///< Handle to the UFS controller
  EDKII_UFS_HC_DRIVER_INTERFACE    *HcDriverInterface;  ///< Pointer to host controller driver interface
  EFI_EVENT                        ScsiPassthruEvent;   ///< Named event for SCSI pass-through protocol
  VOID                             *ScsiPassthruEventReg;  ///< Registration cookie for the SCSI pass-through named event, used to deregister on teardown
  EFI_EVENT                        DeviceConfigEvent;   ///< Named event for UFS device config protocol
  VOID                             *DeviceConfigEventReg;  ///< Registration cookie for the UFS device config named event, used to deregister on teardown
} UFS_CALLBACK_EVENT_CONTEXT;

///
/// UFS Controller Data structure contains all information related to a UFS controller
/// including configuration, capabilities, and runtime state
///
typedef struct {
  UINT32                                  ControllerId;                 ///< Unique identifier for the controller
  UINT32                                  Version;                      ///< Controller version information
  EFI_PHYSICAL_ADDRESS                    UfsMemBase;                   ///< Base address of UFS memory region
  EFI_PHYSICAL_ADDRESS                    UfsHciBase;                   ///< Base address of UFS HCI registers
  UFS_CAPABILITIES                        HostCaps;                     ///< Host controller capabilities
  UFS_CONFIG_DATA                         Config;                       ///< Static configuration data
  UFS_CALLBACK_EVENT_CONTEXT              *UfsCallbackEventContext;     ///< SCSI pass-through event context
  UINT32                                  SpareRegisterValue;           ///< Record of shared info passed to HLOS
  BOOLEAN                                 SharedInfoUpdated;            ///< Flag tracking if shared info was updated
  BOOLEAN                                 WriteBoosterEnabled;          ///< Flag tracking if write booster was enabled
  EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE    CompletedPhase;               ///< Last completed initialization phase
} UFS_CONTROLLER_DATA;

///
/// UFS Device Data structure
///
typedef struct {
  UFS_DEV_DESC          DeviceDesc;                                     ///< Device descriptor
  UFS_RPMB_UNIT_DESC    RpmbUnitDesc;                                   ///< RPMB unit descriptor
  UFS_GEOMETRY_DESC     GeometryDesc;                                   ///< Geometry descriptor
  CHAR16                *SerialNumber;                                  ///< Device serial number string
  UINT32                SerialNumberBytes;                              ///< Serial Number Size in Bytes
} UFS_DEVICE_DATA;

//
// UFS Core Entry Points (defined in UfsCore.c; called from UfsHcPlatform.c's
// EDKII_UFS_HC_PLATFORM_PROTOCOL callback)
//

/**
  Performs pre-HCE initialization for the UFS controller.

  This function performs UFS resource setup and PHY initialization, retrieves
  base addresses, configures the MPHY, enables clocks, and brings the host
  controller out of reset in preparation for link startup. It also initializes
  and/or updates the controller context.

  @param[in]      ControllerId       The UFS controller identifier.
  @param[in]      UfsMemBase         Ufs memory base address
  @param[in]      UfsHciBase         Ufs host controller base address
  @param[in,out]  ControllerContext  Pointer to a location that receives/contains
                                     the controller runtime context. On input, may
                                     point to an existing context or NULL. On output,
                                     set to the allocated/updated controller context.
                                     On error: if *ControllerContext was NULL on entry
                                     (new allocation), it is reset to NULL and the
                                     allocation is freed. If *ControllerContext was
                                     non-NULL on entry (existing context), it is
                                     unconditionally reset to NULL on error so callers
                                     can always treat a non-NULL value as success.
  @param[in]      HcDriverInterface  Pointer to the host controller driver interface
                                     used to communicate with the UFS controller.

  @retval EFI_SUCCESS             Initialization completed successfully.
  @retval EFI_INVALID_PARAMETER   One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES    Memory allocation failed.
  @retval EFI_DEVICE_ERROR        Hardware access failed during initialization.
  @retval EFI_TIMEOUT             Timed out waiting for controller state changes.

**/
EFI_STATUS
EFIAPI
UfsPreHce (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsMemBase,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  );

/**
  Performs post-link-up initialization for the UFS controller.

  This function is invoked after the UFS link has been successfully established.
  It performs post-link configuration such as tuning UniPro parameters and
  reconfiguration of the UTP layer, registers named event listeners for relevant
  protocols, and updates the controller context with event state required by
  runtime operations.

  @param[in]      ControllerId       The UFS controller identifier.
  @param[in]      UfsHciBase         Ufs host controller base address
  @param[in]      ControllerHandle   EFI handle of the UFS controller instance.
  @param[in,out]  ControllerContext  Pointer to the controller runtime context.
                                     On input, may point to an existing context or NULL.
                                     On output, set to the allocated/updated context.
                                     On error: *ControllerContext is unconditionally
                                     reset to NULL regardless of whether the context
                                     was newly allocated or passed in by the caller.
  @param[in]      HcDriverInterface  Pointer to the host controller driver interface.

  @retval EFI_SUCCESS             Post-link initialization completed successfully.
  @retval EFI_INVALID_PARAMETER   One or more input parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES    Memory/resource allocation failed.
  @retval EFI_DEVICE_ERROR        Hardware/driver error during post-link steps.

**/
EFI_STATUS
EFIAPI
UfsPostLinkUp (
  IN     UFS_CONTROLLER_ID              ControllerId,
  IN     EFI_PHYSICAL_ADDRESS           UfsHciBase,
  IN     EFI_HANDLE                     ControllerHandle,
  IN OUT VOID                           **ControllerContext,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  );

