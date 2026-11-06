/** @file
  UFS Transport Protocol (UTP) layer configuration and power mode management.

  Implements UniPro parameter tuning, lane/gear/rate configuration, power mode
  transitions, host capability retrieval, and de-emphasis settings.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS    - Universal Flash Storage
  - UTP    - UFS Transport Protocol
  - UniPro - Unified Protocol (MIPI Alliance interconnect standard)
  - MIB    - Management Information Base
  - LCC    - Line Control Command

  @par Revision Reference:
  - JEDEC Universal Flash Storage (UFS) Version 3.1
  - JEDEC Universal Flash Storage (UFS) Version 4.0

**/

#include <Uefi.h>
#include <IndustryStandard/UfsHci.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UfsCore.h"
#include "UfsUtils.h"
#include "UfsUtpCore.h"

/**
  Populate a UIC command structure with opcode and arguments.

  @param[out]  Var  UIC command variable to populate.
  @param[in]   Op   UIC opcode.
  @param[in]   A1   Argument 1.
  @param[in]   A2   Argument 2.
  @param[in]   A3   Argument 3.
**/
#define PREPARE_UIC(Var, Op, A1, A2, A3)  do {                       \
                                                Var.Opcode  = Op;   \
                                                Var.Arg1    = A1;   \
                                                Var.Arg2    = A2;   \
                                                Var.Arg3    = A3;   \
                                            } while (0)

/**
  Construct UIC Argument 0 for a MIB attribute access.

  @param[in]  Attr  MIB attribute ID (packed into upper 16 bits).
  @param[in]  Sel   Selector index (packed into lower 16 bits).

  @return  UINT32 value encoding Attr and Sel for UIC Arg0.
**/
#define UIC_ARG0_MIB_ATTR(Attr, Sel)  ((((UINT32)(Attr)) << 16) | (((UINT32)(Sel)) & 0xFFFF))

