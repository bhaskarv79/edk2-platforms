## @file
#  Host-based unit tests of the Qualcomm UFS host controller driver.
#
#  QcomUfsHcDxeHostTest is built twice: with the device tree configuration
#  source, and PCD-only with the PreHce phase enabled, which the PCD
#  fallback must refuse.
#
#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = QcomUfsHostTest
  PLATFORM_GUID           = 11C24BF6-5225-4FB8-9BBE-537C842ED2E1
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/QcomUfsHostTest
  SUPPORTED_ARCHITECTURES = IA32|X64|AARCH64
  BUILD_TARGETS           = NOOPT
  SKUID_IDENTIFIER        = DEFAULT

!include UnitTestFrameworkPkg/UnitTestFrameworkPkgHost.dsc.inc

[PcdsFixedAtBuild]
  gQualcommCommonSiliconPkgTokenSpaceGuid.PcdUfs0MemBaseAddress|0x1D84000
  gQualcommCommonSiliconPkgTokenSpaceGuid.PcdUfs0HciOffset|0x0

[Components]
  Silicon/Qualcomm/Drivers/QcomUfsHcDxe/UnitTest/QcomUfsHcDxeHostTest.inf {
    <PcdsFixedAtBuild>
      gQualcommCommonSiliconPkgTokenSpaceGuid.PcdUfsConfigSourceDeviceTree|1
  }

  Silicon/Qualcomm/Drivers/QcomUfsHcDxe/UnitTest/QcomUfsHcDxeHostTest.inf {
    <Defines>
      FILE_GUID = F24DFB2A-838F-41A1-8E26-068883281670
    <PcdsFixedAtBuild>
      gQualcommCommonSiliconPkgTokenSpaceGuid.PcdUfsConfigSourceDeviceTree|0
      gQualcommCommonSiliconPkgTokenSpaceGuid.PcdUfs0PreHceCallbackEn|TRUE
  }
