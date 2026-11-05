/** @file
  Declarations for UFS Transport Protocol (UTP) layer configuration functions.

  Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Glossary:
  - UFS    - Universal Flash Storage
  - UTP    - UFS Transport Protocol
  - UniPro - Unified Protocol (MIPI Alliance interconnect standard)
  - MIB    - Management Information Base
  - LCC    - Line Control Command

**/

#pragma once

#include <Uefi.h>
#include "UfsCore.h"

//
// UFS UTP Attributes
//
#define PA_ACTIVE_TX_DATA_LANES            0x1560
#define PA_ACTIVE_RX_DATA_LANES            0x1580
#define PA_TX_TRAILING_CLOCKS              0x1564
#define PA_PHY_TYPE                        0x1500
#define PA_MIN_RX_TRAILING_CLOCKS          0x1543
#define PA_TX_GEAR                         0x1568
#define PA_TX_TERMINATION                  0x1569
#define PA_PWR_MODE                        0x1571
#define PA_RX_GEAR                         0x1583
#define PA_RX_TERMINATION                  0x1584
#define PA_RX_HS_UNTERMINATION_CAPABILITY  0x15A5
#define PA_RX_LS_UNTERMINATION_CAPABILITY  0x15A6
#define PA_HIBERN8_TIME                    0x15A7
#define PA_T_ACTIVATE                      0x15A8
#define PA_PACP_FRAME_COUNT                0x15C0
#define PA_PACP_ERROR_COUNT                0x15C1
#define PA_PHY_TEST_CONTROL                0x15C2
#define PA_LOGICAL_LANE_MAP                0x15A1
#define PA_GRANULARITY                     0x15AA
#define PA_PEER_RX_HS_ADAPT_INITIAL        0x15D3
#define PA_TX_HS_ADAPT_TYPE                0x15D4
#define PA_MAX_RX_PWM_GEAR                 0x1586
#define PA_MAX_RX_HS_GEAR                  0x1587
#define PA_HS_SERIES                       0x156A
#define PA_AVAIL_TX_DATA_LANES             0x1520
#define PA_AVAIL_RX_DATA_LANES             0x1540
#define PA_LOCAL_VER_INFO                  0x15A9
#define PA_REMOTE_VER_INFO                 0x15A0
#define PA_CONNECTED_TX_DATA_LANES         0x1561
#define PA_CONNECTED_RX_DATA_LANES         0x1581
#define PA_LOCAL_TX_LCC_ENABLE             0x155E
#define PA_PEER_TX_LCC_ENABLE              0x155F
#define DME_VS_CORE_CLK_CTRL               0xD002
#define PA_VS_CORE_CLK_40NS_CYCLES         0x9007
#define PA_PWR_MODE_USER_DATA_0_VAL        0x1FFF
#define PA_PWR_MODE_USER_DATA_1_VAL        0xFFFF
#define PA_PWR_MODE_USER_DATA_2_VAL        0x7FFF
#define PA_PWR_MODE_USER_DATA_3_VAL        0x1FFF
#define PA_PWR_MODE_USER_DATA_4_VAL        0xFFFF
#define PA_PWR_MODE_USER_DATA_5_VAL        0x7FFF

//
// Granularity Values
//
#define PA_GRANULARITY_MIN_VAL  0x1
#define PA_GRANULARITY_MAX_VAL  0x6

//
// Phy Adaption Modes
//
#define UFS_PA_REFRESH   0x0
#define UFS_PA_INITIAL   0x1
#define UFS_PA_NO_ADAPT  0x3

//
// UFS Lane Settings
//
#define TX_HS_EQUALIZER_SETTING         0x37
#define TX_FSM_STATE_HIBERNATE          0x1
#define TX_HS_GEAR_CAPABILITY           0x02
#define TX_FSM_STATE                    0x41
#define RX_HS_ADAPT_INITIAL_CAPABILITY  0x9F

//
// De-Emphasis Values
//
#define DE_EMPHASIS_DISABLE_VAL  0x0
#define DE_EMPHASIS_GEAR5_VAL    0x4

