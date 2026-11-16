cmake_minimum_required(VERSION 3.16)
project(dccp VERSION 7.0.0 LANGUAGES C)

include(GNUInstallDirs)

option(WITH_MODULES "Build and install DCCP kernel modules via Kbuild" ON)
option(WITH_TESTS "Build userspace dccp_test utility" ON)
option(INSTALL_DKMS "Install DKMS source tree into <prefix>/src/dccp-${PROJECT_VERSION}" OFF)
set(KERNEL_UNAME "" CACHE STRING "Target kernel version (defaults to running kernel uname -r)")

# Userspace UAPI header
install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/modules/include/uapi/linux/dccp.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/linux"
)

# Optional userspace test utility
if(WITH_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/dccp_test.c")
    add_executable(dccp_test "${CMAKE_CURRENT_SOURCE_DIR}/test/dccp_test.c")
    target_include_directories(dccp_test BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/include/uapi"
    )
endif()

# Kernel modules via Kbuild
if(WITH_MODULES)
    if(KERNEL_UNAME STREQUAL "")
        execute_process(
            COMMAND uname -r
            OUTPUT_VARIABLE KERNEL_UNAME
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE UNAME_RES
        )
        if(NOT UNAME_RES EQUAL 0 OR KERNEL_UNAME STREQUAL "")
            message(FATAL_ERROR "Failed to determine running kernel version via uname -r")
        endif()
    endif()

    set(KERNEL_LIB_DIR "/lib/modules/${KERNEL_UNAME}")
    if(EXISTS "${KERNEL_LIB_DIR}/build/Makefile")
        set(KERNEL_BUILD_DIR "${KERNEL_LIB_DIR}/build")
    elseif(EXISTS "/usr/src/kernels/${KERNEL_UNAME}/Makefile")
        set(KERNEL_BUILD_DIR "/usr/src/kernels/${KERNEL_UNAME}")
    else()
        message(FATAL_ERROR "Unable to find kernel headers/build directory for ${KERNEL_UNAME}")
    endif()

    set(KERNEL_EXTRA_DIR "${KERNEL_LIB_DIR}/extra")

    set(DCCP_KBUILD_ARGS
        V=1
        -C "${KERNEL_BUILD_DIR}"
        "M=${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp"
        "ROOTDIR=${CMAKE_CURRENT_SOURCE_DIR}/modules"
        CONFIG_IP_DCCP=m
        CONFIG_INET_DCCP_DIAG=m
        CONFIG_IP_DCCP_CCID2_DEBUG=n
        CONFIG_IP_DCCP_CCID3=m
        CONFIG_IP_DCCP_CCID3_DEBUG=n
        CONFIG_IP_DCCP_TFRC_LIB=m
        CONFIG_IP_DCCP_TFRC_DEBUG=n
    )

    add_custom_target(dccp_modules ALL
        COMMAND "${CMAKE_MAKE_PROGRAM}" ${DCCP_KBUILD_ARGS} modules
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building DCCP kernel modules for ${KERNEL_UNAME}"
    )

    add_custom_target(clean_modules
        COMMAND "${CMAKE_MAKE_PROGRAM}" -C "${KERNEL_BUILD_DIR}"
                "M=${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp" clean
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Cleaning DCCP kernel module artifacts"
    )

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake"
"set(DCCP_MODULES dccp_diag dccp_ipv6 dccp_ipv4 dccp)
foreach(mod IN LISTS DCCP_MODULES)
    if(\"\$ENV{DESTDIR}\" STREQUAL \"\" AND EXISTS \"/sys/module/\${mod}\")
        execute_process(COMMAND rmmod \${mod})
    endif()
    file(REMOVE \"\$ENV{DESTDIR}\${KERNEL_EXTRA_DIR}/\${mod}.ko\")
endforeach()
if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
    execute_process(COMMAND depmod -a \"\${KERNEL_UNAME}\")
endif()
")

    add_custom_target(uninstall_modules
        COMMAND "${CMAKE_COMMAND}"
            "-DKERNEL_UNAME=${KERNEL_UNAME}"
            "-DKERNEL_EXTRA_DIR=${KERNEL_EXTRA_DIR}"
            -P "${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake"
        COMMENT "Unloading and removing installed DCCP kernel modules"
    )

    install(CODE "
        set(KERNEL_UNAME \"${KERNEL_UNAME}\")
        set(KERNEL_EXTRA_DIR \"${KERNEL_EXTRA_DIR}\")
        include(\"${CMAKE_CURRENT_BINARY_DIR}/uninstall_modules.cmake\")
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${KERNEL_EXTRA_DIR}\")
        foreach(mod dccp_diag dccp_ipv6 dccp_ipv4 dccp)
            set(ko \"${CMAKE_CURRENT_SOURCE_DIR}/modules/net/dccp/\${mod}.ko\")
            if(EXISTS \"\${ko}\")
                file(INSTALL DESTINATION \"\$ENV{DESTDIR}${KERNEL_EXTRA_DIR}\"
                     TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
                     FILES \"\${ko}\")
            endif()
        endforeach()
        if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
            execute_process(COMMAND depmod -a \"${KERNEL_UNAME}\")
        endif()
    ")
endif()

# Optional DKMS source tree staging (for distro packaging)
if(INSTALL_DKMS)
    set(DKMS_SRC_DEST "${CMAKE_INSTALL_PREFIX}/src/dccp-${PROJECT_VERSION}")
    install(FILES dkms.conf dccp-module-config.h COPYING DESTINATION "${DKMS_SRC_DEST}")
    install(DIRECTORY modules DESTINATION "${DKMS_SRC_DEST}"
        PATTERN "*.o" EXCLUDE
        PATTERN "*.ko" EXCLUDE
        PATTERN "*.mod" EXCLUDE
        PATTERN "*.mod.c" EXCLUDE
        PATTERN "*.cmd" EXCLUDE
        PATTERN "Module.symvers" EXCLUDE
        PATTERN "modules.order" EXCLUDE
        PATTERN ".tmp_versions" EXCLUDE
    )
endif()

# Source archive generation
set(CPACK_PACKAGE_NAME "dccp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_SOURCE_GENERATOR "TXZ;TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/build/"
    "/\\\\.git/"
    ".*~$"
    "\\\\.o$"
    "\\\\.ko$"
    "\\\\.mod$"
    "\\\\.mod\\\\.c$"
    "\\\\.cmd$"
    "Module\\\\.symvers$"
    "modules\\\\.order$"
    "/\\\\.tmp_versions/"
)
include(CPack)
