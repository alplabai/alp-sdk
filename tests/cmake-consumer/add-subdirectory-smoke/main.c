/* SPDX-License-Identifier: Apache-2.0
 *
 * Issue #598 regression fixture: proves an add_subdirectory() consumer
 * both configures/builds AND links against the real alp_sdk static
 * archive (not just that headers resolve) -- alp_last_error() is a
 * plain <alp/peripheral.h> symbol every backend provides.
 */
#include <alp/peripheral.h>

int main(void)
{
	return (int)alp_last_error();
}