//
// UFS Default Settings
//
#define UFS_DEFAULT_HS_GEAR         0x04
#define UFS_DEFAULT_HS_PWR_MODE     0x11
#define UFS_DEFAULT_NO_HS_GEAR      0x04
#define UFS_DEFAULT_NO_HS_PWR_MODE  0x22

//
// UFS Termination Values
//
#define UFS_PHY_HS_TERMINATION     0x01
#define UFS_PHY_NO_HS_TERMINATION  0x00

//
// HCI Register Offsets
//
#define UFS_HC_HW_VER_OFFSET           0x00E4
#define UFS_HC_CFG0_OFFSET             0x00D8
#define UFS_HC_CFG1_OFFSET             0x00DC
#define UFS_HC_SYS1CLK_1US             0x00C0
#define UFS_MEM_MPHY_START_OFFSET      0x400
#define UFS_MEM_MPHY_PCS_READY_OFFSET  0x5A8

//
// HCI Register Masks
//
#define UFS_HC_IS_UPMS                  BIT4
#define UFS_HC_IS_MASK_ALL_ERROR        (BIT2 | BIT11 | BIT12 | BIT16 | BIT17 | BIT18)
#define UFS_HC_CFG0_QUNIPRO_G4_SEL      BIT5
#define UFS_HC_CFG1_PHY_SOFT_RESET      BIT1
#define UFS_HC_CFG1_DEV_REFCLK_ENABLED  BIT26
#define UFS_MEM_MPHY_START              BIT0
#define UFS_MEM_MPHY_PCS_READY          BIT0

//
// Phy Configuration Helper Macros
//
#define UFS_PHY_CONFIG_STOP_CONDITION  0x8000
#define UFS_RATE_SHIFT                 0x0
#define UFS_RATE_MASK                  0x00000003
#define UFS_GEAR_SHIFT                 0x2
#define UFS_GEAR_MASK                  0x0000000C

/**
  Build the PHY conditional flag word from rate and gear.

  @param[in]  Rate  UFS rate value.
  @param[in]  Gear  UFS gear value.

  @return  UINT32 PHY conditional flag with rate and gear fields set.
**/
#define UFS_PHY_CONDITIONAL_FLAG(Rate, Gear)  (((Rate) << UFS_RATE_SHIFT) |   \
                                              ((Gear) << UFS_GEAR_SHIFT))

/**
  Compute the GenSelect lane index for an RX lane.

  The UniPro GenSelect register encodes TX and RX lanes in a single field.
  Per the JEDEC UFS / MIPI UniPro specification the lane index layout is:
    Bits [3:0]  TX lanes: TX lane N -> index N   (GEN_SELECT_TX)
    Bits [7:4]  RX lanes: RX lane N -> index N+4 (GEN_SELECT_RX)
  Hence RX lane 0 maps to index 4, RX lane 1 to index 5, etc.

  @param[in]  Ln  Lane number (0-based).

  @return  RX GenSelect index for the given lane (Ln + 4).
**/
#define GEN_SELECT_RX(Ln)  ((Ln) + 4)

/**
  Compute the GenSelect lane index for a TX lane.

  TX lanes occupy the lower nibble of the GenSelect field (indices 0-3),
  so the TX index equals the lane number directly. See GEN_SELECT_RX for
  the full register layout description.

  @param[in]  Ln  Lane number (0-based).

  @return  TX GenSelect index for the given lane (Ln, identity mapping).
**/
#define GEN_SELECT_TX(Ln)  (Ln)

/**
  Compute the PA power mode user data MIB attribute address for a lane.

  @param[in]  x  Lane offset.

  @return  MIB attribute address for PA power mode user data.
**/
#define PA_PWR_MODE_USER_DATA(x)  (0x15B0 + (x))

//
// UFS Inquiry Response Helper Macros
//
#define UFS_INQUIRY_VENDOR_ID_BYTES   8
#define UFS_INQUIRY_PRODUCT_ID_BYTES  16
#define UFS_INQUIRY_REV_LEVEL_BYTES   4

