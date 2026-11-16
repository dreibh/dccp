# ==========================================================================
#              _     _                    ____   ____ ____ ____
#             | |   (_)_ __  _   ___  __ |  _ \ / ___/ ___|  _ \
#             | |   | | '_ \| | | \ \/ / | | | | |  | |   | |_) |
#             | |___| | | | | |_| |>  <  | |_| | |__| |___|  __/
#             |_____|_|_| |_|\__,_/_/\_\ |____/ \____\____|_|
#
#             Linux DCCP Kernel Module Build for Kernels >= 6.19
#                   Copyright (C) 2026 by Thomas Dreibholz
# ==========================================================================
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Contact:  dreibh@simula.no
# Homepage: https://github.com/dreibh/dccp

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
SET(BUILD_MAJOR "7")
SET(BUILD_MINOR "0")
SET(BUILD_PATCH "0")
STRING(REGEX MATCH "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?" CMAKE_COMPAT_VERSION "${BUILD_MAJOR}.${BUILD_MINOR}.${BUILD_PATCH}")
PROJECT(dccp
        VERSION      "${CMAKE_COMPAT_VERSION}"
        LANGUAGES    C
        HOMEPAGE_URL "https://github.com/dreibh/dccp")
SET(PROJECT_SOURCE_URL     "https://github.com/dreibh/dccp")
SET(PROJECT_BUGTRACKER_URL "https://github.com/dreibh/dccp/issues")


#############################################################################
#### CMAKE INCLUDES                                                      ####
#############################################################################

INCLUDE(GNUInstallDirs)


#############################################################################
#### PACKAGING                                                           ####
#############################################################################

SET(CPACK_SOURCE_GENERATOR "TXZ")
SET(CPACK_SOURCE_PACKAGE_FILE_NAME
    "${CMAKE_PROJECT_NAME}-${BUILD_MAJOR}.${BUILD_MINOR}.${BUILD_PATCH}")
SET(CPACK_SOURCE_IGNORE_FILES "\\\\.git;\\\\.swp$;~$;\\\\.\\\\#;/\\\\#")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "^${PROJECT_SOURCE_DIR}/${CMAKE_PROJECT_NAME}[_-][0-9]")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "\\\\.make$|\\\\.log$")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "/CMakeCache\\\\.txt$")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "/(CMakeFiles|CMakeScripts|_CPack_Packages)/")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "/package-version\\\\.h$")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "/packaging\\\\.conf$")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "^${PROJECT_SOURCE_DIR}/(po.*/|src.*/|)Makefile$")
LIST(APPEND CPACK_SOURCE_IGNORE_FILES "^${PROJECT_SOURCE_DIR}/(po.*|src.*|[^/]*)\\\\.cmake$")
INCLUDE(CPack)

ADD_CUSTOM_TARGET(dist COMMAND ${CMAKE_MAKE_PROGRAM} clean package_source)


#############################################################################
#### UNINSTALL                                                           ####
#############################################################################

