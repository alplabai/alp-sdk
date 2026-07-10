/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * etl-fixed-containers -- teaches the Embedded Template Library
 * (https://github.com/ETLCPP/etl)'s fixed-capacity `etl::vector` and
 * `etl::map`: STL-like containers with a compile-time capacity and
 * NO heap allocation, ever.
 *
 * What success looks like:
 *
 *   [etl-fixed-containers] vector: size=5 capacity=8
 *   [etl-fixed-containers] vector: sum=15
 *   [etl-fixed-containers] map: size=3
 *   [etl-fixed-containers] map: lookup("b") = 2
 *   [etl-fixed-containers] done
 *
 * Where this runs: native_sim.  `board.yaml`'s `libraries: [etl]`
 * adds vendors/etl/include (the vendored ETL headers) and
 * metadata/library-profiles/etl/etl_profile.h to the include path.
 * This example's prj.conf deliberately does NOT set
 * CONFIG_REQUIRES_FULL_LIBCPP, so etl_profile.h's guard leaves
 * ETL_NO_STL on -- the same standalone (no host STL) mode ETL runs
 * in on real Cortex-M silicon.  See prj.conf for why.
 *
 * The fixed-capacity / no-heap property:
 *
 *   `etl::vector<int, 8>` and `etl::map<const char *, int, 4>` both
 *   embed their storage inline (as class members), sized for their
 *   MAX_SIZE template parameter at COMPILE time.  There is no
 *   `new`/`malloc` anywhere in this file, and none inside ETL's
 *   vector/map implementation either -- push_back()/insert() past
 *   capacity calls etl_error_handler (ETL_NO_EXCEPTIONS in the
 *   profile routes it there instead of throwing std::length_error).
 *   That's what makes these containers safe on a heap-less firmware
 *   build: capacity violations are a deterministic fault, not a
 *   silent allocation that might fail at 3am in the field.
 */

/* Plain C <stdio.h>, not <cstdio> -- this example deliberately stays
 * out of ETL_NO_STL's way (see prj.conf): Zephyr's minimal C++
 * library (the default without CONFIG_REQUIRES_FULL_LIBCPP) has no
 * <cstdio>/std:: wrappers, only the C library headers the C++
 * compiler is still free to include. */
#include <stdio.h>

#include <etl/map.h>
#include <etl/vector.h>

int main(void)
{
	/* etl::vector<int, 8>: an STL-vector-shaped container whose
	 * storage is 8 `int`s embedded in the object itself -- no
	 * heap, no pointer indirection to a separate buffer. push_back()
	 * behaves like std::vector's until the 9th element, at which
	 * point it would hit ETL_NO_EXCEPTIONS's error handler instead
	 * of growing (fixed capacity means no growing). */
	etl::vector<int, 8> numbers;

	numbers.push_back(1);
	numbers.push_back(2);
	numbers.push_back(3);
	numbers.push_back(4);
	numbers.push_back(5);

	printf("[etl-fixed-containers] vector: size=%u capacity=%u\n",
	       static_cast<unsigned>(numbers.size()),
	       static_cast<unsigned>(numbers.capacity()));

	/* Range-based for works exactly like std::vector -- ETL's
	 * iterators satisfy the same contiguous-container contract. */
	int sum = 0;
	for (int value : numbers) {
		sum += value;
	}
	printf("[etl-fixed-containers] vector: sum=%d\n", sum);

	/* etl::map<TKey, TValue, MAX_SIZE>: a fixed-capacity associative
	 * container backed by an intrusive binary tree over a pool of
	 * MAX_SIZE pre-allocated nodes (no per-insert allocation).
	 * Lookup is the familiar O(log n) map semantics; capacity is
	 * capped at compile time just like the vector above. */
	etl::map<const char *, int, 4> scores;

	scores["a"] = 1;
	scores["b"] = 2;
	scores["c"] = 3;

	printf("[etl-fixed-containers] map: size=%u\n", static_cast<unsigned>(scores.size()));

	/* find() returns an iterator, mirroring std::map -- end() means
	 * "not found".  We already know "b" is present; this is the
	 * lookup pattern a real app would use for a key it doesn't
	 * control the presence of. */
	auto it = scores.find("b");
	if (it != scores.end()) {
		printf("[etl-fixed-containers] map: lookup(\"b\") = %d\n", it->second);
	} else {
		printf("[etl-fixed-containers] map: lookup(\"b\") = <not found>\n");
	}

	printf("[etl-fixed-containers] done\n");
	return 0;
}