/**
  Tunes UniPro parameters for optimal UFS operation.

  This function configures UniPro layer parameters including granularity,
  PA_TActivate timing, and PA_Hibern8Time to ensure proper communication
  between the host and UFS device. It reads local and peer parameters
  and adjusts them as needed for optimal performance.

  @param[in] HcDriverInterface    Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS             UniPro parameters tuned successfully.
  @retval EFI_INVALID_PARAMETER   Invalid input parameter.
  @retval EFI_NOT_STARTED         Required interface not available.
  @retval EFI_DEVICE_ERROR        Failed to configure UniPro parameters.

**/
EFI_STATUS
EFIAPI
UfsUtpTuneUniproParams (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS         Status;
  UINT32             Granularity;
  UINT32             PeerGranularity;
  UINT32             PaTactivate;
  UINT32             PeerPaTactivate;
  UINT32             PaTactivateUs;
  UINT32             PeerPaTactivateUs;
  UINT32             NewPeerPaTactivate;
  UINT32             TunedPaHibern8Time;
  EDKII_UIC_COMMAND  UicCommand;

  //
  // UniPro Spec, Table 28
  //
  CONST UINT8  GranToUsTable[] = { 1, 4, 8, 16, 32, 100 };

  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status             = EFI_SUCCESS;
  TunedPaHibern8Time = 0;
  ZeroMem (&UicCommand, sizeof (UicCommand));

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  //
  // PA_TACTIVATE = Time to wait before activating a burst in order to wake-up peer M-RX
  // host PA_TACTIVATE = device PA_TACTIVATE + 1
  //

  //
  // GET PA_Granularity (local)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_GRANULARITY, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Failed to read PA_Granularity (local) [Status=%r]\n",
      Status
      ));
    goto Exit;
  }

  Granularity = UicCommand.Arg3;

  //
  // PEER_GET PA_Granularity (device)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmePeerGet,
    UIC_ARG0_MIB_ATTR (PA_GRANULARITY, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Failed to read PA_Granularity (peer) [Status=%r]\n",
      Status
      ));
    goto Exit;
  }

  PeerGranularity = UicCommand.Arg3;

  if ((Granularity < PA_GRANULARITY_MIN_VAL) ||
      (Granularity > PA_GRANULARITY_MAX_VAL))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Invalid local PA_Granularity %u\n", Granularity));
    Status = EFI_INVALID_PARAMETER;
    goto Exit;
  }

  if ((PeerGranularity < PA_GRANULARITY_MIN_VAL) ||
      (PeerGranularity > PA_GRANULARITY_MAX_VAL))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Invalid peer PA_Granularity %u\n", PeerGranularity));
    Status = EFI_INVALID_PARAMETER;
    goto Exit;
  }

  //
  // GET PA_TActivate (local)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_T_ACTIVATE, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Failed to read PA_TActivate (local) [Status=%r]\n",
      Status
      ));
    goto Exit;
  }

  PaTactivate = UicCommand.Arg3;

  //
  // PEER_GET PA_TActivate (device)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmePeerGet,
    UIC_ARG0_MIB_ATTR (PA_T_ACTIVATE, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Failed to read PA_TActivate (peer) [Status=%r]\n",
      Status
      ));
    goto Exit;
  }

  PeerPaTactivate = UicCommand.Arg3;

  //
  // Convert to microseconds using granularity tables
  //
  PaTactivateUs     = PaTactivate      * GranToUsTable[Granularity      - 1];
  PeerPaTactivateUs = PeerPaTactivate  * GranToUsTable[PeerGranularity  - 1];

  //
  // If host activation time is larger, bump peer's PA_TActivate
  //
  if (PaTactivateUs > PeerPaTactivateUs) {
    NewPeerPaTactivate = PaTactivateUs / GranToUsTable[PeerGranularity - 1];
    NewPeerPaTactivate++;

    PREPARE_UIC (
      UicCommand,
      UfsUicDmePeerSet,
      UIC_ARG0_MIB_ATTR (PA_T_ACTIVATE, 0),
      0,
      NewPeerPaTactivate
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc] Failed to set PA_TActivate (peer) [Status=%r]\n",
        Status
        ));
      goto Exit;
    }
  }

  //
  // PA_Hibern8Time: the host forces UFS_PA_HIBERN8_TIME_TARGET_US,
  // expressed in units of the local granularity.
  //
  TunedPaHibern8Time = UFS_PA_HIBERN8_TIME_TARGET_US / GranToUsTable[Granularity - 1];

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_HIBERN8_TIME, 0),
    0,
    TunedPaHibern8Time
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsHc] Failed to set PA_Hibern8Time (local) [Status=%r]\n",
      Status
      ));
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Configures the active TX and RX data lane count for the UFS link.

  Reads the connected lane count from the UniPro PA layer, caps it to the
  configured maximum, sets PA_ActiveTxDataLanes and PA_ActiveRxDataLanes,
  and records the negotiated lane count in the active configuration.

  @param[in,out] ControllerData     Pointer to controller data; ActiveConfig.NumLanes
                                    is updated with the negotiated lane count.
  @param[in]     HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Lane configuration applied successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to read or write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigureLanes (
  IN OUT UFS_CONTROLLER_DATA            *ControllerData,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                  Status;
  UINT32                      RxLanes;
  UINT32                      TxLanes;
  EDKII_UIC_COMMAND           UicCommand;
  UFS_CONFIG_DATA             *Config;
  UFS_SPEED_CONFIG            *SpeedConfig;
  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status  = EFI_SUCCESS;
  RxLanes = 0;
  TxLanes = 0;
  ZeroMem (&UicCommand, sizeof (UicCommand));
  Config            = NULL;
  SpeedConfig       = NULL;
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  Config = &ControllerData->Config;

  SpeedConfig = &Config->SpeedConfig;

  if ((SpeedConfig->NumLanes == 0) || (SpeedConfig->NumLanes > UFS_MAX_DATA_LANES)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Invalid lane count %u (1..%u)\n", SpeedConfig->NumLanes, UFS_MAX_DATA_LANES));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get connected Tx data lanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_CONNECTED_TX_DATA_LANES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to read PA_ConnectedTxDataLanes [Status=%r]\n", Status));
    goto Exit;
  }

  TxLanes = UicCommand.Arg3;
  TxLanes = (SpeedConfig->NumLanes < TxLanes) ? SpeedConfig->NumLanes : TxLanes;

  //
  // PA_ActiveTxDataLanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_ACTIVE_TX_DATA_LANES, 0),
    0,
    TxLanes
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_ActiveTxDataLanes [Status=%r]\n", Status));
    goto Exit;
  }

  //
  // Get connected Rx data lanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_CONNECTED_RX_DATA_LANES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to read PA_ConnectedRxDataLanes [Status=%r]\n", Status));
    goto Exit;
  }

  RxLanes = UicCommand.Arg3;
  RxLanes = (SpeedConfig->NumLanes < RxLanes) ? SpeedConfig->NumLanes : RxLanes;

  //
  // PA_ActiveRxDataLanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_ACTIVE_RX_DATA_LANES, 0),
    0,
    RxLanes
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_ActiveRxDataLanes [Status=%r]\n", Status));
    goto Exit;
  }

  if ((RxLanes == 0) || (RxLanes != TxLanes)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Unusable lane configuration: RxLanes %u, TxLanes %u\n", RxLanes, TxLanes));
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }

  SpeedConfig->NumLanes = RxLanes;

