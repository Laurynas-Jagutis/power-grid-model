<!--
SPDX-FileCopyrightText: Contributors to the Power Grid Model project <powergridmodel@lfenergy.org>

SPDX-License-Identifier: MPL-2.0
-->

# Component Test Case: Symmetrical generator

Test case for validation of a symmetrical generator component for asymmetrical power flow calculations in pandapower.

- A symmetrical generator can be in open or closed state.
- It can be of 3 types: constant power, constant impedance and constant current.

The circuit diagram is as follows:

```txt
source_4--node_1--line_3--node_2--sym_gen_6    (const_power)
                          node_2--sym_gen_7    (status=0)
```

## Modelling incompatibility with pandapower

- Source impedance is set too low. Result of source component here should be ignored
- Only constant power implementation is possible in pandapower for asymmetrical calculations.
