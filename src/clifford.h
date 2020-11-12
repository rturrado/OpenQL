/**
 * @file   clifford.h
 * @date   05/2019
 * @author Hans van Someren
 * @brief  clifford sequence optimizer
 */

#pragma once

#include "utils/str.h"
#include "program.h"
#include "platform.h"

namespace ql {

/**
 * Clifford sequence optimizer.
 */
void clifford_optimize(
    quantum_program *programp,
    const quantum_platform &platform,
    const utils::Str &passname
);

} // namespace ql