///
/// Minimum INQUIRY response length needed to safely read Vendor (offset 8, 8 bytes),
/// Product (offset 16, 16 bytes), and Revision (offset 32, 4 bytes):
///   8 (vendor offset) + 8 (vendor) + 16 (product) + 4 (revision) = 36
///
#define UFS_INQUIRY_MIN_RESPONSE_LEN \
  (8 + UFS_INQUIRY_VENDOR_ID_BYTES + UFS_INQUIRY_PRODUCT_ID_BYTES + UFS_INQUIRY_REV_LEVEL_BYTES)

/**
  Tunes UniPro PA_TActivate and PA_Hibern8Time parameters for optimal UFS operation.

  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           UniPro parameters tuned successfully.
  @retval EFI_INVALID_PARAMETER HcDriverInterface is NULL.
  @retval EFI_NOT_STARTED       Required interface not available.
  @retval EFI_DEVICE_ERROR      Failed to configure UniPro parameters.

**/
EFI_STATUS
EFIAPI
UfsUtpTuneUniproParams (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  );

/**
  Configures the active TX and RX data lane count for the UFS link.

  @param[in,out] ControllerData     ActiveConfig.NumLanes is updated with the
                                    negotiated lane count.
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
  );

/**
  Configures the PA-layer TX and RX gear for the UFS link.

  @param[in] ControllerData     Pointer to controller data with target gear.
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
  );

/**
  Configures PA-layer TX and RX termination for the UFS link.

  @param[in] ControllerData     Pointer to controller data with HS enable flag.
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
  );

/**
  Configures the PA-layer HS series (rate A or B) for the UFS link.

  @param[in] ControllerData     Pointer to controller data with target rate.
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
  );

/**
  Configures HS adaptation settings for the UFS link.

  @param[in] ControllerData     Pointer to controller data with adapt enable
                                flag and gear setting.
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
  );

/**
  Triggers a UFS PA-layer power mode change and waits for completion.

  @param[in] ControllerData     Pointer to controller data with HS enable flag,
                                UMPS poll timeout, and HCI base address.
  @param[in] HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Power mode change completed successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       Required interfaces are not available.
  @retval EFI_TIMEOUT           Timed out waiting for IS.UPMS to assert.
  @retval EFI_DEVICE_ERROR      Power mode change result indicated an error.

**/
EFI_STATUS
EFIAPI
UfsUtpConfigurePowerMode (
  IN UFS_CONTROLLER_DATA            *ControllerData,
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  );

/**
  Performs a full UTP layer reconfiguration after link startup.

  @param[in,out] ControllerData     ActiveConfig is updated with the reloaded
                                    configuration.
  @param[in]     HcDriverInterface  Pointer to the UFS HC driver interface.

  @retval EFI_SUCCESS           Reconfiguration completed successfully.
  @retval EFI_INVALID_PARAMETER One or more input parameters are NULL.
  @retval EFI_NOT_STARTED       Required interfaces are not available.
  @retval EFI_DEVICE_ERROR      One or more configuration steps failed.

**/
EFI_STATUS
EFIAPI
UfsUtpReconfigure (
  IN OUT UFS_CONTROLLER_DATA            *ControllerData,
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *HcDriverInterface
  );

/**
  Retrieves UFS host capabilities (gear, lanes, rate, UniPro version).

  @param[in,out] ControllerData     HostCaps is populated on first call.
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
  );

/**
  Configures UFS controller clock control registers and UniPro clock attributes.

  @param[in] ControllerData     Pointer to controller data with PM timing config.
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
  );

/**
  Polls the TX FSM state until the UFS link enters hibernate.

  @param[in] ControllerData     Pointer to controller data with hibernate timeout.
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
  );

/**
  Disables the Local Link Control (LLC) on both host and device sides.

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
  );

/**
  Applies TX de-emphasis settings to all active UFS lanes.

  @param[in] ControllerData     Pointer to controller data with active lane count.
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
  );
