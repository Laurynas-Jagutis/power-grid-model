// SPDX-FileCopyrightText: 2022 Contributors to the Power Grid Model project <dynamic.grid.calculation@alliander.com>
//
// SPDX-License-Identifier: MPL-2.0

#include "power_grid_model_c.hpp"



// create and destory handle
POWER_GRID_MODEL_Handle* POWER_GRID_MODEL_create_handle() {
    return new POWER_GRID_MODEL_Handle{};
}
void POWER_GRID_MODEL_destroy_handle(POWER_GRID_MODEL_Handle* handle) {
    delete handle;
}

// error handling
POWER_GRID_MODEL_Idx POWER_GRID_MODEL_err_code(POWER_GRID_MODEL_Handle const* handle) {
    return handle->err_code;
}
char const* POWER_GRID_MODEL_err_msg(POWER_GRID_MODEL_Handle const* handle) {
    return handle->err_msg.c_str();
}

// construct and destroy model