Exit:
  return Status;
}

/**
  Configures the PA-layer TX and RX gear for the UFS link.

  Sets PA_TxGear and PA_RxGear to the gear value specified in the
  controller configuration.

  @param[in] ControllerData     Pointer to controller data containing the
                                target gear setting.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Gear configuration applied successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigureGear (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS         Status;
  EDKII_UIC_COMMAND  UicCommand;
  UFS_CONFIG_DATA    *Config;
  UFS_SPEED_CONFIG   *SpeedConfig;

  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status = EFI_SUCCESS;
  ZeroMem (&UicCommand, sizeof (UicCommand));
  Config            = NULL;
  SpeedConfig       = NULL;
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  Config      = &ControllerData->Config;
  SpeedConfig = &Config->SpeedConfig;

  //
  // In HS mode the gear is an HS gear (1..UfsHsGearMax); in PWM mode the
  // same attribute holds a PWM gear, which must not be 0 either.
  //
  if ((SpeedConfig->Gear == 0) ||
      (SpeedConfig->HsEnable && (SpeedConfig->Gear > UfsHsGearMax)))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Invalid gear %u (HsEnable=%u)\n", SpeedConfig->Gear, SpeedConfig->HsEnable));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Set RX gear (L1.5)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_RX_GEAR, 0),
    0,
    SpeedConfig->Gear
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_RxGear [Status=%r]\n", Status));
    goto Exit;
  }

  //
  // Set TX gear
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_TX_GEAR, 0),
    0,
    SpeedConfig->Gear
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_TxGear [Status=%r]\n", Status));
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Configures PA-layer TX and RX termination for the UFS link.

  Sets PA_TxTermination and PA_RxTermination based on whether High Speed
  mode is enabled in the controller configuration.

  @param[in] ControllerData     Pointer to controller data containing the
                                HS enable flag.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Termination configuration applied successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigureTermination (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS         Status;
  UINT32             PhyTerm;
  EDKII_UIC_COMMAND  UicCommand;
  UFS_CONFIG_DATA    *Config;
  UFS_SPEED_CONFIG   *SpeedConfig;

  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status  = EFI_SUCCESS;
  PhyTerm = UFS_PHY_HS_TERMINATION;
  ZeroMem (&UicCommand, sizeof (UicCommand));
  Config            = NULL;
  SpeedConfig       = NULL;
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  Config      = &ControllerData->Config;
  SpeedConfig = &Config->SpeedConfig;

  if (!SpeedConfig->HsEnable) {
    PhyTerm = UFS_PHY_NO_HS_TERMINATION;
  }

  //
  // TX termination
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_TX_TERMINATION, 0),
    0,
    PhyTerm
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_TxTermination [Status=%r]\n", Status));
    goto Exit;
  }

  //
  // RX termination
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_RX_TERMINATION, 0),
    0,
    PhyTerm
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_RxTermination [Status=%r]\n", Status));
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Configures the PA-layer HS series (rate A or B) for the UFS link.

  Sets PA_HSSeries to the rate value specified in the controller
  configuration.

  @param[in] ControllerData     Pointer to controller data containing the
                                target rate setting.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Rate configuration applied successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigureRate (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS         Status;
  EDKII_UIC_COMMAND  UicCommand;
  UFS_SPEED_CONFIG   *SpeedConfig;

  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status = EFI_SUCCESS;
  ZeroMem (&UicCommand, sizeof (UicCommand));
  SpeedConfig       = NULL;
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  SpeedConfig = &ControllerData->Config.SpeedConfig;

  if ((SpeedConfig->Rate != UfsRateA) && (SpeedConfig->Rate != UfsRateB)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Invalid HS rate series %u\n", SpeedConfig->Rate));
    return EFI_INVALID_PARAMETER;
  }

  //
  // HS series (rate)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_HS_SERIES, 0),
    0,
    SpeedConfig->Rate
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_HSSeries [Status=%r]\n", Status));
    goto Exit;
  }

Exit:
  return Status;
}

/**
  Configures HS adaptation settings for the UFS link.

  When adaptation is enabled and the gear is G4 or higher, reads the peer
  RX_HS_ADAPT_INITIAL_Capability and programs PA_PeerRxHsAdaptInitial and
  PA_TxHsAdaptType accordingly. For lower gears or when disabled, sets
  PA_TxHsAdaptType to NO_ADAPT.

  @param[in] ControllerData     Pointer to controller data containing the
                                adapt enable flag and gear setting.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Adaptation configuration applied successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to read or write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigureAdapt (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS         Status;
  EDKII_UIC_COMMAND  UicCommand;
  UFS_CONFIG_DATA    *Config;
  UFS_SPEED_CONFIG   *SpeedConfig;
  UINT32             HsAdaptCaps;

  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  Status = EFI_SUCCESS;
  ZeroMem (&UicCommand, sizeof (UicCommand));
  Config            = NULL;
  SpeedConfig       = NULL;
  HsAdaptCaps       = 0;
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  Config      = &ControllerData->Config;
  SpeedConfig = &Config->SpeedConfig;

  if (SpeedConfig->AdaptEnable &&
      (SpeedConfig->Gear >= UfsHsGear4))
  {
    PREPARE_UIC (
      UicCommand,
      UfsUicDmePeerGet,
      UIC_ARG0_MIB_ATTR (RX_HS_ADAPT_INITIAL_CAPABILITY, GEN_SELECT_RX (0)),
      0,
      0
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc] Failed to read RX_HS_ADAPT_INITIAL_Capability (Peer) [Status=%r]\n",
        Status
        ));
      goto Exit;
    }

    HsAdaptCaps = UicCommand.Arg3;

    PREPARE_UIC (
      UicCommand,
      UfsUicDmeSet,
      UIC_ARG0_MIB_ATTR (PA_PEER_RX_HS_ADAPT_INITIAL, 0),
      0,
      HsAdaptCaps
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_PeerRxHsAdaptInitial=0x%x [Status=%r]\n", HsAdaptCaps, Status));
      goto Exit;
    }

    PREPARE_UIC (
      UicCommand,
      UfsUicDmeSet,
      UIC_ARG0_MIB_ATTR (PA_TX_HS_ADAPT_TYPE, 0),
      0,
      UFS_PA_INITIAL
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_TxHsAdaptType=UFS_PA_INITIAL [Status=%r]\n", Status));
      goto Exit;
    }
  } else {
    //
    // Disable HS adaptation for lower gears or when AdaptEnable is false
    //
    PREPARE_UIC (
      UicCommand,
      UfsUicDmeSet,
      UIC_ARG0_MIB_ATTR (PA_TX_HS_ADAPT_TYPE, 0),
      0,
      UFS_PA_NO_ADAPT
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_TxHsAdaptType=NO_ADAPT [Status=%r]\n", Status));
      goto Exit;
    }
  }

Exit:
  return Status;
}

/**
  Wait for a UIC power mode change to complete.

  Samples IS once per iteration, so the error check and the completion check
  always see the same value, including on the first sample.

  @param[in]  UfsHc      The UFS host controller protocol.
  @param[in]  TimeoutUs  The time to wait, in microseconds.

  @retval EFI_SUCCESS       IS.UPMS was set with no error bits.
  @retval EFI_DEVICE_ERROR  An error bit was set in IS.
  @retval EFI_TIMEOUT       IS.UPMS was not set within TimeoutUs.
  @retval other             A register read failed.

**/
STATIC
EFI_STATUS
UfsUtpWaitPowerModeComplete (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc,
  IN UINT32                              TimeoutUs
  )
{
  EFI_STATUS  Status;
  UINT32      Is;

  while (TRUE) {
    Status = UfsHciRead32 (UfsHc, UFS_HC_IS_OFFSET, &Is);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Is & UFS_HC_IS_MASK_ALL_ERROR) != 0) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Error during power mode change [IS=0x%x]\n", Is));
      return EFI_DEVICE_ERROR;
    }

    if ((Is & UFS_HC_IS_UPMS) != 0) {
      return EFI_SUCCESS;
    }

    if (TimeoutUs < UFS_POLL_INTERVAL_US) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Timeout waiting for UPMS [IS=0x%x]\n", Is));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (UFS_POLL_INTERVAL_US);
    TimeoutUs -= UFS_POLL_INTERVAL_US;
  }
}

