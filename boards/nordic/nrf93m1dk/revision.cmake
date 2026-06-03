# Copyright (c) 2025 Nordic Semiconductor ASA
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause


set(BOARD_REVISIONS "v0.1" "v0.2" "v0.3")
if(NOT DEFINED BOARD_REVISION)
        set(BOARD_REVISION "v0.3")
else()
        if(NOT BOARD_REVISION IN_LIST BOARD_REVISIONS)
                message(FATAL_ERROR "${BOARD_REVISION} is not a valid revision for nrf93m1dk. Accepted revisions: ${BOARD_REVISIONS}")
        endif()
endif()