IF (NOT TARGET uninstall)
   FILE(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
"IF (EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt\")
   FILE(READ \"${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt\" files)
   STRING(REGEX REPLACE \"\\r?\\n\" \";\" files \"\${files}\")
   FOREACH(file IN LISTS files)
      IF (NOT \"\${file}\" STREQUAL \"\")
         MESSAGE(STATUS \"Uninstalling \$ENV{DESTDIR}\${file}\")
         FILE(REMOVE \"\$ENV{DESTDIR}\${file}\")
      ENDIF()
   ENDFOREACH()
ENDIF()
")

   ADD_CUSTOM_TARGET(uninstall
      COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
      COMMENT "Uninstalling DCCP files")
ENDIF()


#############################################################################
#### OS-DEPENDENT                                                        ####
#############################################################################

IF (CMAKE_SYSTEM_NAME MATCHES "Linux")
   MESSAGE(STATUS "${CMAKE_SYSTEM_NAME} supported")
ELSE()
   MESSAGE(FATAL_ERROR "${CMAKE_SYSTEM_NAME} not supported!")
ENDIF()


#############################################################################
#### OPTIONS                                                             ####
#############################################################################

OPTION(WITH_MODULES "Build and install DCCP kernel modules via Kbuild"                   ON)
OPTION(WITH_TESTS   "Build userspace dccp_test utility"                                  ON)
OPTION(INSTALL_DKMS "Install DKMS source tree into <prefix>/src/dccp-${PROJECT_VERSION}" OFF)
SET(KERNEL_UNAME "" CACHE STRING "Target kernel version (defaults to running kernel uname -r)")


#############################################################################
#### UAPI HEADERS & USERSPACE TESTS                                      ####
#############################################################################

# ====== UAPI Header ========================================================
INSTALL(
   FILES       "${CMAKE_CURRENT_SOURCE_DIR}/modules/include/uapi/linux/dccp.h"
   DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/linux"
)

# ====== Userspace Test Utility =============================================
IF (WITH_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/dccp_test.c")
   ADD_EXECUTABLE(dccp_test "${CMAKE_CURRENT_SOURCE_DIR}/test/dccp_test.c")
   TARGET_INCLUDE_DIRECTORIES(dccp_test BEFORE PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/modules/include/uapi"
   )
ENDIF()


#############################################################################
#### KERNEL MODULES                                                      ####
#############################################################################

IF (WITH_MODULES)
   # ====== Target Kernel Detection =========================================
   IF (KERNEL_UNAME STREQUAL "")
      EXECUTE_PROCESS(
         COMMAND uname -r
         OUTPUT_VARIABLE KERNEL_UNAME
         OUTPUT_STRIP_TRAILING_WHITESPACE
         RESULT_VARIABLE UNAME_RES
      )
      IF (NOT UNAME_RES EQUAL 0 OR KERNEL_UNAME STREQUAL "")
         MESSAGE(FATAL_ERROR "Failed to determine running kernel version via uname -r")
      ENDIF()
   ENDIF()

   SET(KERNEL_LIB_DIR "/lib/modules/${KERNEL_UNAME}")
   IF (EXISTS "${KERNEL_LIB_DIR}/build/Makefile")
      SET(KERNEL_BUILD_DIR "${KERNEL_LIB_DIR}/build")
   ELSEIF (EXISTS "/usr/src/kernels/${KERNEL_UNAME}/Makefile")
      SET(KERNEL_BUILD_DIR "/usr/src/kernels/${KERNEL_UNAME}")
   ELSE()
      MESSAGE(FATAL_ERROR "Unable to find kernel headers/build directory for ${KERNEL_UNAME}")
   ENDIF()

   SET(KERNEL_EXTRA_DIR "${KERNEL_LIB_DIR}/extra")

   MESSAGE(STATUS "Kernel configuration:")
   MESSAGE(STATUS " KERNEL_UNAME:     ${KERNEL_UNAME}")
   MESSAGE(STATUS " KERNEL_BUILD_DIR: ${KERNEL_BUILD_DIR}")
   MESSAGE(STATUS " KERNEL_EXTRA_DIR: ${KERNEL_EXTRA_DIR}")

   # ====== Kbuild Targets ==================================================
   SET(DCCP_KBUILD_ARGS
      V=1
      -C "${KERNEL_BUILD_DIR}"
      "M=${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp"
      "ROOTDIR=${CMAKE_CURRENT_SOURCE_DIR}/modules"
      CONFIG_IP_DCCP=m
      CONFIG_INET_DCCP_DIAG=m
      CONFIG_IP_DCCP_CCID2_DEBUG=n
      CONFIG_IP_DCCP_CCID3=y
      CONFIG_IP_DCCP_CCID3_DEBUG=n
      CONFIG_IP_DCCP_TFRC_LIB=y
      CONFIG_IP_DCCP_TFRC_DEBUG=n
   )

   ADD_CUSTOM_TARGET(dccp_modules ALL
      COMMAND "${CMAKE_MAKE_PROGRAM}" ${DCCP_KBUILD_ARGS} modules
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Building DCCP kernel modules for ${KERNEL_UNAME}"
   )

   ADD_CUSTOM_TARGET(clean_modules
      COMMAND "${CMAKE_MAKE_PROGRAM}" -C "${KERNEL_BUILD_DIR}"
         "M=${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp" clean
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Cleaning DCCP kernel module artifacts"
   )

   # ====== Module Installation & Uninstallation ============================
   FILE(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake"
"SET(DCCP_MODULES dccp_diag dccp_ipv6 dccp_ipv4 dccp)
FOREACH(mod IN LISTS DCCP_MODULES)
   IF (\"\$ENV{DESTDIR}\" STREQUAL \"\" AND EXISTS \"/sys/module/\${mod}\")
      EXECUTE_PROCESS(COMMAND rmmod \${mod})
   ENDIF()
   FILE(REMOVE \"\$ENV{DESTDIR}\${KERNEL_EXTRA_DIR}/\${mod}.ko\")
ENDFOREACH()
IF (\"\$ENV{DESTDIR}\" STREQUAL \"\")
   EXECUTE_PROCESS(COMMAND depmod -a \"\${KERNEL_UNAME}\")
ENDIF()
")

   ADD_CUSTOM_TARGET(uninstall_modules
      COMMAND "${CMAKE_COMMAND}"
         "-DKERNEL_UNAME=${KERNEL_UNAME}"
         "-DKERNEL_EXTRA_DIR=${KERNEL_EXTRA_DIR}"
         -P "${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake"
      COMMENT "Unloading and removing installed DCCP kernel modules"
   )

   ADD_DEPENDENCIES(uninstall uninstall_modules)

   INSTALL(CODE "
      SET(KERNEL_UNAME \"${KERNEL_UNAME}\")
      SET(KERNEL_EXTRA_DIR \"${KERNEL_EXTRA_DIR}\")
      INCLUDE(\"${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake\")
      FILE(MAKE_DIRECTORY \"\$ENV{DESTDIR}${KERNEL_EXTRA_DIR}\")
      FOREACH(mod dccp_diag dccp_ipv6 dccp_ipv4 dccp)
         SET(ko \"${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp/\${mod}.ko\")
         IF (EXISTS \"\${ko}\")
            FILE(INSTALL DESTINATION \"\$ENV{DESTDIR}${KERNEL_EXTRA_DIR}\"
               TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
               FILES \"\${ko}\")
         ENDIF()
      ENDFOREACH()
      IF (\"\$ENV{DESTDIR}\" STREQUAL \"\")
         EXECUTE_PROCESS(COMMAND depmod -a \"${KERNEL_UNAME}\")
      ENDIF()
   ")
ENDIF()


#############################################################################
#### DKMS SOURCE STAGING                                                 ####
#############################################################################

IF (INSTALL_DKMS)
   SET(DKMS_SRC_DEST "${CMAKE_INSTALL_PREFIX}/src/dccp-${PROJECT_VERSION}")
   INSTALL(FILES dkms.conf dccp-module-config.h COPYING DESTINATION "${DKMS_SRC_DEST}")
   INSTALL(DIRECTORY modules DESTINATION "${DKMS_SRC_DEST}"
      PATTERN "*.o"            EXCLUDE
      PATTERN "*.ko"           EXCLUDE
      PATTERN "*.mod"          EXCLUDE
      PATTERN "*.mod.c"        EXCLUDE
      PATTERN "*.cmd"          EXCLUDE
      PATTERN "Module.symvers" EXCLUDE
      PATTERN "modules.order"  EXCLUDE
      PATTERN ".tmp_versions"  EXCLUDE
   )
ENDIF()
