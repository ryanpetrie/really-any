#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "really/any.hpp"
#include <iostream>

using namespace really;

struct operation_counter
{
	static inline int instances = 0;
	static inline int destructed = 0;
	static inline int default_constructed = 0;
	static inline int copy_constructed = 0;
	static inline int move_constructed = 0;
	static inline int copy_assigned = 0;
	static inline int move_assigned = 0;

	static void reset()
	{
		instances = 0;
		destructed = 0;
		default_constructed = 0;
		copy_constructed = 0;
		move_constructed = 0;
		copy_assigned = 0;
		move_assigned = 0;
	}

	operation_counter()
	{
		++instances;
		++default_constructed;
	}
	~operation_counter()
	{
		++destructed;
		--instances;
	}

	operation_counter(const operation_counter&)
	{
		++instances;
		++copy_constructed;
	}
	operation_counter(operation_counter&&) noexcept
	{
		++instances;
		++move_constructed;
	}

	operation_counter& operator=(const operation_counter&)
	{
		++copy_assigned;
		return *this;
	}
	operation_counter& operator=(operation_counter&&) noexcept
	{
		++move_assigned;
		return *this;
	}
};

TEST_SUITE_BEGIN("any");

TEST_CASE_TEMPLATE("basic-usage", any_t, copyable_any, movable_any)
{
	any_t a;
	CHECK(!a.has_value());

	a = 5;
	CHECK(a.has_value());
	CHECK(a.has_type<int>());
	CHECK(a.try_get_value<int>() != nullptr);
	CHECK(a.try_get_value<char>() == nullptr);
	CHECK(a.value<int>() == 5);

	a.reset();
	CHECK(!a.has_value());
	CHECK(!a.has_type<int>());
	CHECK(a.try_get_value<int>() == nullptr);
}

TEST_CASE("nonmovable-any")
{
	nonmovable_any a;
	CHECK(!a.has_value());

	a.emplace<int>(5);
	CHECK(a.has_value());
	CHECK(a.has_type<int>());

	a.emplace<char>('a');
	CHECK(a.has_value());
	CHECK(a.has_type<char>());

	a.reset();
	CHECK(!a.has_value());
}

TEST_CASE_TEMPLATE("any-type-handling", any_t, copyable_any, movable_any,
				   any_of_size<sizeof(void*)>)
{
	operation_counter::reset();

	any_t a;

	// default construction
	a.emplace<operation_counter>();
	CHECK(operation_counter::instances == 1);
	CHECK(operation_counter::default_constructed == 1);

	// move assignment
	a = operation_counter{};
	CHECK(operation_counter::instances == 1);
	CHECK(operation_counter::copy_assigned == 0);
	CHECK(operation_counter::move_assigned == 1);

	// copy assignment
	if constexpr (any_t::copy_support == any_copy_support::copy_and_move)
	{
		operation_counter oc;
		a = oc;
		CHECK(operation_counter::copy_assigned == 1);
	}

	// reset
	a.reset();
	CHECK(operation_counter::instances == 0);

	// move construction
	operation_counter::reset();
	a = operation_counter{};
	CHECK(operation_counter::instances == 1);
	CHECK(operation_counter::move_constructed == 1);

	// copy construction
	if constexpr (any_t::copy_support == any_copy_support::copy_and_move)
	{
		operation_counter::reset();
		operation_counter oc;
		a.emplace<operation_counter>(oc);
		CHECK(operation_counter::copy_constructed == 1);
	}
}

TEST_SUITE_END();
