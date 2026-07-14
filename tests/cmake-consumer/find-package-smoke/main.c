/* SPDX-License-Identifier: Apache-2.0
 *
 * Prints the linked alp-sdk's runtime version so the CI job (or a
 * developer running the recipe in this directory's CMakeLists.txt
 * comment) can assert the installed package resolves to the expected
 * release -- see issue #607.
 */
#include <stdio.h>

#include <alp/version.h>

int main(void)
{
	printf("alp-sdk version: %s\n", alp_version_string());
	return 0;
}
