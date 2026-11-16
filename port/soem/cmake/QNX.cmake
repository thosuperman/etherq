# This software is dual-licensed under GPLv3 and a commercial
# license. See the file LICENSE.md distributed with this software for
# full license information.
#
# QNX OS 8.0 platform file, included by SOEM's top-level CMakeLists.txt as
# cmake/${CMAKE_SYSTEM_NAME}.cmake. QNX OS 8.0 port: Copyright (C) 2026 EtherQ
# contributors (GPL-3.0-or-later).

target_sources(soem PRIVATE
  osal/qnx/osal.c
  osal/qnx/osal_defs.h
  oshw/qnx/oshw.c
  oshw/qnx/oshw.h
  oshw/qnx/nicdrv.c
  oshw/qnx/nicdrv.h
)

target_include_directories(soem PUBLIC
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/osal/qnx>
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/oshw/qnx>
  $<INSTALL_INTERFACE:include/soem>
)

# QNX: POSIX extensions (clock_nanosleep, if_nameindex, ...) are visible by default
foreach(target IN ITEMS
    soem
    ec_sample
    eepromtool
    eni_test
    firm_update
    simple_ng
    slaveinfo)
  if (TARGET ${target})
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
    )
  endif()
endforeach()

# libsocket provides the BSD socket/ifnet API (if_nameindex) on QNX
target_link_libraries(soem PUBLIC socket)

install(FILES
  osal/qnx/osal_defs.h
  oshw/qnx/nicdrv.h
  DESTINATION include/soem
)