/**
  Check that the controller accepted the last UIC power mode change.

  @param[in]  UfsHc  The UFS host controller protocol.

  @retval EFI_SUCCESS       HCS.UPMCRS is PWR_OK, PWR_LOCAL or PWR_REMOTE.
  @retval EFI_DEVICE_ERROR  The change was rejected or failed.
  @retval other             A register read failed.

**/
STATIC
EFI_STATUS
UfsUtpCheckPowerModeResult (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc
  )
{
  EFI_STATUS  Status;
  UINT32      Hcs;
  UINT32      Result;

  Status = UfsHciRead32 (UfsHc, UFS_HC_STATUS_OFFSET, &Hcs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Result = (Hcs & UFS_HC_HCS_UPMCRS_MASK) >> UFS_HC_HCS_UPMCRS_SHIFT;
  if ((Result != UFS_HC_UPMCRS_PWR_OK) &&
      (Result != UFS_HC_UPMCRS_PWR_LOCAL) &&
      (Result != UFS_HC_UPMCRS_PWR_REMOTE))
  {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Power mode change rejected, HCS.UPMCRS=%u\n", Result));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Applies the configured power mode to the UFS link.

  Programs the PA_PWRModeUserData timeout values, clears IS.UPMS, triggers
  the power mode change through PA_PWRMode, waits for IS.UPMS and checks the
  result in HCS.

  @param[in] ControllerData     Pointer to controller data containing the HS
                                enable flag and timeout configuration.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           The power mode change completed.
  @retval EFI_INVALID_PARAMETER An input parameter is NULL, or UmpsPollTimeout is 0.
  @retval EFI_NOT_STARTED       The UIC command interface or the host controller
                                protocol is not available.
  @retval EFI_DEVICE_ERROR      A UIC command failed, an error was reported in IS,
                                or the change was rejected.
  @retval EFI_TIMEOUT           IS.UPMS was not set in time.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigurePowerMode (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                          Status;
  EDKII_UIC_COMMAND                   UicCommand;
  UINT32                              PwrMode;
  UINTN                               Index;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EDKII_UFS_EXEC_UIC_COMMAND          UfsExecUicCommand;

  STATIC CONST UINT32  PwrModeUserData[] = {
    PA_PWR_MODE_USER_DATA_0_VAL,
    PA_PWR_MODE_USER_DATA_1_VAL,
    PA_PWR_MODE_USER_DATA_2_VAL,
    PA_PWR_MODE_USER_DATA_3_VAL,
    PA_PWR_MODE_USER_DATA_4_VAL,
    PA_PWR_MODE_USER_DATA_5_VAL
  };

  ZeroMem (&UicCommand, sizeof (UicCommand));

  if ((ControllerData == NULL) || (HcDriverInterface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  UfsHc             = HcDriverInterface->UfsHcProtocol;
  if ((UfsExecUicCommand == NULL) || (UfsHc == NULL)) {
    return EFI_NOT_STARTED;
  }

  //
  // Validate before anything is sent: once PA_PWRMode is written the link
  // renegotiates, and it must then be waited for.
  //
  if (ControllerData->Config.TimeoutConfig.UmpsPollTimeout == 0) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] UmpsPollTimeout is 0\n"));
    return EFI_INVALID_PARAMETER;
  }

  PwrMode = ControllerData->Config.SpeedConfig.HsEnable ? UFS_DEFAULT_HS_PWR_MODE : UFS_DEFAULT_NO_HS_PWR_MODE;

  for (Index = 0; Index < ARRAY_SIZE (PwrModeUserData); Index++) {
    PREPARE_UIC (
      UicCommand,
      UfsUicDmeSet,
      UIC_ARG0_MIB_ATTR (PA_PWR_MODE_USER_DATA (Index), 0),
      0,
      PwrModeUserData[Index]
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_PWRModeUserData(%u) [Status=%r]\n", (UINT32)Index, Status));
      return Status;
    }
  }

  //
  // Clear a stale IS.UPMS so the wait below sees this change complete. IS
  // is write-1-to-clear, so write only that bit and leave any other pending
  // status for its owner.
  //
  Status = UfsHciWrite32 (UfsHc, UFS_HC_IS_OFFSET, UFS_HC_IS_UPMS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Set the HOST power state (triggers the UIC power mode change)
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_PWR_MODE, 0),
    0,
    PwrMode
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsHc] Failed to set PA_PWRMode [Status=%r]\n", Status));
    return Status;
  }

  Status = UfsUtpWaitPowerModeComplete (UfsHc, ControllerData->Config.TimeoutConfig.UmpsPollTimeout);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsHciWrite32 (UfsHc, UFS_HC_IS_OFFSET, UFS_HC_IS_UPMS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return UfsUtpCheckPowerModeResult (UfsHc);
}

/**
  Performs a UTP layer reconfiguration after link startup.

  The lane, gear, termination, rate and adaptation attributes only take
  effect through a PA_PWRMode change, so they are programmed only when that
  change will follow: when UEFI brought the link up in this boot (PreHce ran,
  the link is in its slow start-up mode), or when
  PcdUfs0PowerModeReconfigEnable forces it. When the previous boot stage
  brought the link up, re-issuing the change would renegotiate a link that
  is not expecting it, so the link is left in the mode it came up in.

  @param[in,out] ControllerData     Pointer to controller data.
  @param[in]     HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Reconfiguration completed, or was not needed.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.
  @retval EFI_NOT_STARTED       Required interfaces are not available.
  @retval other                 A configuration step failed.

**/
EFI_STATUS
EFIAPI
UfsUtpReconfigure (
  IN OUT UFS_CONTROLLER_DATA            *ControllerData,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS  Status;

  if ((ControllerData == NULL) || (HcDriverInterface == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!ControllerData->UefiBringUp && !FixedPcdGetBool (PcdUfs0PowerModeReconfigEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "[UfsHc] Controller[%u]: link brought up by the previous boot stage, power mode left unchanged\n",
      ControllerData->ControllerId
      ));
    return EFI_SUCCESS;
  }

  Status = UfsUtpConfigureLanes (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsUtpConfigureGear (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsUtpConfigureTermination (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsUtpConfigureRate (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsUtpConfigureAdapt (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsUtpConfigurePowerMode (ControllerData, HcDriverInterface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "[UfsHc] Controller[%u]: power mode applied (gear %u, rate %u, %u lanes)\n",
    ControllerData->ControllerId,
    ControllerData->Config.SpeedConfig.Gear,
    ControllerData->Config.SpeedConfig.Rate,
    ControllerData->Config.SpeedConfig.NumLanes
    ));

  return EFI_SUCCESS;
}

/**
  Retrieves UFS host capabilities for the specified controller.

  This function reads host capabilities including maximum gear, lane count,
  HS rate, and UniPro version information from the UFS controller.

  @param[in,out] ControllerData     Pointer to the UFS controller data.
  @param[in]     HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Host capabilities retrieved successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are invalid.
  @retval EFI_NOT_STARTED       Required interface not available.

**/
EFI_STATUS
EFIAPI
UfsUtpGetHostCaps (
  IN OUT UFS_CONTROLLER_DATA            *ControllerData,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS                  Status;
  UFS_CAPABILITIES            Caps;
  EDKII_UIC_COMMAND           UicCommand;
  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  ZeroMem (&UicCommand, sizeof (UicCommand));
  ZeroMem (&Caps, sizeof (Caps));
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  if (ControllerData->HostCaps.Valid) {
    return EFI_SUCCESS;
  }

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_MAX_RX_PWM_GEAR, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.MaxPwmGear = UicCommand.Arg3;

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_MAX_RX_HS_GEAR, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.MaxHsGear = UicCommand.Arg3;

  //
  // Get HS Rate capability
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_HS_SERIES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.LastHsRate = UicCommand.Arg3;

  //
  // Get Available TX Data Lanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_AVAIL_TX_DATA_LANES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.TxLanes = UicCommand.Arg3;

  //
  // Get Available RX Data Lanes
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_AVAIL_RX_DATA_LANES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.RxLanes = UicCommand.Arg3;

  //
  // Get Local Version Info
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_LOCAL_VER_INFO, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.UniproVersionLocal = UicCommand.Arg3;

  //
  // Get Remote Version Info
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_REMOTE_VER_INFO, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Caps.UniproVersionRemote = UicCommand.Arg3;

  //
  // Publish the capabilities only once every read has succeeded, so a
  // reader never sees a half-written structure.
  //
  Caps.Valid = TRUE;
  CopyMem (&ControllerData->HostCaps, &Caps, sizeof (Caps));

  return EFI_SUCCESS;
}

/**
  Configures UFS controller clock control registers and UniPro clock attributes.

  Programs SYS1CLK_1US, DME_VS_CORE_CLK_CTRL, and PA_VS_CORE_CLK_40NS_CYCLES
  with timing values from the active power management configuration.

  @param[in] ControllerData     Pointer to controller data containing the PM
                                timing configuration and HCI base address.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Clock control configured successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       Required interfaces are not available.
  @retval EFI_DEVICE_ERROR      Failed to read or write a register or attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpSetClockControl (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS            Status;
  UINT32                RegValue;
  UFS_PM_TIMING_CONFIG  *TimingConfig;
  UFS_CONFIG_DATA       *Config;

  EDKII_UIC_COMMAND                   UicCommand;
  EDKII_UFS_EXEC_UIC_COMMAND          UfsExecUicCommand;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;

  ZeroMem (&UicCommand, sizeof (UicCommand));
  UfsExecUicCommand = NULL;
  UfsHc             = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Config = &ControllerData->Config;

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  UfsHc = HcDriverInterface->UfsHcProtocol;
  if (UfsHc == NULL) {
    return EFI_NOT_STARTED;
  }

  if (Config->PmConfig == NULL) {
    return EFI_NOT_STARTED;
  }

  TimingConfig = &Config->PmConfig->TimingConfig;

  Status = UfsHciWrite32 (UfsHc, UFS_HC_SYS1CLK_1US, TimingConfig->Sysclk1uS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (DME_VS_CORE_CLK_CTRL, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RegValue = UicCommand.Arg3;
  RegValue = (RegValue & ~DME_VS_CORE_CLK_CTRL_MAX_1US_MASK) |
             ((TimingConfig->DmeMax1uS << DME_VS_CORE_CLK_CTRL_MAX_1US_SHIFT) & DME_VS_CORE_CLK_CTRL_MAX_1US_MASK);
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (DME_VS_CORE_CLK_CTRL, 0),
    0,
    RegValue
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeGet,
    UIC_ARG0_MIB_ATTR (PA_VS_CORE_CLK_40NS_CYCLES, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RegValue = UicCommand.Arg3;
  RegValue = (RegValue & ~PA_VS_CORE_CLK_40NS_CYCLES_MASK) | (TimingConfig->PaVs40nS & PA_VS_CORE_CLK_40NS_CYCLES_MASK);

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_VS_CORE_CLK_40NS_CYCLES, 0),
    0,
    RegValue
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Status;
}

/**
  Polls the TX FSM state until the UFS link enters hibernate.

  Reads TX_FSM_State via DME_GET and waits until it equals
  TX_FSM_State_Hibernate, using the hibernate poll timeout from the
  controller configuration.

  @param[in] ControllerData     Pointer to controller data containing the
                                hibernate poll timeout.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Link entered hibernate successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_TIMEOUT           Timed out waiting for hibernate state.

**/
EFI_STATUS
EFIAPI
UfsUtpCheckLinkHibernate (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS          Status;
  UINTN               Timeout;
  UINT32              RegValue;
  UFS_CONFIG_DATA     *Config;
  UFS_TIMEOUT_CONFIG  *TimeoutConfig;

  EDKII_UIC_COMMAND           UicCommand;
  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  ZeroMem (&UicCommand, sizeof (UicCommand));
  UfsExecUicCommand = NULL;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  Config        = &ControllerData->Config;
  TimeoutConfig = &Config->TimeoutConfig;

  //
  // Poll for link in hibernate, sampling every UFS_POLL_INTERVAL_US for up
  // to HibernatePollTimeout microseconds.
  //
  Timeout = TimeoutConfig->HibernatePollTimeout;
  if (Timeout == 0) {
    return EFI_INVALID_PARAMETER;
  }

  do {
    PREPARE_UIC (
      UicCommand,
      UfsUicDmeGet,
      UIC_ARG0_MIB_ATTR (TX_FSM_STATE, 0),
      0,
      0
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    RegValue = UicCommand.Arg3;

    if (RegValue == TX_FSM_STATE_HIBERNATE) {
      break;
    }

    if (Timeout < UFS_POLL_INTERVAL_US) {
      DEBUG ((DEBUG_ERROR, "[UfsHc] TX_FSM_State HIBERN8 timeout\n"));
      Status = EFI_TIMEOUT;
      break;
    }

    MicroSecondDelay (UFS_POLL_INTERVAL_US);
    Timeout -= UFS_POLL_INTERVAL_US;
  } while (TRUE);

  return Status;
}

/**
  Disables the Local Link Control (LLC) on both host and device sides.

  Sets PA_Local_TX_LCC_Enable and PA_Peer_TX_LCC_Enable to 0 via DME_SET
  commands to disable LCC on both ends of the UFS link.

  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           LLC disabled successfully on both sides.
  @retval EFI_INVALID_PARAMETER HcDriverInterface is NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to write a UniPro attribute.

**/
EFI_STATUS
EFIAPI
UfsUtpDisableLlc (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  )
{
  EFI_STATUS  Status;

  EDKII_UIC_COMMAND           UicCommand;
  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;

  ZeroMem (&UicCommand, sizeof (UicCommand));
  UfsExecUicCommand = NULL;

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  //
  // Disable host and device LCC
  //
  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_LOCAL_TX_LCC_ENABLE, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PREPARE_UIC (
    UicCommand,
    UfsUicDmeSet,
    UIC_ARG0_MIB_ATTR (PA_PEER_TX_LCC_ENABLE, 0),
    0,
    0
    );

  Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Status;
}

/**
  Applies TX de-emphasis settings to all active UFS lanes.

  Writes the specified de-emphasis value to TX_HS_Equalizer_Setting for
  each active lane via DME_SET commands.

  @param[in] ControllerData     Pointer to controller data containing the
                                active lane count.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.
  @param[in] DeEmphasisValue    De-emphasis value to program into each lane.

  @retval EFI_SUCCESS           De-emphasis applied to all lanes successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       UIC command interface is not available.
  @retval EFI_DEVICE_ERROR      Failed to write a UniPro attribute on a lane.

**/
EFI_STATUS
EFIAPI
UfsUtpApplyDeEmphasisSettings (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface,
  IN UINT32                         DeEmphasisValue
  )
{
  EFI_STATUS                  Status;
  UINTN                       Loop;
  EDKII_UIC_COMMAND           UicCommand;
  EDKII_UFS_EXEC_UIC_COMMAND  UfsExecUicCommand;
  UFS_SPEED_CONFIG            *SpeedConfig;

  Status = EFI_SUCCESS;

  if (ControllerData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (HcDriverInterface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  UfsExecUicCommand = HcDriverInterface->UfsExecUicCommand;
  if (UfsExecUicCommand == NULL) {
    return EFI_NOT_STARTED;
  }

  SpeedConfig = &ControllerData->Config.SpeedConfig;

  for (Loop = 0; Loop < MIN (SpeedConfig->NumLanes, UFS_MAX_DATA_LANES); Loop++) {
    PREPARE_UIC (
      UicCommand,
      UfsUicDmeSet,
      UIC_ARG0_MIB_ATTR (TX_HS_EQUALIZER_SETTING, GEN_SELECT_TX (Loop)),
      0,
      DeEmphasisValue
      );

    Status = UfsExecUicCommand (HcDriverInterface, &UicCommand);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "[UfsHc] Failed to apply De-Emphasis [Status=%r]\n",
        Status
        ));

      break;
    }
  }

  return Status;
}
